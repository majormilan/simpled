/* See LICENSE file for copyright and license details.
 *
 * simpled: PID 1. Starts the units default.target pulls in, in order,
 * restarts them, stops them, and brings the system down.
 *
 * One thread, one loop. Signal handlers only raise a flag and wake the
 * loop through a pipe that never holds more than one byte (pipe writes
 * can block on miviex, and ppoll's mask is not atomic there).
 */
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "util.h"
#include "config.h"
#include "unit.h"

enum { INACTIVE, ACTIVATING, ACTIVE, DEACTIVATING, FAILED };
enum { JOB_NONE, JOB_START, JOB_STOP };
/* what a service is doing; the command lists run one after the other */
enum { PH_NONE, PH_PRE, PH_START, PH_MAIN, PH_RELOAD, PH_STOP, PH_KILL, PH_WAIT };

typedef struct Unit Unit;
struct Unit {
	char *name, *path;
	int kind, load;
	Conf c;
	int state, phase, job;
	int busy;               /* the job has begun */
	int again;              /* start once the stop job is through */
	int killed;             /* SIGKILL sent */
	int canceled;           /* a stop replaced the start job */
	size_t cmd;             /* next command of the phase */
	pid_t main, ctl;        /* each leads its own session and group */
	int status;             /* last wait status */
	const char *result;     /* why it failed */
	long long since, timer, window;
	int starts;
	unsigned mark;
	char *pub;              /* what RUNDIR/units/<name> says */
	Unit *next;
};

static const char *statename[] = {
	[INACTIVE] = "inactive", [ACTIVATING] = "activating", [ACTIVE] = "active",
	[DEACTIVATING] = "deactivating", [FAILED] = "failed",
};
static const char *phasename[] = {
	[PH_NONE] = "dead", [PH_PRE] = "start-pre", [PH_START] = "start",
	[PH_MAIN] = "running", [PH_RELOAD] = "reload", [PH_STOP] = "stop",
	[PH_KILL] = "stop-sigterm", [PH_WAIT] = "auto-restart",
};
static const char *loadname[] = {
	[LOAD_OK] = "loaded", [LOAD_NOTFOUND] = "not-found",
	[LOAD_MASKED] = "masked", [LOAD_ERROR] = "error",
};

/* what the kernel does not mount for PID 1 */
static const struct {
	char *src, *dir, *type, *data;
} mounts[] = {
	{ "proc",  "/proc", "proc",  NULL },
	{ "tmpfs", "/run",  "tmpfs", "mode=0755" },
};

/* A request that waits for the jobs of its units */
typedef struct Req Req;
struct Req {
	int fd;
	char *path;             /* <name>.wait, renamed to <name>.done */
	Buf out;
	int code;
	int stop;               /* reports the stop jobs, not the starts */
	Unit **wait;
	size_t n;
	Req *next;
};

static Unit *units;
static Req *reqs;
static unsigned gen;            /* marks visited units in start and stop */
static int pid1;
static int halting;             /* the reboot(2) command once shutting down */
static long long deadline;
static int wakefd[2];
static volatile sig_atomic_t sigs[NSIG], woken;

static void start(Unit *);
static void stop(Unit *);

static void
onsig(int sig)
{
	int e = errno;

	sigs[sig] = 1;
	if (!woken) {
		woken = 1;
		if (write(wakefd[1], "", 1) < 0) {
			/* full: the loop is awake anyway */
		}
	}
	errno = e;
}

static const char *
desc(Unit *u)
{
	return u->c.desc ? u->c.desc : u->name;
}

static void
enter(Unit *u, int state)
{
	if (u->state == state)
		return;
	if (state == ACTIVE)
		warn("started %s", desc(u));
	else if (state == FAILED)
		warn("failed %s (%s)", desc(u), u->result ? u->result : "?");
	else if (state == INACTIVE && u->state == ACTIVATING && u->c.type == ONESHOT &&
	         u->phase == PH_NONE)
		warn("finished %s", desc(u));
	else if (state == INACTIVE && u->state != FAILED)
		warn("stopped %s", desc(u));
	u->state = state;
	u->since = now();
}

static void
unitload(Unit *u)
{
	char path[PATH_MAX];
	Conf c;

	free(u->path);
	u->path = NULL;
	if ((u->load = unitfind(u->name, path, sizeof(path))) == LOAD_OK) {
		u->path = estrdup(path);
		if (unitparse(&c, u->name, path) < 0) {
			conffree(&c);
			u->load = LOAD_ERROR;
		} else {
			conffree(&u->c);
			u->c = c;
			unitwants(&u->c, u->name);
		}
	}
	if (u->load != LOAD_OK && u->state == INACTIVE) {
		conffree(&u->c);
		if (u->load == LOAD_NOTFOUND)
			unitwants(&u->c, u->name);
	}
}

/* The unit called name (or what it is an alias of), loaded on first
 * use; NULL for a bad name. */
static Unit *
unitget(const char *name)
{
	char alias[2][NAME_MAX + 1];
	Unit *u, **p;
	int i;

	for (i = 0; i < 8 && unitalias(name, alias[i % 2], sizeof(alias[0])) == 1; i++)
		name = alias[i % 2];
	for (p = &units; *p; p = &(*p)->next)
		if (!strcmp((*p)->name, name))
			return *p;
	if (unitkind(name) < 0)
		return NULL;
	u = ecalloc(1, sizeof(*u));
	u->name = estrdup(name);
	u->kind = unitkind(name);
	confinit(&u->c);
	unitload(u);
	return *p = u;
}

/* Environment for the child: PATH, the user's (the one it runs as, root
 * without User=), then the unit's. */
static int
setupenv(Unit *u, struct passwd *pw)
{
	List l = { 0 };
	FILE *fp;
	char *line = NULL, *f, *v, buf[16];
	size_t i, j, cap = 0;
	ssize_t n;

	clearenv();
	setenv("PATH", DEFPATH, 1);
	if (pw || (pw = getpwuid(getuid()))) {
		setenv("HOME", pw->pw_dir, 1);
		setenv("USER", pw->pw_name, 1);
		setenv("LOGNAME", pw->pw_name, 1);
		setenv("SHELL", pw->pw_shell, 1);
	}
	if (u->main) {
		snprintf(buf, sizeof(buf), "%d", (int)u->main);
		setenv("MAINPID", buf, 1);
	}
	for (i = 0; i < u->c.envfile.n; i++) {
		f = u->c.envfile.v[i];
		if (!(fp = fopen(f + (*f == '-'), "re"))) {
			if (*f == '-')
				continue;
			warn("%s:", f);
			return -1;
		}
		while ((n = getline(&line, &cap, fp)) > 0) {
			line[strcspn(line, "\n")] = '\0';
			if (*line && *line != '#' && *line != ';' && strchr(line, '='))
				ladd(&l, line);
		}
		fclose(fp);
	}
	for (i = 0; i < u->c.env.n; i++)
		ladd(&l, u->c.env.v[i]);
	for (i = 0; i < l.n; i++) {
		v = strchr(l.v[i], '=');
		*v++ = '\0';
		j = strlen(v);
		if (j >= 2 && (*v == '"' || *v == '\'') && v[j - 1] == *v) {
			v[j - 1] = '\0';
			v++;
		}
		setenv(l.v[i], v, 1);
	}
	free(line);
	lfree(&l);
	return 0;
}

static int
openio(Unit *u, int io, int flags)
{
	char path[PATH_MAX];
	int fd;

	switch (io) {
	case IO_LOG:
		mkdir(rpath(path, sizeof(path), LOGDIR), 0755);
		snprintf(path, sizeof(path), "%s%s/%s.log", root, LOGDIR, u->name);
		return open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0640);
	case IO_CONSOLE:
		return open(CONSOLE, flags | O_NOCTTY | O_CLOEXEC);
	case IO_TTY:
		if ((fd = open(u->c.tty ? u->c.tty : CONSOLE, O_RDWR | O_CLOEXEC)) >= 0 && flags == O_RDONLY)
			ioctl(fd, TIOCSCTTY, 1);
		return fd;
	default:
		return open("/dev/null", flags | O_CLOEXEC);
	}
}

/* stdin, stdout and stderr; on failure the inherited one stays */
static void
setupio(Unit *u)
{
	int io[3], fd[3], i;

	io[0] = u->c.in == IO_TTY ? IO_TTY : IO_NULL;
	io[1] = u->c.out == IO_INHERIT ? io[0] : u->c.out;
	io[2] = u->c.err == IO_INHERIT ? io[1] : u->c.err;
	for (i = 0; i < 3; i++) {
		if (i && io[i] == io[i - 1] && fd[i - 1] >= 0)
			fd[i] = dup(fd[i - 1]);
		else
			fd[i] = openio(u, io[i], i ? O_WRONLY : O_RDONLY);
		if (fd[i] < 0)
			warn("%s: stdio %d:", u->name, i);
	}
	for (i = 0; i < 3; i++)
		if (fd[i] >= 0)
			dup2(fd[i], i);
	for (i = 0; i < 3; i++)
		if (fd[i] > 2)
			close(fd[i]);
}

static void
child(Unit *u, const char *cmd)
{
	struct passwd *pw = NULL;
	struct group *gr;
	List argv = { 0 };
	sigset_t set;
	gid_t gid;
	const char *dir;
	int i;

	for (i = 1; i < NSIG; i++)
		signal(i, SIG_DFL);
	sigemptyset(&set);
	sigprocmask(SIG_SETMASK, &set, NULL);
	setsid();
	setupio(u);

	/* a user or group that cannot be set fails the unit: never root */
	if (u->c.user && !(pw = getpwnam(u->c.user))) {
		warn("%s: no user %s", u->name, u->c.user);
		_exit(126);
	}
	gid = pw ? pw->pw_gid : getgid();
	if (u->c.group) {
		if (!(gr = getgrnam(u->c.group))) {
			warn("%s: no group %s", u->name, u->c.group);
			_exit(126);
		}
		gid = gr->gr_gid;
	}
	if ((pw && initgroups(pw->pw_name, gid) < 0) ||
	    (!pw && u->c.group && setgroups(0, NULL) < 0) ||
	    setgid(gid) < 0 || (pw && setuid(pw->pw_uid) < 0)) {
		warn("%s: credentials:", u->name);
		_exit(126);
	}
	umask(u->c.umask);
	dir = u->c.workdir ? u->c.workdir : "/";
	if (!strcmp(dir, "~"))
		dir = pw ? pw->pw_dir : "/";
	if (chdir(dir + (*dir == '-')) < 0 && *dir != '-') {
		warn("%s: chdir %s:", u->name, dir);
		_exit(126);
	}
	if (setupenv(u, pw) < 0)
		_exit(126);
	if (splitwords(cmd, &argv) < 0 || !argv.n) {
		warn("%s: bad command line: %s", u->name, cmd);
		_exit(126);
	}
	execvp(argv.v[0], argv.v);
	warn("%s: exec %s:", u->name, argv.v[0]);
	_exit(127);
}

/* A leading '-' means the exit status does not matter. */
static pid_t
spawn(Unit *u, const char *cmd)
{
	pid_t pid;

	if ((pid = fork()) < 0) {
		warn("%s: fork:", u->name);
		return -1;
	}
	if (pid == 0)
		child(u, cmd + (*cmd == '-'));
	return pid;
}

static List *
cmds(Unit *u)
{
	switch (u->phase) {
	case PH_PRE:    return &u->c.pre;
	case PH_START:  return &u->c.start;
	case PH_RELOAD: return &u->c.reload;
	case PH_STOP:   return &u->c.stop;
	default:        return NULL;
	}
}

static void
done(Unit *u)
{
	u->job = JOB_NONE;
	u->busy = 0;
	if (u->again && !halting) {
		u->again = 0;
		gen++;
		start(u);
	}
}

static void
fail(Unit *u, const char *why)
{
	u->result = why;
	u->phase = PH_NONE;
	u->timer = 0;
	enter(u, FAILED);
	done(u);
}

static void
stopped(Unit *u)
{
	u->phase = PH_NONE;
	u->timer = 0;
	if (u->state != FAILED)
		enter(u, INACTIVE);
	done(u);
}

static void
killall(Unit *u, int sig)
{
	if (u->main) {
		kill(-u->main, sig);
		kill(-u->main, SIGCONT);
	}
	if (u->ctl) {
		kill(-u->ctl, sig);
		kill(-u->ctl, SIGCONT);
	}
}

/* Runs the next command of the phase, or moves on to the next phase. */
static void
advance(Unit *u)
{
	List *l;

	while ((l = cmds(u))) {
		if (u->cmd < l->n) {
			if ((u->ctl = spawn(u, l->v[u->cmd++])) > 0)
				return;
			u->ctl = 0;
			if (u->phase == PH_PRE || u->phase == PH_START) {
				fail(u, "resources");
				return;
			}
			continue;
		}
		switch (u->phase) {
		case PH_PRE:
			if (u->c.type == ONESHOT) {
				u->phase = PH_START;
				u->cmd = 0;
				continue;
			}
			if ((u->main = spawn(u, u->c.start.v[0])) < 0) {
				u->main = 0;
				fail(u, "resources");
				return;
			}
			u->phase = PH_MAIN;
			enter(u, ACTIVE);
			done(u);
			return;
		case PH_START:
			u->phase = PH_NONE;
			enter(u, u->c.remain ? ACTIVE : INACTIVE);
			done(u);
			return;
		case PH_RELOAD:
			u->phase = u->main ? PH_MAIN : PH_NONE;
			return;
		case PH_STOP:
			u->phase = PH_KILL;
			if (!u->main) {
				stopped(u);
				return;
			}
			killall(u, u->c.killsig);
			return;
		}
	}
}

static int
startlimit(Unit *u)
{
	long long t = now();

	if (!u->c.interval || !u->c.burst)
		return 0;
	if (!u->window || t - u->window > u->c.interval) {
		u->window = t;
		u->starts = 0;
	}
	return ++u->starts > u->c.burst;
}

/* a Requires= unit that failed and has nothing more coming */
static int
depfailed(Unit *u)
{
	Unit *d;
	size_t i;

	for (i = 0; i < u->c.dep[DEP_REQUIRES].n; i++) {
		d = unitget(u->c.dep[DEP_REQUIRES].v[i]);
		if (!d || (d->job == JOB_NONE && d->state == FAILED))
			return 1;
	}
	return 0;
}

static void
beginstart(Unit *u)
{
	u->timer = 0;
	u->result = NULL;
	u->canceled = 0;
	if (u->load != LOAD_OK) {
		fail(u, loadname[u->load]);
		return;
	}
	if (depfailed(u)) {
		fail(u, "dependency");
		return;
	}
	if (u->kind == UNIT_TARGET) {
		enter(u, ACTIVE);
		done(u);
		return;
	}
	if (startlimit(u)) {
		fail(u, "start-limit-hit");
		return;
	}
	u->busy = 1;
	enter(u, ACTIVATING);
	u->phase = PH_PRE;
	u->cmd = 0;
	advance(u);
}

static void
beginstop(Unit *u)
{
	if (!u->main && !u->ctl && !(u->state == ACTIVE && u->c.stop.n)) {
		stopped(u);
		return;
	}
	u->busy = 1;
	u->killed = 0;
	u->timer = u->c.stoptimeout ? now() + u->c.stoptimeout : 0;
	enter(u, DEACTIVATING);
	if (u->ctl) {
		/* a start or reload command still runs: cut it short */
		u->phase = PH_KILL;
		killall(u, u->c.killsig);
		return;
	}
	u->phase = PH_STOP;
	u->cmd = 0;
	advance(u);
}

static void
mainexit(Unit *u, int status)
{
	int clean = WIFEXITED(status) && WEXITSTATUS(status) == 0;

	kill(-u->main, SIGKILL); /* what it left behind in its group */
	u->main = 0;
	u->status = status;
	if (u->state == DEACTIVATING) {
		if (u->phase == PH_KILL && !u->ctl)
			stopped(u);
		return;
	}
	if (WIFEXITED(status))
		warn("%s exited, status %d", u->name, WEXITSTATUS(status));
	else
		warn("%s killed by signal %d", u->name, WTERMSIG(status));
	if (u->ctl)
		kill(-u->ctl, SIGKILL);
	if (!halting && u->job != JOB_STOP &&
	    (u->c.restart == RESTART_ALWAYS || (u->c.restart == RESTART_ONFAILURE && !clean))) {
		u->phase = PH_WAIT;
		u->timer = now() + u->c.restartsec;
		enter(u, ACTIVATING);
		return;
	}
	u->phase = PH_NONE;
	if (clean) {
		enter(u, INACTIVE);
	} else {
		u->result = WIFEXITED(status) ? "exit-code" : "signal";
		enter(u, FAILED);
	}
}

static void
ctlexit(Unit *u, int status)
{
	List *l = cmds(u);
	int ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;

	if (l && u->cmd && l->v[u->cmd - 1][0] == '-')
		ok = 1;
	u->ctl = 0;
	switch (u->phase) {
	case PH_PRE:
	case PH_START:
		u->status = status;
		if (!ok) {
			fail(u, WIFEXITED(status) ? "exit-code" : "signal");
			return;
		}
		advance(u);
		break;
	case PH_RELOAD:
		if (!ok) {
			warn("%s: reload failed", u->name);
			u->phase = u->main ? PH_MAIN : PH_NONE;
			return;
		}
		advance(u);
		break;
	case PH_STOP:
		advance(u);
		break;
	case PH_KILL:
		if (!u->main)
			stopped(u);
		break;
	}
}

static void
reap(void)
{
	Unit *u;
	pid_t pid;
	int status;

	while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
		for (u = units; u; u = u->next) {
			if (pid == u->main) {
				mainexit(u, status);
				break;
			}
			if (pid == u->ctl) {
				ctlexit(u, status);
				break;
			}
		}
		/* anything else was an orphan we adopted */
	}
}

/* a must start before b, and b stop before a. A target comes after
 * what it pulls in, unless that is ordered after the target itself. */
static int
before(Unit *a, Unit *b)
{
	return lhas(&b->c.dep[DEP_AFTER], a->name) ||
	       lhas(&a->c.dep[DEP_BEFORE], b->name) ||
	       (b->kind == UNIT_TARGET && !lhas(&a->c.dep[DEP_AFTER], b->name) &&
	        !lhas(&b->c.dep[DEP_BEFORE], a->name) &&
	        (lhas(&b->c.dep[DEP_WANTS], a->name) || lhas(&b->c.dep[DEP_REQUIRES], a->name)));
}

/* the unit whose job u's job waits for, or NULL */
static Unit *
blocker(Unit *u)
{
	Unit *v;

	for (v = units; v; v = v->next) {
		if (v == u || v->job == JOB_NONE)
			continue;
		if (u->job == JOB_START && before(v, u))
			return v;
		if (u->job == JOB_STOP && v->job == JOB_STOP && before(u, v))
			return v;
	}
	return NULL;
}

static void
begin(Unit *u)
{
	if (u->job == JOB_START)
		beginstart(u);
	else
		beginstop(u);
}

static void
dispatch(void)
{
	Unit *u, *c = NULL;
	Buf b = { 0 };
	int progress, n = 0;

	do {
		progress = 0;
		for (u = units; u; u = u->next) {
			if (u->job == JOB_NONE || u->busy || blocker(u))
				continue;
			begin(u);
			progress = 1;
		}
	} while (progress);

	/* Jobs are left, but none runs to unblock them: they wait in a
	 * circle. Following what each waits for long enough lands in it. */
	for (u = units; u; u = u->next) {
		if (u->busy)
			return;
		if (u->job != JOB_NONE && !c)
			c = u;
		n++;
	}
	if (!c)
		return;
	while (n-- > 0)
		c = blocker(c);
	u = c;
	do {
		bufprintf(&b, "%s -> ", u->name);
	} while ((u = blocker(u)) != c);
	warn("ordering cycle: %s%s, %s %s first", b.s, c->name,
	     c->job == JOB_START ? "starting" : "stopping", c->name);
	free(b.s);
	begin(c);
	dispatch();
}

/* Queues a start job for u and what it pulls in. */
static void
start(Unit *u)
{
	Unit *v;
	size_t i;
	int up;

	if (u->mark == gen || halting)
		return;
	u->mark = gen;
	if (u->job == JOB_STOP) {
		if (u->busy)
			u->again = 1;
		else
			u->job = JOB_NONE;
	}
	up = u->state == ACTIVE || (u->state == ACTIVATING && u->phase != PH_WAIT);
	if (u->job == JOB_NONE && !u->again && !up)
		u->job = JOB_START;
	for (i = 0; i < u->c.dep[DEP_REQUIRES].n; i++)
		if ((v = unitget(u->c.dep[DEP_REQUIRES].v[i])))
			start(v);
	for (i = 0; i < u->c.dep[DEP_WANTS].n; i++)
		if ((v = unitget(u->c.dep[DEP_WANTS].v[i])))
			start(v);
	for (i = 0; i < u->c.dep[DEP_CONFLICTS].n; i++)
		if ((v = unitget(u->c.dep[DEP_CONFLICTS].v[i])))
			stop(v);
	for (v = units; v; v = v->next)
		if (lhas(&v->c.dep[DEP_CONFLICTS], u->name))
			stop(v);
}

/* Queues a stop job for u and the units that require it. */
static void
stop(Unit *u)
{
	Unit *v;

	if (u->mark == gen)
		return;
	u->mark = gen;
	u->again = 0;
	if (u->job == JOB_START) {
		u->job = JOB_NONE;
		u->busy = 0;
		u->canceled = 1;
	}
	if (u->job == JOB_NONE && (u->main || u->ctl || u->phase == PH_WAIT ||
	    (u->state != INACTIVE && u->state != FAILED)))
		u->job = JOB_STOP;
	for (v = units; v; v = v->next)
		if (lhas(&v->c.dep[DEP_REQUIRES], u->name))
			stop(v);
}

static void
timeout(Unit *u)
{
	if (!u->killed) {
		warn("%s: stop timed out, killing", u->name);
		u->killed = 1;
		u->phase = PH_KILL;
		killall(u, SIGKILL);
		u->timer = now() + KILLTIMEOUT;
		return;
	}
	warn("%s: does not die, abandoning it", u->name);
	u->main = u->ctl = 0;
	stopped(u);
}

static void
firetimers(void)
{
	long long t = now();
	Unit *u;

	for (u = units; u; u = u->next) {
		if (!u->timer || u->timer > t)
			continue;
		u->timer = 0;
		if (u->phase == PH_WAIT) {
			warn("restarting %s", u->name);
			if (u->job == JOB_NONE && !halting)
				u->job = JOB_START;
		} else if (u->state == DEACTIVATING) {
			timeout(u);
		}
	}
}

static long long
nexttimer(void)
{
	long long next = halting ? deadline : 0;
	Unit *u;

	for (u = units; u; u = u->next)
		if (u->timer && (!next || u->timer < next))
			next = u->timer;
	return next;
}

static const char *
sub(Unit *u)
{
	if (u->state == FAILED)
		return "failed";
	if (u->state == ACTIVE && u->phase == PH_NONE)
		return u->kind == UNIT_TARGET ? "active" : "exited";
	return phasename[u->phase];
}

/* Writes RUNDIR/units/<name> when what it says changes: simplectl reads
 * status, is-active and list from there, for anyone, with no request. */
static void
publish(Unit *u)
{
	char path[PATH_MAX + NAME_MAX + 2], tmp[PATH_MAX + NAME_MAX + 8];
	Buf b = { 0 };
	FILE *fp;

	bufprintf(&b, "load=%s\nstate=%s\nsub=%s\nsince=%lld\n", loadname[u->load],
	          statename[u->state], sub(u), u->since);
	bufprintf(&b, "path=%s\ndesc=%s\n", u->path ? u->path + strlen(root) : "",
	          u->c.desc ? u->c.desc : "");
	bufprintf(&b, "main=%d\nctl=%d\nresult=%s\nstatus=%d\njob=%s\n", (int)u->main,
	          (int)u->ctl, u->result ? u->result : "", u->status,
	          u->job == JOB_START ? "start" : u->job == JOB_STOP ? "stop" : "");
	if (u->pub && !strcmp(u->pub, b.s)) {
		free(b.s);
		return;
	}
	snprintf(path, sizeof(path), "%s%s/units/%s", root, RUNDIR, u->name);
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	if (!(fp = fopen(tmp, "we")) || fputs(b.s, fp) < 0 || fclose(fp) < 0 || rename(tmp, path) < 0) {
		warn("%s:", tmp);
		free(b.s);
		return;
	}
	free(u->pub);
	u->pub = b.s;
}

static void
halt(int how)
{
	Unit *u;

	if (halting)
		return;
	warn("shutting down");
	halting = how;
	deadline = now() + SHUTDOWNTIMEOUT;
	gen++;
	for (u = units; u; u = u->next)
		stop(u);
}

static void
reload(void)
{
	Unit *u;

	warn("reloading unit files");
	for (u = units; u; u = u->next)
		unitload(u);
}

static int
pending(Unit *u)
{
	return u->job != JOB_NONE || u->again;
}

/* why the last start of u did not bring it up, NULL if it did */
static const char *
startresult(Unit *u)
{
	if (u->canceled)
		return "canceled";
	if (u->state == FAILED)
		return u->result ? u->result : "failed";
	if (u->state == ACTIVE || u->state == ACTIVATING)
		return NULL;
	if (u->kind == UNIT_SERVICE && u->c.type == ONESHOT && u->state == INACTIVE)
		return NULL;
	return "canceled";
}

static void
reply(Req *r)
{
	char done[PATH_MAX + NAME_MAX + 8];
	const char *why;
	size_t i;

	for (i = 0; i < r->n; i++) {
		if (r->stop || !(why = startresult(r->wait[i])))
			continue;
		bufprintf(&r->out, "Job for %s failed (%s).\nSee 'simplectl status %s'.\n",
		          r->wait[i]->name, why, r->wait[i]->name);
		r->code = 1;
	}
	snprintf(r->out.s, 4, "%-3d", r->code);
	r->out.s[3] = '\n';
	if (ftruncate(r->fd, 0) < 0 || pwrite(r->fd, r->out.s, r->out.n, 0) != (ssize_t)r->out.n)
		warn("%s:", r->path);
	close(r->fd);
	/* the name ends in .wait: swap it for .done */
	snprintf(done, sizeof(done), "%.*s.done", (int)(strlen(r->path) - 5), r->path);
	rename(r->path, done);
	free(r->path);
	free(r->out.s);
	free(r->wait);
	free(r);
}

/* Answers the requests whose jobs are all through. */
static void
answer(void)
{
	Req **p = &reqs, *r;
	size_t i;

	while ((r = *p)) {
		for (i = 0; i < r->n && !pending(r->wait[i]); i++)
			;
		if (i < r->n) {
			p = &r->next;
			continue;
		}
		*p = r->next;
		reply(r);
	}
}

/* One request: "command [--no-block] [unit...]". The reply starts with
 * the exit code simplectl returns, the rest is printed. start, stop and
 * restart wait for their jobs, unless --no-block. */
static void
command(char *req, Req *r)
{
	List w = { 0 };
	Unit *u;
	size_t i, j;
	int block = 1;
	char *cmd;

	bufprintf(&r->out, "%-3d\n", 0);
	if (splitwords(req, &w) < 0 || !w.n) {
		bufprintf(&r->out, "bad request\n");
		r->code = 1;
		goto end;
	}
	cmd = w.v[0];
	if (!strcmp(cmd, "daemon-reload")) {
		reload();
		goto end;
	}
	if (!strcmp(cmd, "poweroff") || !strcmp(cmd, "reboot") || !strcmp(cmd, "halt")) {
		halt(*cmd == 'p' ? RB_POWER_OFF : *cmd == 'r' ? RB_AUTOBOOT : RB_HALT_SYSTEM);
		goto end;
	}

	/* the rest act on units: the reply's code is the worst of them */
	r->stop = !strcmp(cmd, "stop");
	for (i = 1; i < w.n; i++) {
		if (!strcmp(w.v[i], "--no-block")) {
			block = 0;
			continue;
		}
		gen++;
		if (unittemplate(w.v[i], NULL, 0) < 0 || !(u = unitget(w.v[i]))) {
			bufprintf(&r->out, "%s: not a unit name\n", w.v[i]);
			r->code = r->code < 1 ? 1 : r->code;
			continue;
		}
		if (!strcmp(cmd, "reset-failed")) {
			if (u->state == FAILED)
				enter(u, INACTIVE);
			u->result = NULL;
			u->starts = 0;
			continue;
		}
		if (u->load != LOAD_OK && strcmp(cmd, "stop")) {
			bufprintf(&r->out, "%s: %s\n", u->name, loadname[u->load]);
			r->code = 5;
			continue;
		}
		if (!strcmp(cmd, "start")) {
			start(u);
		} else if (!strcmp(cmd, "stop")) {
			stop(u);
		} else if (!strcmp(cmd, "restart")) {
			stop(u);
			if (u->job == JOB_STOP) {
				u->again = 1;
			} else {
				gen++;
				start(u);
			}
		} else if (!strcmp(cmd, "reload")) {
			if (!u->c.reload.n || u->state != ACTIVE || u->ctl) {
				bufprintf(&r->out, "%s: cannot reload now\n", u->name);
				r->code = 1;
			} else {
				u->phase = PH_RELOAD;
				u->cmd = 0;
				advance(u);
			}
			continue;
		} else {
			bufprintf(&r->out, "%s: unknown command\n", cmd);
			r->code = 1;
			break;
		}
		for (j = 0; j < r->n && r->wait[j] != u; j++)
			;
		if (j == r->n) {
			r->wait = erealloc(r->wait, (r->n + 1) * sizeof(*r->wait));
			r->wait[r->n++] = u;
		}
	}
	if (!block)
		r->n = 0;
end:
	lfree(&w);
}

/* Requests are files in RUNDIR/req, which only root can write to. A
 * request taken is renamed to <name>.wait, and to <name>.done once
 * answered in place. */
static void
requests(void)
{
	char dir[PATH_MAX], path[PATH_MAX + NAME_MAX + 2], wait[PATH_MAX + NAME_MAX + 8], req[4096];
	struct dirent *e;
	struct stat st;
	ssize_t n;
	DIR *d;
	Req *r;
	int fd;

	snprintf(dir, sizeof(dir), "%s%s/req", root, RUNDIR);
	if (!(d = opendir(dir)))
		return;
	while ((e = readdir(d))) {
		if (strchr(e->d_name, '.'))
			continue;
		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		snprintf(wait, sizeof(wait), "%s.wait", path);
		if ((fd = open(path, O_RDWR | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)) < 0) {
			unlink(path);
			continue;
		}
		if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1 ||
		    (n = read(fd, req, sizeof(req) - 1)) < 0 || rename(path, wait) < 0) {
			close(fd);
			unlink(path);
			continue;
		}
		req[n] = '\0';
		r = ecalloc(1, sizeof(*r));
		r->fd = fd;
		r->path = estrdup(wait);
		command(req, r);
		r->next = reqs;
		reqs = r;
	}
	closedir(d);
}

static void
waitall(long long ms)
{
	long long until = now() + ms;
	struct timespec ts = { 0, 50 * 1000000 };

	while (waitpid(-1, NULL, WNOHANG) >= 0 || errno == EINTR) {
		if (now() >= until)
			return;
		nanosleep(&ts, NULL);
	}
}

static void
umountall(void)
{
	List l = { 0 };
	FILE *fp;
	char *line = NULL, *dir;
	size_t cap = 0, i;

	if ((fp = fopen("/proc/mounts", "re"))) {
		while (getline(&line, &cap, fp) > 0) {
			strtok(line, " ");
			if ((dir = strtok(NULL, " ")) && strcmp(dir, "/"))
				ladd(&l, dir);
		}
		fclose(fp);
	}
	for (i = l.n; i-- > 0;)
		umount2(l.v[i], MNT_DETACH);
	mount(NULL, "/", NULL, MS_REMOUNT | MS_RDONLY, NULL);
	free(line);
	lfree(&l);
}

static void
finish(void)
{
	char path[PATH_MAX];

	warn("all units stopped");
	unlink(rpath(path, sizeof(path), RUNDIR "/pid"));
	if (!pid1)
		exit(0);
	sync();
	warn("sending SIGTERM to all processes");
	kill(-1, SIGTERM);
	kill(-1, SIGCONT);
	waitall(KILLTIMEOUT);
	warn("sending SIGKILL to all processes");
	kill(-1, SIGKILL);
	waitall(KILLTIMEOUT);
	sync();
	umountall();
	sync();
	reboot(halting);
	warn("reboot:");
	for (;;)
		pause();
}

/* PID 1 work before any unit: what the kernel does not set up */
static void
boot(void)
{
	char name[256];
	FILE *fp;
	size_t i;

	for (i = 0; i < LEN(mounts); i++) {
		mkdir(mounts[i].dir, 0755);
		if (mount(mounts[i].src, mounts[i].dir, mounts[i].type, 0, mounts[i].data) < 0)
			warn("mount %s:", mounts[i].dir);
	}
	if ((fp = fopen("/etc/hostname", "re"))) {
		if (fgets(name, sizeof(name), fp)) {
			name[strcspn(name, "\n")] = '\0';
			if (sethostname(name, strlen(name)) < 0)
				warn("sethostname:");
		}
		fclose(fp);
	}
	/* Ctrl+Alt+Del reaches us as SIGINT */
	reboot(RB_DISABLE_CAD);
}

static void
usage(void)
{
	die("usage: %s [-r root] [-t unit]", argv0);
}

int
main(int argc, char *argv[])
{
	static const int handled[] = { SIGCHLD, SIGINT, SIGTERM, SIGHUP, SIGUSR1, SIGUSR2 };
	struct sigaction sa = { 0 };
	struct pollfd pfd;
	char path[PATH_MAX], buf[64], *target = DEFTARGET;
	long long next;
	Unit *u;
	FILE *fp;
	size_t i;
	int t;

	argv0 = "simpled";
	pid1 = getpid() == 1;
	for (i = 1; i < (size_t)argc; i++) {
		if (!strcmp(argv[i], "-r") && i + 1 < (size_t)argc)
			root = argv[++i];
		else if (!strcmp(argv[i], "-t") && i + 1 < (size_t)argc)
			target = argv[++i];
		/* Linux passes kernel arguments it does not know to init */
		else if (!strncmp(argv[i], "simpled.unit=", 13))
			target = argv[i] + 13;
		else if (!strcmp(argv[i], "emergency") || !strcmp(argv[i], "single"))
			target = EMERGENCY;
		else if (!pid1)
			usage();
	}

	if (pipe(wakefd) < 0)
		die("pipe:");
	for (i = 0; i < 2; i++)
		fcntl(wakefd[i], F_SETFD, FD_CLOEXEC);
	fcntl(wakefd[1], F_SETFL, O_NONBLOCK);
	sa.sa_handler = onsig;
	sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
	sigfillset(&sa.sa_mask);
	for (i = 0; i < LEN(handled); i++)
		sigaction(handled[i], &sa, NULL);

	if (pid1)
		boot();
	warn("starting %s", target);
	mkdir(rpath(path, sizeof(path), RUNDIR), 0755);
	mkdir(rpath(path, sizeof(path), RUNDIR "/req"), 0700);
	chmod(path, 0700);
	mkdir(rpath(path, sizeof(path), RUNDIR "/units"), 0755);
	if ((fp = fopen(rpath(path, sizeof(path), RUNDIR "/pid"), "we"))) {
		fprintf(fp, "%d\n", (int)getpid());
		fclose(fp);
	}

	gen++;
	if (!(u = unitget(target)) || u->load != LOAD_OK) {
		warn("cannot load %s, starting %s", target, EMERGENCY);
		u = unitget(EMERGENCY);
	}
	if (u)
		start(u);

	pfd.fd = wakefd[0];
	pfd.events = POLLIN;
	for (;;) {
		if (woken) {
			while (read(wakefd[0], buf, sizeof(buf)) < 0 && errno == EINTR)
				;
			woken = 0;
		}
		reap();
		sigs[SIGCHLD] = 0;
		if (sigs[SIGUSR1]) {
			sigs[SIGUSR1] = 0;
			requests();
		}
		if (sigs[SIGHUP]) {
			sigs[SIGHUP] = 0;
			reload();
		}
		if (sigs[SIGINT] || sigs[SIGTERM]) {
			sigs[SIGINT] = sigs[SIGTERM] = 0;
			halt(RB_AUTOBOOT);
		}
		if (sigs[SIGUSR2]) {
			sigs[SIGUSR2] = 0;
			halt(RB_POWER_OFF);
		}
		firetimers();
		dispatch();
		/* the state first: what a reply says must already be readable */
		for (u = units; u; u = u->next)
			publish(u);
		answer();
		if (halting) {
			for (u = units; u && u->job == JOB_NONE; u = u->next)
				;
			if (!u || now() >= deadline)
				finish();
		}
		next = nexttimer();
		t = next ? (int)(next > now() ? next - now() : 0) : -1;
		if (poll(&pfd, 1, t) < 0 && errno != EINTR)
			warn("poll:");
	}
}
