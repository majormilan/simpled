/* See LICENSE file for copyright and license details. */

/* Unit files, searched in this order; the first one found wins. The first
 * directory is the administrator's: enable, disable and mask change it. */
#define UNITPATH   "/etc/simpled/system", PREFIX "/lib/simpled/system"

#define RUNDIR     "/run/simpled"         /* requests, pid file */
#define LOGDIR     "/var/log/simpled"     /* <unit>.log */
#define CONSOLE    "/dev/console"
#define DEFPATH    "/bin:/sbin:/usr/bin:/usr/sbin"
#define DEFTARGET  "default.target"
#define EMERGENCY  "emergency.service"    /* when DEFTARGET cannot load */

/* All times in milliseconds */
#define RESTARTSEC       100     /* RestartSec= */
#define STOPTIMEOUT      10000   /* TimeoutStopSec=, then SIGKILL */
#define STARTBURST       5       /* StartLimitBurst= */
#define STARTINTERVAL    10000   /* StartLimitIntervalSec= */
#define SHUTDOWNTIMEOUT  30000   /* stopping every unit at shutdown */
#define KILLTIMEOUT      3000    /* final SIGTERM to SIGKILL */
#define CTLTIMEOUT       10000   /* simplectl waiting for an answer */
