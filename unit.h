/* See LICENSE file for copyright and license details. */
enum { UNIT_SERVICE, UNIT_TARGET };
enum { LOAD_OK, LOAD_NOTFOUND, LOAD_MASKED, LOAD_ERROR };
enum { DEP_WANTS, DEP_REQUIRES, DEP_AFTER, DEP_BEFORE, DEP_CONFLICTS, NDEP };
enum { SIMPLE, ONESHOT };
enum { RESTART_NO, RESTART_ONFAILURE, RESTART_ALWAYS };
enum { IO_INHERIT, IO_NULL, IO_LOG, IO_CONSOLE, IO_TTY };

/* What a unit file says */
typedef struct {
	char *desc;
	List dep[NDEP];          /* unit names */
	List wantedby;
	int burst;
	long interval;
	int type, restart, remain;
	List pre, start, stop, reload;
	List env, envfile;
	char *user, *group, *workdir, *tty;
	int in, out, err;
	int umask, killsig;
	long restartsec, stoptimeout;
} Conf;

extern const char *unitpath[];

int unitkind(const char *);
int unittemplate(const char *, char *, size_t);
int unitalias(const char *, char *, size_t);
int unitfind(const char *, char *, size_t);
void confinit(Conf *);
int unitparse(Conf *, const char *, const char *);
void unitwants(Conf *, const char *);
void conffree(Conf *);
