# simpled roadmap

Goal: miviex's own init. One PID 1 binary (`simpled`) and one control
tool (`simplectl`). It reads a subset of systemd's unit files and behaves
like systemd where it matters: units, targets, dependencies, restarts,
clean shutdown, `start`/`stop`/`status`/`enable`. It is written to the
suckless standard, in a few thousand lines of C at most, static and
linked against musl.

Each phase ends with a check that can be run on miviex itself (or on
Linux before miviex can run it).

## Design rules

- Single thread, one event loop. Signal handlers write one byte to a
  self-pipe; the loop `ppoll`s on it. miviex has `pipe2`, `ppoll` and
  `rt_sigprocmask`, but no `signalfd`, `epoll` or `timerfd`, so timeouts
  come from the `ppoll` timeout.
- PID 1 never exits and never calls `die()` after boot. On a fatal error
  it starts an emergency shell on the console.
- Every service runs in its own session (`setsid`), so its process group
  stands in for systemd's cgroup: stop means signalling `-pgid`.
- Paths, timeouts and the default target live in `config.def.h`, which
  is copied to `config.h` (the suckless convention). The unit structures
  move to `unit.h`.
- Everything that can be done on the filesystem is done there:
  `enable`/`disable` create and remove symlinks, so they need no daemon.
- The kernel is Linux-ABI, so it is developed and tested on Linux first:
  unprivileged with `-r <root>`, then as PID 1 under
  `unshare -fp --mount-proc`, then in QEMU on miviex.

## Phase 0: housekeeping

- `git init`; it builds with no warnings (`mkdir` needs `<sys/stat.h>`,
  `putenv` is XSI, `stop_service` does not exist, `simplectl.c` is
  empty).
- One code style. miviex uses suckless style (tabs, return type on its
  own line); simpled should match it.
- `config.mk` with `PREFIX`/`DESTDIR`, an `install` target, `CC=musl-gcc`
  and `-static`, header dependencies in the Makefile.
- Command-line options for testing: `-r` root prefix, `-n` not PID 1
  (skip mounts and `reboot`).

Done when `make` is clean with `-Wall -Wextra -pedantic` and simpled
runs a test unit directory as an ordinary user on Linux.

## Phase 1: a correct PID 1

- Event loop: self-pipe, `ppoll`, signals blocked outside it. Reaping
  happens in the loop, never in the handler, and reaps orphans too.
- The duties of `miviex/user/init.c`: mount `/proc` and `/run`, set the
  hostname, `reboot(RB_DISABLE_CAD)`.
- Spawning: `setsid`, stdin from `/dev/null`, a clean signal mask and
  dispositions, every inherited fd closed or `O_CLOEXEC`, `initgroups`
  then `setgid` then `setuid` with every return checked (a failed
  lookup must fail the unit, not run it as root), an absolute
  `ExecStart` run with `execve` and an environment built for it.
- `Restart=no|on-failure|always`, `RestartSec=`, and a start limit so a
  unit that dies at once does not loop forever.
- Log lines are unbuffered on the console.

Done when simpled replaces `user/init.c` in the miviex image, boots to
a login prompt on tty1 and ttyS0 through two units, and killing a getty
brings it back.

## Phase 2: units and dependencies

- A parser that reports `file:line` for unknown sections and keys,
  frees what it allocates and rejects over-long lines instead of
  splitting them.
- `ExecStart=` split with systemd's quoting rules (double quotes and
  backslashes; no shell). `%i`, `%n` specifiers.
- Search path `/etc/simpled/system` then `/lib/simpled/system`; a file
  in `/etc` masks one of the same name in `/lib`.
- `.target` units; `default.target` as a symlink.
- `Wants=`, `Requires=`, `After=`, `Before=`, `Conflicts=`;
  `[Install] WantedBy=` and `<target>.wants/` directories.
- Template units: `getty@.service` started as `getty@tty1`.
- `Type=oneshot` with `RemainAfterExit=`: units ordered after a oneshot
  start only once it has exited successfully.
- Start jobs: the set of units pulled in by the target, ordered by
  `After`/`Before`, cycles reported and broken. A failed `Requires=`
  fails its dependents.
- `Environment=`, `EnvironmentFile=`, `WorkingDirectory=`, `UMask=`.

Done when miviex boots `default.target` -> `sysinit.target` (a oneshot
that mounts `/etc/fstab`, hostname) -> `multi-user.target` with
`getty@tty1` and `getty@ttyS0`, a dependency cycle is reported without
hanging the boot, and a unit whose `Requires=` failed is not started.

## Phase 3: stopping and shutdown

- `ExecStop=`, `ExecReload=`, `KillSignal=`, `TimeoutStopSec=`:
  `SIGTERM` to the process group, `SIGKILL` once the timeout passes.
- Stopping follows the dependency order backwards; stopping a unit
  stops what `Requires=` it.
- Shutdown: stop all units, `kill(-1, SIGTERM)`, wait, `kill(-1,
  SIGKILL)`, `sync`, unmount (read-only remount for `/`), then
  `reboot(RB_POWER_OFF | RB_AUTOBOOT | RB_HALT_SYSTEM)`.
- Ctrl+Alt+Del (`SIGINT` to PID 1) reboots cleanly.

Done when `poweroff` from the shell stops every unit in order and powers
QEMU off, and a test unit that ignores `SIGTERM` is killed after its
timeout.

## Phase 4: simplectl

miviex has no Unix domain sockets until its phase 9, so the first
transport is a spool: `simplectl` writes a request to
`/run/simpled/req/<pid>.tmp`, renames it to `<pid>`, sends `SIGUSR1` to
PID 1 and waits for `/run/simpled/rep/<pid>`. The transport sits behind
one small interface so a socket can replace it later.

- `start`, `stop`, `restart`, `reload`, `status`, `list-units`,
  `is-active`, `is-enabled`, `daemon-reload`.
- `enable`, `disable`, `mask`, `unmask`: symlinks only.
- `poweroff`, `reboot`, `halt`; `rootfs/bin/{poweroff,reboot}` become
  links to simplectl.
- systemctl's exit codes (`status` returns 3 for an inactive unit), so
  scripts written for systemd keep working.

Done when `simplectl status getty@tty1` shows the state, pid, uptime,
last exit status and the last lines of its log, and `daemon-reload`
picks up a new unit without a reboot.

## Phase 5: logging

- Each service's stdout and stderr go to a pipe read by PID 1
  (non-blocking), written with timestamps to
  `/var/log/simpled/<unit>.log`, one rotation at a size limit.
- Lines from before `/var/log` is writable stay in a memory buffer and
  are flushed later.
- `StandardOutput=journal|console|null|file:` subset.
- `simplectl log <unit>` (with `-f`).

Done when every boot message and service line is in a file after boot,
and a service that floods its output cannot stall PID 1.

## Phase 6: once miviex has disks and sockets

- miviex phase 6: a disk root, `fsck` and remount read-write as units,
  logs that survive a reboot.
- miviex phase 9: `simplectl` over `/run/simpled.sock`; `Type=notify`
  (the `READY=1` subset of `sd_notify`).
- Optional, only when something needs it: `LimitNOFILE=`-style rlimits,
  `Type=forking` with `PIDFile=`, simple `.timer` units.

## Not planned

cgroups, D-Bus, a binary journal, socket activation, udev, logind,
`.mount`/`.device`/`.path` units, sandboxing options. These carry most
of systemd's size, and miviex does not need them.
