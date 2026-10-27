/* See LICENSE file for copyright and license details.
 *
 * simplectl: talks to simpled, or changes unit links on disk. Also
 * poweroff, reboot and halt, by the name it is called as.
 *
 * miviex has no Unix sockets yet, so a request is a file: written to
 * RUNDIR/req/<pid>, announced with SIGUSR1, taken by simpled as
 * <pid>.wait, answered in place and renamed to <pid>.done. Only root may
 * do that. status, is-active and list need no request: simpled keeps
 * each unit's state in RUNDIR/units/<name> for anyone to read.
 */
#include <sys/reboot.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "util.h"
#include "config.h"
#include "unit.h"

#define TAIL 10

/* a unit's state, as simpled publishes it */
typedef struct {
	char load[16], state[16], sub[16], result[32], job[8];
	char path[PATH_MAX], desc[256];
	long long since;
	int main, ctl, status;
} State;

#define S(m) offsetof(State, m), sizeof(((State *)0)->m)
static const struct {
	char *key;
	size_t off, size;       /* size 0: a number */
} statekeys[] = {
	{ "load", S(load) }, { "state", S(state) }, { "sub", S(sub) },
	{ "result", S(result) }, { "job", S(job) }, { "path", S(path) },
	{ "desc", S(desc) }, { "since", offsetof(State, since), 0 },
	{ "main", offsetof(State, main), 0 }, { "ctl", offsetof(State, ctl), 0 },
	{ "status", offsetof(State, status), 0 },
};

static void
usage(void)
{
	die("usage: %s [-r root] [-f] [--no-block] command [unit...]\n"
	    "  start stop restart reload status is-active reset-failed unit...\n"
	    "  list daemon-reload poweroff reboot halt\n"
	    "  enable disable is-enabled mask unmask cat unit...\n"
	    "  get-default, set-default target", argv0);
}

/* "foo" means foo.service, as with systemctl */
static char *
unitname(const char *arg)
{
	static char buf[NAME_MAX + 1];

	if (unitkind(arg) >= 0)
		return (char *)arg;
	snprintf(buf, sizeof(buf), "%s.service", arg);
	if (unitkind(buf) < 0)
		die("%s: not a unit name", arg);
	return buf;
}

static pid_t
daemonpid(void)
{
	char path[PATH_MAX];
	FILE *fp;
	int pid = 1;

	if ((fp = fopen(rpath(path, sizeof(path), RUNDIR "/pid"), "re"))) {
		if (fscanf(fp, "%d", &pid) != 1)
			pid = 1;
		fclose(fp);
	}
	return pid;
}

/* the request's names, for onint to clean up */
static char rq[4][PATH_MAX + 8];

static void
onint(int sig)
{
	int i;

	(void)sig;
	for (i = 0; i < 4; i++)
		unlink(rq[i]);
	_exit(130);
}

/* Sends one request, prints the answer, returns its exit code. Waits
 * for jobs as long as they take, but not for a simpled that is gone. */
static int
ask(const char *req, int block)
{
	static const char *ext[] = { "", ".tmp", ".wait", ".done" };
	struct timespec ts = { 0, 10 * 1000000 };
	struct sigaction sa = { 0 };
	long long until = now() + CTLTIMEOUT;
	char buf[4096], *p;
	Buf out = { 0 };
	pid_t pid = daemonpid();
	ssize_t n;
	int fd, i;

	for (i = 0; i < 4; i++)
		snprintf(rq[i], sizeof(rq[i]), "%s%s/req/%d%s", root, RUNDIR, (int)getpid(), ext[i]);
	sa.sa_handler = onint;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);

	unlink(rq[1]);
	if ((fd = open(rq[1], O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600)) < 0) {
		if (errno == EACCES)
			die("%.*s: only root may do that", (int)strcspn(req, " \n"), req);
		die("cannot reach simpled: %s: %s", rq[1], strerror(errno));
	}
	if (write(fd, req, strlen(req)) != (ssize_t)strlen(req) || close(fd) < 0 ||
	    rename(rq[1], rq[0]) < 0) {
		unlink(rq[1]);
		die("%s:", rq[0]);
	}
	if (kill(pid, SIGUSR1) < 0) {
		unlink(rq[0]);
		die("signal simpled:");
	}
	for (i = 0; (fd = open(rq[3], O_RDONLY | O_CLOEXEC)) < 0; i++) {
		if ((!block && now() >= until) || (kill(pid, 0) < 0 && errno == ESRCH)) {
			onint(0);
			die("simpled does not answer");
		}
		if (i == 100)
			ts.tv_nsec = 100 * 1000000;
		nanosleep(&ts, NULL);
	}
	while ((n = read(fd, buf, sizeof(buf))) > 0)
		bufadd(&out, buf, n);
	close(fd);
	unlink(rq[3]);
	if (!out.s || !(p = strchr(out.s, '\n')))
		die("bad answer from simpled");
	fputs(p + 1, stdout);
	n = atoi(out.s);
	free(out.s);
	return n;
}

/* Reads what simpled says about name. A unit simpled has not loaded
 * (or a simpled not running) gets what its file says, inactive. */
static void
readstate(const char *name, State *st)
{
	char path[PATH_MAX], *line = NULL, *v;
	size_t cap = 0, i;
	FILE *fp;
	Conf c;
	int load;

	memset(st, 0, sizeof(*st));
	snprintf(path, sizeof(path), "%s%s/units/%s", root, RUNDIR, name);
	if ((fp = fopen(path, "re"))) {
		while (getline(&line, &cap, fp) > 0) {
			line[strcspn(line, "\n")] = '\0';
			if (!(v = strchr(line, '=')))
				continue;
			*v++ = '\0';
			for (i = 0; i < LEN(statekeys); i++) {
				if (strcmp(line, statekeys[i].key))
					continue;
				if (!statekeys[i].size && statekeys[i].off == offsetof(State, since))
					st->since = atoll(v);
				else if (!statekeys[i].size)
					*(int *)((char *)st + statekeys[i].off) = atoi(v);
				else
					snprintf((char *)st + statekeys[i].off, statekeys[i].size, "%s", v);
			}
		}
		free(line);
		fclose(fp);
		return;
	}
	strcpy(st->state, "inactive");
	strcpy(st->sub, "dead");
	load = unitfind(name, path, sizeof(path));
	strcpy(st->load, load == LOAD_OK ? "loaded" : load == LOAD_MASKED ? "masked" : "not-found");
	if (load != LOAD_OK)
		return;
	snprintf(st->path, sizeof(st->path), "%s", path + strlen(root));
	if (unitparse(&c, name, path) == 0 && c.desc)
		snprintf(st->desc, sizeof(st->desc), "%s", c.desc);
	conffree(&c);
}

static void
fmtdur(long long ms)
{
	long long s = ms / 1000;

	if (s >= 3600)
		printf("%lldh %lldmin", s / 3600, s / 60 % 60);
	else if (s >= 60)
		printf("%lldmin %llds", s / 60, s % 60);
	else if (s)
		printf("%llds", s);
	else
		printf("%lldms", ms);
}

/* 0 active, 3 not, 4 no such unit: as systemctl status */
static int
status(const char *name)
{
	State st;

	readstate(name, &st);
	printf("* %s - %s\n", name, *st.desc ? st.desc : name);
	printf("    Loaded: %s", st.load);
	if (*st.path)
		printf(" (%s)", st.path);
	printf("\n    Active: %s (%s)", st.state, st.sub);
	if (st.since) {
		printf(" for ");
		fmtdur(now() - st.since);
	}
	putchar('\n');
	if (*st.job)
		printf("       Job: %s\n", st.job);
	if (st.main)
		printf("  Main PID: %d\n", st.main);
	if (st.ctl)
		printf("   Control: %d\n", st.ctl);
	if (*st.result)
		printf("    Result: %s\n", st.result);
	if (st.status && WIFEXITED(st.status))
		printf(" Last exit: status %d\n", WEXITSTATUS(st.status));
	else if (st.status)
		printf(" Last exit: signal %d\n", WTERMSIG(st.status));
	if (!strcmp(st.state, "active"))
		return 0;
	return strcmp(st.load, "not-found") ? 3 : 4;
}

static int
isactive(const char *name)
{
	State st;

	readstate(name, &st);
	puts(st.state);
	return strcmp(st.state, "active") ? 3 : 0;
}

static int
cmpstr(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

static int
list(void)
{
	char dir[PATH_MAX];
	struct dirent *e;
	List l = { 0 };
	State st;
	size_t i;
	DIR *d;

	if (!(d = opendir(rpath(dir, sizeof(dir), RUNDIR "/units"))))
		die("%s:", dir);
	while ((e = readdir(d)))
		if (unitkind(e->d_name) >= 0)
			ladd(&l, e->d_name);
	closedir(d);
	if (l.n)
		qsort(l.v, l.n, sizeof(*l.v), cmpstr);
	printf("%-32s %-9s %-12s %-12s %s\n", "UNIT", "LOAD", "ACTIVE", "SUB", "DESCRIPTION");
	for (i = 0; i < l.n; i++) {
		readstate(l.v[i], &st);
		if (!strcmp(st.load, "loaded") || strcmp(st.state, "inactive"))
			printf("%-32s %-9s %-12s %-12s %s\n", l.v[i], st.load, st.state,
			       st.sub, *st.desc ? st.desc : l.v[i]);
	}
	lfree(&l);
	return 0;
}

static void
tail(const char *name)
{
	char path[PATH_MAX], *line = NULL, *lines[TAIL] = { 0 };
	size_t cap = 0, n = 0, i;
	FILE *fp;

	snprintf(path, sizeof(path), "%s%s/%s.log", root, LOGDIR, name);
	if (!(fp = fopen(path, "re")))
		return;
	while (getline(&line, &cap, fp) > 0) {
		free(lines[n % TAIL]);
		lines[n++ % TAIL] = estrdup(line);
	}
	fclose(fp);
	if (n)
		printf("\n%s:\n", path + strlen(root));
	for (i = n > TAIL ? n - TAIL : 0; i < n; i++)
		fputs(lines[i % TAIL], stdout);
	for (i = 0; i < TAIL; i++)
		free(lines[i]);
	free(line);
}

static char *
etcpath(char *buf, size_t size, const char *name)
{
	snprintf(buf, size, "%s%s/%s", root, unitpath[0], name);
	return buf;
}

static void
mkdirs(const char *path)
{
	char buf[PATH_MAX], *p;

	snprintf(buf, sizeof(buf), "%s", path);
	for (p = buf + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		mkdir(buf, 0755);
		*p = '/';
	}
	if (mkdir(buf, 0755) < 0 && errno != EEXIST)
		die("mkdir %s:", buf);
}

/* calls fn for every <x>.wants and <x>.requires directory in /etc */
static int
eachdir(const char *name, int (*fn)(const char *, const char *))
{
	char dir[PATH_MAX], sub[PATH_MAX + NAME_MAX + 2];
	struct dirent *e;
	const char *dot;
	DIR *d;
	int r = 0;

	if (!(d = opendir(etcpath(dir, sizeof(dir), ""))))
		return 0;
	while ((e = readdir(d))) {
		if (!(dot = strrchr(e->d_name, '.')) ||
		    (strcmp(dot, ".wants") && strcmp(dot, ".requires")))
			continue;
		snprintf(sub, sizeof(sub), "%s/%s", dir, e->d_name);
		r |= fn(sub, name);
	}
	closedir(d);
	return r;
}

static int
unlinkin(const char *dir, const char *name)
{
	char path[PATH_MAX];

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	if (unlink(path) < 0)
		return 0;
	printf("Removed %s\n", path + strlen(root));
	return 1;
}

static int
linkedin(const char *dir, const char *name)
{
	char path[PATH_MAX];
	struct stat st;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	return lstat(path, &st) == 0;
}

static int
enable(const char *name)
{
	char path[PATH_MAX], dir[PATH_MAX], sub[NAME_MAX + 8], lnk[PATH_MAX + NAME_MAX];
	Conf c;
	size_t i;
	int r = 0;

	if (unittemplate(name, NULL, 0) < 0) {
		warn("%s is a template: enable an instance, like %.*s@x%s", name,
		     (int)(strchr(name, '@') - name), name, strchr(name, '@') + 1);
		return 1;
	}
	switch (unitfind(name, path, sizeof(path))) {
	case LOAD_MASKED:
		warn("%s is masked", name);
		return 1;
	case LOAD_NOTFOUND:
		warn("%s not found", name);
		return 1;
	}
	if (unitparse(&c, name, path) < 0) {
		conffree(&c);
		return 1;
	}
	if (!c.wantedby.n)
		warn("%s has no WantedBy=: it is static, started by what needs it", name);
	for (i = 0; i < c.wantedby.n; i++) {
		snprintf(sub, sizeof(sub), "%s.wants", c.wantedby.v[i]);
		mkdirs(etcpath(dir, sizeof(dir), sub));
		snprintf(lnk, sizeof(lnk), "%s/%s", dir, name);
		if (symlink(path + strlen(root), lnk) < 0 && errno != EEXIST) {
			warn("symlink %s:", lnk);
			r = 1;
			continue;
		}
		printf("Created symlink %s -> %s\n", lnk + strlen(root), path + strlen(root));
	}
	conffree(&c);
	return r;
}

static int
mask(const char *name)
{
	char path[PATH_MAX], lnk[PATH_MAX];
	ssize_t n;

	mkdirs(etcpath(path, sizeof(path), ""));
	etcpath(path, sizeof(path), name);
	if ((n = readlink(path, lnk, sizeof(lnk) - 1)) >= 0) {
		lnk[n] = '\0';
		if (!strcmp(lnk, "/dev/null"))
			return 0;
	}
	if (symlink("/dev/null", path) < 0) {
		warn("cannot mask %s: %s", path + strlen(root),
		     errno == EEXIST ? "a unit file is there" : strerror(errno));
		return 1;
	}
	printf("Created symlink %s -> /dev/null\n", path + strlen(root));
	return 0;
}

static int
unmask(const char *name)
{
	char path[PATH_MAX], lnk[PATH_MAX];
	ssize_t n;

	etcpath(path, sizeof(path), name);
	if ((n = readlink(path, lnk, sizeof(lnk) - 1)) < 0)
		return 0;
	lnk[n] = '\0';
	if (strcmp(lnk, "/dev/null"))
		return 0;
	if (unlink(path) < 0) {
		warn("unlink %s:", path);
		return 1;
	}
	printf("Removed %s\n", path + strlen(root));
	return 0;
}

static int
isenabled(const char *name)
{
	char path[PATH_MAX];
	Conf c;
	int st;

	switch (unitfind(name, path, sizeof(path))) {
	case LOAD_MASKED:
		puts("masked");
		return 1;
	case LOAD_NOTFOUND:
		puts("not-found");
		return 4;
	}
	if (eachdir(name, linkedin)) {
		puts("enabled");
		return 0;
	}
	unitparse(&c, name, path);
	st = c.wantedby.n != 0;
	conffree(&c);
	puts(st ? "disabled" : "static");
	return st;
}

static int
setdefault(const char *arg)
{
	char name[NAME_MAX + 1], path[PATH_MAX], lnk[PATH_MAX];

	snprintf(name, sizeof(name), unitkind(arg) >= 0 ? "%s" : "%s.target", arg);
	if (unitkind(name) != UNIT_TARGET) {
		warn("%s is not a target", name);
		return 1;
	}
	if (unitfind(name, path, sizeof(path)) != LOAD_OK) {
		warn("%s not found", name);
		return 1;
	}
	mkdirs(etcpath(lnk, sizeof(lnk), ""));
	etcpath(lnk, sizeof(lnk), "default.target");
	if ((unlink(lnk) < 0 && errno != ENOENT) || symlink(path + strlen(root), lnk) < 0) {
		warn("%s:", lnk);
		return 1;
	}
	printf("Created symlink %s -> %s\n", lnk + strlen(root), path + strlen(root));
	return 0;
}

static int
getdefault(void)
{
	char name[2][NAME_MAX + 1], path[PATH_MAX];
	const char *n = "default.target";
	int i;

	for (i = 0; i < 8 && unitalias(n, name[i % 2], sizeof(name[0])) == 1; i++)
		n = name[i % 2];
	if (unitfind(n, path, sizeof(path)) != LOAD_OK) {
		warn("no default target");
		return 1;
	}
	puts(n);
	return 0;
}

static int
cat(const char *name)
{
	char path[PATH_MAX], buf[4096];
	size_t n;
	FILE *fp;

	switch (unitfind(name, path, sizeof(path))) {
	case LOAD_MASKED:
		printf("# %s is masked\n", name);
		return 1;
	case LOAD_NOTFOUND:
		warn("%s not found", name);
		return 1;
	}
	if (!(fp = fopen(path, "re"))) {
		warn("%s:", path);
		return 1;
	}
	printf("# %s\n", path + strlen(root));
	while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
		fwrite(buf, 1, n, stdout);
	fclose(fp);
	return 0;
}

/* poweroff -f: no units stopped, no questions asked */
static void
force(int how)
{
	sync();
	if (reboot(how) < 0)
		die("reboot:");
}

int
main(int argc, char *argv[])
{
	static const struct {
		char *name;
		int how;
	} power[] = {
		{ "poweroff", RB_POWER_OFF }, { "reboot", RB_AUTOBOOT }, { "halt", RB_HALT_SYSTEM },
	};
	/* sent to simpled together; start, stop and restart wait for the jobs */
	static const char *daemon[] = {
		"start", "stop", "restart", "reload", "reset-failed",
	};
	Buf req = { 0 };
	char *cmd, *name;
	int i, n, r = 0, f = 0, block = 1, code;
	size_t k;

	argv0 = strrchr(argv[0], '/') ? strrchr(argv[0], '/') + 1 : argv[0];
	/* options anywhere; the rest move to the front of argv */
	for (i = n = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-r") && i + 1 < argc)
			root = argv[++i];
		else if (!strcmp(argv[i], "-f") || !strcmp(argv[i], "--force"))
			f = 1;
		else if (!strcmp(argv[i], "--no-block"))
			block = 0;
		else if (argv[i][0] == '-')
			usage();
		else
			argv[n++] = argv[i];
	}
	argc = n;
	i = 1;
	for (k = 0; k < LEN(power); k++)
		if (!strcmp(argv0, power[k].name))
			break;
	if (k < LEN(power)) {
		cmd = argv0;
	} else {
		if (i >= argc)
			usage();
		cmd = argv[i++];
	}
	for (k = 0; k < LEN(power); k++) {
		if (strcmp(cmd, power[k].name))
			continue;
		if (f)
			force(power[k].how);
		return ask(cmd, 0);
	}
	if (!strcmp(cmd, "list") || !strcmp(cmd, "list-units"))
		return list();
	if (!strcmp(cmd, "daemon-reload"))
		return ask("daemon-reload", 0);
	if (!strcmp(cmd, "get-default"))
		return getdefault();
	if (i >= argc)
		usage();
	if (!strcmp(cmd, "set-default"))
		return setdefault(argv[i]);

	for (; i < argc; i++) {
		name = unitname(argv[i]);
		for (k = 0; k < LEN(daemon) && strcmp(cmd, daemon[k]); k++)
			;
		if (k < LEN(daemon)) {
			if (!req.n)
				bufprintf(&req, "%s%s", cmd, block ? "" : " --no-block");
			bufprintf(&req, " %s", name);
		} else if (!strcmp(cmd, "status")) {
			code = status(name);
			tail(name);
			r = code > r ? code : r;
			if (i + 1 < argc)
				putchar('\n');
		} else if (!strcmp(cmd, "is-active")) {
			code = isactive(name);
			r = code > r ? code : r;
		} else if (!strcmp(cmd, "enable")) {
			r |= enable(name);
		} else if (!strcmp(cmd, "disable")) {
			eachdir(name, unlinkin);
		} else if (!strcmp(cmd, "is-enabled")) {
			code = isenabled(name);
			r = code > r ? code : r;
		} else if (!strcmp(cmd, "mask")) {
			r |= mask(name);
		} else if (!strcmp(cmd, "unmask")) {
			r |= unmask(name);
		} else if (!strcmp(cmd, "cat")) {
			r |= cat(name);
			if (i + 1 < argc)
				putchar('\n');
		} else {
			usage();
		}
	}
	if (req.n) {
		bufadd(&req, "\n", 1);
		r = ask(req.s, block);
	}
	free(req.s);
	return r;
}
