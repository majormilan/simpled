/* See LICENSE file for copyright and license details.
 *
 * simplectl: talks to simpled, or changes unit links on disk. Also
 * poweroff, reboot and halt, by the name it is called as.
 *
 * miviex has no Unix sockets yet, so a request is a file: written to
 * RUNDIR/req/<pid>, announced with SIGUSR1, answered in place by simpled
 * and renamed to <pid>.done.
 */
#include <sys/reboot.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "util.h"
#include "config.h"
#include "unit.h"

#define TAIL 10

static void
usage(void)
{
	die("usage: %s [-r root] [-f] command [unit...]\n"
	    "  start stop restart reload status is-active reset-failed unit...\n"
	    "  list daemon-reload poweroff reboot halt\n"
	    "  enable disable is-enabled mask unmask unit...", argv0);
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

/* Sends one request, prints the answer, returns its exit code. */
static int
ask(const char *req)
{
	char path[PATH_MAX], tmp[PATH_MAX + 8], done[PATH_MAX + 8], buf[4096], *p;
	struct timespec ts = { 0, 10 * 1000000 };
	long long until;
	Buf out = { 0 };
	ssize_t n;
	int fd;

	snprintf(path, sizeof(path), "%s%s/req/%d", root, RUNDIR, (int)getpid());
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	snprintf(done, sizeof(done), "%s.done", path);
	if ((fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600)) < 0)
		die("cannot reach simpled: %s: %s", tmp, strerror(errno));
	if (write(fd, req, strlen(req)) != (ssize_t)strlen(req) || close(fd) < 0 ||
	    rename(tmp, path) < 0) {
		unlink(tmp);
		die("%s:", path);
	}
	if (kill(daemonpid(), SIGUSR1) < 0) {
		unlink(path);
		die("signal simpled:");
	}
	for (until = now() + CTLTIMEOUT; (fd = open(done, O_RDONLY | O_CLOEXEC)) < 0;) {
		if (now() >= until) {
			unlink(path);
			die("simpled does not answer");
		}
		nanosleep(&ts, NULL);
	}
	while ((n = read(fd, buf, sizeof(buf))) > 0)
		bufadd(&out, buf, n);
	close(fd);
	unlink(done);
	if (!out.s || !(p = strchr(out.s, '\n')))
		die("bad answer from simpled");
	fputs(p + 1, stdout);
	n = atoi(out.s);
	free(out.s);
	return n;
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
	Buf req = { 0 };
	char *cmd, *name;
	int i, r = 0, f = 0, code;
	size_t k;

	argv0 = strrchr(argv[0], '/') ? strrchr(argv[0], '/') + 1 : argv[0];
	for (k = 0; k < LEN(power); k++)
		if (!strcmp(argv0, power[k].name))
			break;
	for (i = 1; i < argc && argv[i][0] == '-'; i++) {
		if (!strcmp(argv[i], "-r") && i + 1 < argc)
			root = argv[++i];
		else if (!strcmp(argv[i], "-f") || !strcmp(argv[i], "--force"))
			f = 1;
		else if (!strcmp(argv[i], "--"))
			break;
		else
			usage();
	}
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
		return ask(cmd);
	}
	if (!strcmp(cmd, "list") || !strcmp(cmd, "list-units"))
		return ask("list");
	if (!strcmp(cmd, "daemon-reload"))
		return ask("daemon-reload");
	if (i >= argc)
		usage();

	for (; i < argc; i++) {
		name = unitname(argv[i]);
		if (!strcmp(cmd, "enable")) {
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
		} else if (!strcmp(cmd, "status")) {
			/* one at a time, each with the end of its log */
			bufreset(&req);
			bufprintf(&req, "status %s\n", name);
			code = ask(req.s);
			tail(name);
			r = code > r ? code : r;
			if (i + 1 < argc)
				putchar('\n');
		} else if (!strcmp(cmd, "start") || !strcmp(cmd, "stop") ||
		           !strcmp(cmd, "restart") || !strcmp(cmd, "reload") ||
		           !strcmp(cmd, "is-active") || !strcmp(cmd, "reset-failed")) {
			if (!req.n)
				bufprintf(&req, "%s", cmd);
			bufprintf(&req, " %s", name);
		} else {
			usage();
		}
	}
	if (req.n && strncmp(req.s, "status ", 7)) {
		bufadd(&req, "\n", 1);
		r = ask(req.s);
	}
	free(req.s);
	return r;
}
