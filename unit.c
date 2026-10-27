/* See LICENSE file for copyright and license details. */
#include <sys/stat.h>
#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util.h"
#include "config.h"
#include "unit.h"

const char *unitpath[] = { UNITPATH, NULL };

static const struct {
	char *name;
	int sig;
} signals[] = {
	{ "SIGHUP", SIGHUP }, { "SIGINT", SIGINT }, { "SIGQUIT", SIGQUIT },
	{ "SIGKILL", SIGKILL }, { "SIGUSR1", SIGUSR1 }, { "SIGUSR2", SIGUSR2 },
	{ "SIGTERM", SIGTERM }, { "SIGCONT", SIGCONT }, { "SIGSTOP", SIGSTOP },
};

static char *
trim(char *s)
{
	char *e;

	while (isspace((unsigned char)*s))
		s++;
	for (e = s + strlen(s); e > s && isspace((unsigned char)e[-1]); e--)
		;
	*e = '\0';
	return s;
}

/* .service or .target, no slashes; NULL-safe for names from anywhere */
int
unitkind(const char *name)
{
	const char *dot = strrchr(name, '.');

	if (!*name || *name == '.' || strchr(name, '/') || strlen(name) > NAME_MAX || !dot)
		return -1;
	if (!strcmp(dot, ".service"))
		return UNIT_SERVICE;
	if (!strcmp(dot, ".target"))
		return UNIT_TARGET;
	return -1;
}

/* For an instance like getty@tty1.service: 1 and getty@.service in buf.
 * For a bare template like getty@.service: -1. Otherwise 0. */
int
unittemplate(const char *name, char *buf, size_t size)
{
	const char *at = strchr(name, '@'), *dot = strrchr(name, '.');

	if (!at || !dot || dot < at)
		return 0;
	if (at + 1 == dot)
		return -1;
	snprintf(buf, size, "%.*s%s", (int)(at - name + 1), name, dot);
	return 1;
}

/* Finds the file for name in unitpath: LOAD_OK and its path, LOAD_MASKED
 * for a link to /dev/null, or LOAD_NOTFOUND. Instances fall back to their
 * template. */
int
unitfind(const char *name, char *path, size_t size)
{
	char tmpl[NAME_MAX + 1], lnk[PATH_MAX];
	const char *try[2] = { name, NULL };
	struct stat st;
	ssize_t n;
	size_t i, j;

	if (unittemplate(name, tmpl, sizeof(tmpl)) == 1)
		try[1] = tmpl;
	for (j = 0; j < LEN(try) && try[j]; j++) {
		for (i = 0; unitpath[i]; i++) {
			snprintf(path, size, "%s%s/%s", root, unitpath[i], try[j]);
			if (lstat(path, &st) < 0)
				continue;
			if (S_ISLNK(st.st_mode) && (n = readlink(path, lnk, sizeof(lnk) - 1)) > 0) {
				lnk[n] = '\0';
				if (!strcmp(lnk, "/dev/null"))
					return LOAD_MASKED;
			}
			if (stat(path, &st) == 0 && S_ISREG(st.st_mode))
				return LOAD_OK;
		}
	}
	return LOAD_NOTFOUND;
}

/* An alias is a link in unitpath to a unit of another name, like
 * default.target -> multi-user.target: 1 and that name in buf. */
int
unitalias(const char *name, char *buf, size_t size)
{
	char path[PATH_MAX], lnk[PATH_MAX], *base;
	struct stat st;
	ssize_t n;
	size_t i;

	for (i = 0; unitpath[i]; i++) {
		snprintf(path, sizeof(path), "%s%s/%s", root, unitpath[i], name);
		if (lstat(path, &st) < 0)
			continue;
		if (!S_ISLNK(st.st_mode) || (n = readlink(path, lnk, sizeof(lnk) - 1)) < 0)
			return 0;
		lnk[n] = '\0';
		base = strrchr(lnk, '/') ? strrchr(lnk, '/') + 1 : lnk;
		if (!strcmp(lnk, "/dev/null") || !strcmp(base, name) ||
		    unitkind(base) != unitkind(name))
			return 0;
		snprintf(buf, size, "%s", base);
		return 1;
	}
	return 0;
}

/* %n name, %N name without suffix, %p prefix, %i and %I instance, %% */
static char *
expand(const char *name, const char *s)
{
	const char *at = strchr(name, '@'), *dot = strrchr(name, '.');
	Buf b = { 0 };

	bufadd(&b, "", 0);
	for (; *s; s++) {
		if (*s != '%' || !s[1]) {
			bufadd(&b, s, 1);
			continue;
		}
		switch (*++s) {
		case 'n':
			bufadd(&b, name, strlen(name));
			break;
		case 'N':
			bufadd(&b, name, dot - name);
			break;
		case 'p':
			bufadd(&b, name, (at ? at : dot) - name);
			break;
		case 'i':
		case 'I':
			if (at)
				bufadd(&b, at + 1, dot - at - 1);
			break;
		default:
			bufadd(&b, s - 1, 2);
		}
	}
	return b.s;
}

static int
setstr(void *p, char *v)
{
	char **s = p;

	free(*s);
	*s = *v ? estrdup(v) : NULL;
	return 0;
}

/* Each assignment appends; an empty one resets the list. */
static int
addline(void *p, char *v)
{
	if (!*v)
		lfree(p);
	else
		ladd(p, v);
	return 0;
}

static int
addnames(void *p, char *v)
{
	char *t;
	int r = 0;

	if (!*v)
		lfree(p);
	for (t = strtok(v, " \t"); t; t = strtok(NULL, " \t")) {
		if (unitkind(t) < 0)
			r = -1;
		else if (!lhas(p, t))
			ladd(p, t);
	}
	return r;
}

static int
addenv(void *p, char *v)
{
	List l = { 0 };
	size_t i;
	int r = 0;

	if (!*v)
		lfree(p);
	if (splitwords(v, &l) < 0)
		return -1;
	for (i = 0; i < l.n; i++) {
		if (!strchr(l.v[i], '=') || l.v[i][0] == '=')
			r = -1;
		else
			ladd(p, l.v[i]);
	}
	lfree(&l);
	return r;
}

static int
setbool(void *p, char *v)
{
	if (!strcmp(v, "yes") || !strcmp(v, "true") || !strcmp(v, "on") || !strcmp(v, "1"))
		*(int *)p = 1;
	else if (!strcmp(v, "no") || !strcmp(v, "false") || !strcmp(v, "off") || !strcmp(v, "0"))
		*(int *)p = 0;
	else
		return -1;
	return 0;
}

static int
setint(void *p, char *v)
{
	char *e;
	long n = strtol(v, &e, 10);

	if (!*v || *e || n < 0 || n > INT_MAX)
		return -1;
	*(int *)p = n;
	return 0;
}

/* "90", "1min 30s", "500ms", "infinity" (0: no limit) */
static int
settime(void *p, char *v)
{
	static const struct {
		char *unit;
		long ms;
	} units[] = {
		{ "ms", 1 }, { "msec", 1 }, { "", 1000 }, { "s", 1000 }, { "sec", 1000 },
		{ "m", 60000 }, { "min", 60000 }, { "h", 3600000 }, { "hr", 3600000 },
	};
	long total = 0, n;
	size_t i, k;
	char *e;

	if (!strcmp(v, "infinity")) {
		*(long *)p = 0;
		return 0;
	}
	if (!*v)
		return -1;
	while (*v) {
		if (!isdigit((unsigned char)*v))
			return -1;
		n = strtol(v, &e, 10);
		for (k = 0; isalpha((unsigned char)e[k]); k++)
			;
		for (i = 0; i < LEN(units); i++)
			if (strlen(units[i].unit) == k && !strncmp(e, units[i].unit, k))
				break;
		if (i == LEN(units))
			return -1;
		total += n * units[i].ms;
		for (v = e + k; isspace((unsigned char)*v); v++)
			;
	}
	*(long *)p = total;
	return 0;
}

static int
settype(void *p, char *v)
{
	if (!strcmp(v, "simple") || !strcmp(v, "exec"))
		*(int *)p = SIMPLE;
	else if (!strcmp(v, "oneshot"))
		*(int *)p = ONESHOT;
	else
		return -1;
	return 0;
}

static int
setrestart(void *p, char *v)
{
	if (!strcmp(v, "no"))
		*(int *)p = RESTART_NO;
	else if (!strcmp(v, "on-failure"))
		*(int *)p = RESTART_ONFAILURE;
	else if (!strcmp(v, "always"))
		*(int *)p = RESTART_ALWAYS;
	else
		return -1;
	return 0;
}

static int
setsig(void *p, char *v)
{
	size_t i;

	for (i = 0; i < LEN(signals); i++) {
		if (!strcmp(v, signals[i].name) || !strcmp(v, signals[i].name + 3)) {
			*(int *)p = signals[i].sig;
			return 0;
		}
	}
	return setint(p, v);
}

static int
setumask(void *p, char *v)
{
	char *e;
	long n = strtol(v, &e, 8);

	if (!*v || *e || n < 0 || n > 0777)
		return -1;
	*(int *)p = n;
	return 0;
}

static int
setio(void *p, char *v)
{
	if (!strcmp(v, "inherit"))
		*(int *)p = IO_INHERIT;
	else if (!strcmp(v, "null"))
		*(int *)p = IO_NULL;
	else if (!strcmp(v, "journal") || !strcmp(v, "kmsg") || !strcmp(v, "journal+console"))
		*(int *)p = IO_LOG;
	else if (!strcmp(v, "console"))
		*(int *)p = IO_CONSOLE;
	else if (!strcmp(v, "tty") || !strcmp(v, "tty-force"))
		*(int *)p = IO_TTY;
	else
		return -1;
	return 0;
}

static int
ignore(void *p, char *v)
{
	(void)p;
	(void)v;
	return 0;
}

#define O(m) offsetof(Conf, m)
static const struct {
	char *sect, *key;
	int (*set)(void *, char *);
	size_t off;
} keys[] = {
	{ "Unit",    "Description",           setstr,     O(desc) },
	{ "Unit",    "Documentation",         ignore,     0 },
	{ "Unit",    "Wants",                 addnames,   O(dep[DEP_WANTS]) },
	{ "Unit",    "Requires",              addnames,   O(dep[DEP_REQUIRES]) },
	{ "Unit",    "After",                 addnames,   O(dep[DEP_AFTER]) },
	{ "Unit",    "Before",                addnames,   O(dep[DEP_BEFORE]) },
	{ "Unit",    "Conflicts",             addnames,   O(dep[DEP_CONFLICTS]) },
	{ "Unit",    "StartLimitBurst",       setint,     O(burst) },
	{ "Unit",    "StartLimitIntervalSec", settime,    O(interval) },
	{ "Service", "Type",                  settype,    O(type) },
	{ "Service", "RemainAfterExit",       setbool,    O(remain) },
	{ "Service", "Restart",               setrestart, O(restart) },
	{ "Service", "RestartSec",            settime,    O(restartsec) },
	{ "Service", "TimeoutStopSec",        settime,    O(stoptimeout) },
	{ "Service", "KillSignal",            setsig,     O(killsig) },
	{ "Service", "ExecStartPre",          addline,    O(pre) },
	{ "Service", "ExecStart",             addline,    O(start) },
	{ "Service", "ExecStop",              addline,    O(stop) },
	{ "Service", "ExecReload",            addline,    O(reload) },
	{ "Service", "Environment",           addenv,     O(env) },
	{ "Service", "EnvironmentFile",       addline,    O(envfile) },
	{ "Service", "User",                  setstr,     O(user) },
	{ "Service", "Group",                 setstr,     O(group) },
	{ "Service", "WorkingDirectory",      setstr,     O(workdir) },
	{ "Service", "UMask",                 setumask,   O(umask) },
	{ "Service", "StandardInput",         setio,      O(in) },
	{ "Service", "StandardOutput",        setio,      O(out) },
	{ "Service", "StandardError",         setio,      O(err) },
	{ "Service", "TTYPath",               setstr,     O(tty) },
	{ "Install", "WantedBy",              addnames,   O(wantedby) },
};

void
confinit(Conf *c)
{
	memset(c, 0, sizeof(*c));
	c->burst = STARTBURST;
	c->interval = STARTINTERVAL;
	c->type = SIMPLE;
	c->restart = RESTART_NO;
	c->restartsec = RESTARTSEC;
	c->stoptimeout = STOPTIMEOUT;
	c->killsig = SIGTERM;
	c->umask = 022;
	c->in = IO_NULL;
	c->out = IO_LOG;
	c->err = IO_INHERIT;
}

/* Fills c from the file at path. Mistakes in single lines are reported
 * and skipped, as systemd does; -1 only if the unit cannot work at all.
 * c is valid (and must be freed) either way. */
int
unitparse(Conf *c, const char *name, const char *path)
{
	FILE *fp;
	Buf b = { 0 };
	char *line = NULL, *l, *v, *x, sect[32] = "";
	size_t cap = 0, lineno = 0, i;
	ssize_t len;
	int kind = unitkind(name), skip = 0, r = 0;

	confinit(c);
	if (!(fp = fopen(path, "re"))) {
		warn("%s:", path);
		return -1;
	}
	while ((len = getline(&line, &cap, fp)) != -1) {
		lineno++;
		if (len && line[len - 1] == '\n')
			line[--len] = '\0';
		/* a trailing backslash joins the next line */
		if (len && line[len - 1] == '\\') {
			line[len - 1] = ' ';
			bufadd(&b, line, len);
			continue;
		}
		bufadd(&b, line, len);
		l = trim(b.s);
		if (!*l || *l == '#' || *l == ';')
			goto next;
		if (*l == '[') {
			if (!(v = strchr(l, ']')) || v[1]) {
				warn("%s:%zu: bad section header", path, lineno);
				goto next;
			}
			*v = '\0';
			snprintf(sect, sizeof(sect), "%s", l + 1);
			skip = strcmp(sect, "Unit") && strcmp(sect, "Install") &&
			       (kind != UNIT_SERVICE || strcmp(sect, "Service"));
			if (skip)
				warn("%s:%zu: unknown section [%s]", path, lineno, sect);
			goto next;
		}
		if (skip)
			goto next;
		if (!(v = strchr(l, '='))) {
			warn("%s:%zu: no '=' in line", path, lineno);
			goto next;
		}
		*v++ = '\0';
		l = trim(l);
		v = trim(v);
		if (!strncmp(l, "X-", 2))
			goto next;
		for (i = 0; i < LEN(keys); i++)
			if (!strcmp(keys[i].sect, sect) && !strcmp(keys[i].key, l))
				break;
		if (i == LEN(keys)) {
			warn("%s:%zu: unknown key %s in [%s]", path, lineno, l, sect);
			goto next;
		}
		x = expand(name, v);
		if (keys[i].set((char *)c + keys[i].off, x) < 0)
			warn("%s:%zu: bad value for %s: %s", path, lineno, l, x);
		free(x);
next:
		bufreset(&b);
	}
	free(line);
	free(b.s);
	fclose(fp);

	if (kind == UNIT_SERVICE && c->type == SIMPLE && c->start.n != 1) {
		warn("%s: Type=simple needs exactly one ExecStart=", path);
		r = -1;
	}
	return r;
}

static void
adddir(Conf *c, int dep, const char *dir)
{
	struct dirent *e;
	DIR *d;

	if (!(d = opendir(dir)))
		return;
	while ((e = readdir(d)))
		if (unitkind(e->d_name) >= 0 && !lhas(&c->dep[dep], e->d_name))
			ladd(&c->dep[dep], e->d_name);
	closedir(d);
}

/* Adds what <name>.wants/ and <name>.requires/ link to, in every
 * directory of unitpath, for the template too. */
void
unitwants(Conf *c, const char *name)
{
	char tmpl[NAME_MAX + 1], dir[PATH_MAX];
	const char *try[2] = { name, NULL };
	size_t i, j;

	if (unittemplate(name, tmpl, sizeof(tmpl)) == 1)
		try[1] = tmpl;
	for (j = 0; j < LEN(try) && try[j]; j++) {
		for (i = 0; unitpath[i]; i++) {
			snprintf(dir, sizeof(dir), "%s%s/%s.wants", root, unitpath[i], try[j]);
			adddir(c, DEP_WANTS, dir);
			snprintf(dir, sizeof(dir), "%s%s/%s.requires", root, unitpath[i], try[j]);
			adddir(c, DEP_REQUIRES, dir);
		}
	}
}

void
conffree(Conf *c)
{
	size_t i;

	for (i = 0; i < NDEP; i++)
		lfree(&c->dep[i]);
	lfree(&c->wantedby);
	lfree(&c->pre);
	lfree(&c->start);
	lfree(&c->stop);
	lfree(&c->reload);
	lfree(&c->env);
	lfree(&c->envfile);
	free(c->desc);
	free(c->user);
	free(c->group);
	free(c->workdir);
	free(c->tty);
	confinit(c);
}
