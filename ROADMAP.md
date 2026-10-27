# simpled roadmap

Goal: miviex's own init. One PID 1 binary (`simpled`) and one control
tool (`simplectl`). It reads a subset of systemd's unit files and behaves
like systemd where it matters: units, targets, dependencies, restarts,
clean shutdown, `start`/`stop`/`status`/`enable`. It is written to the
suckless standard, in a few thousand lines of C at most, static and
linked against musl.

Each phase ends with a check that can be run on miviex itself (or on
Linux before miviex can run it). `make check` runs the Linux side.

## Design rules

- Single thread, one event loop. Signal handlers set a flag and write
  one byte to a self-pipe, only if none is pending: miviex pipes ignore
  `O_NONBLOCK` and its `ppoll` ignores the signal mask, so the handler
  must never be able to fill the pipe. Timeouts come from `poll`.
- PID 1 never exits; `die()` freezes it instead of panicking the kernel.
- Every service runs in its own session (`setsid`), so its process group
  stands in for systemd's cgroup: stop means signalling `-pgid`.
- `config.def.h` holds paths, timeouts and the default target as
  defines; the unit structures live in `unit.h`.
- Everything that can be done on the filesystem is done there:
  `enable`/`disable`/`mask` create and remove symlinks without the daemon.
- Linux-ABI first: tested unprivileged with `-r <root>`, then as PID 1
  under `unshare -Urfpmu`, then in QEMU on miviex.

## Done

- Phase 0: git, suckless layout and style, `config.mk` with
  `PREFIX`/`DESTDIR`, static musl builds; `-r` root prefix and `-t`
  target for testing; `make check`.
- Phase 1: the event loop; reaping in the loop, orphans included; PID 1
  mounts (`/proc`, `/run`), hostname, `RB_DISABLE_CAD`. Spawning with
  `setsid`, stdin from `/dev/null`, a clean signal mask, `O_CLOEXEC`
  everywhere, `initgroups`/`setgid`/`setuid` all checked (a bad
  `User=` fails the unit, it never runs as root), `execvp` with a built
  environment. `Restart=no|on-failure|always`, `RestartSec=`,
  `StartLimitBurst=`/`StartLimitIntervalSec=`.
- Phase 2: a parser reporting `file:line`, `\` continuations, quoting
  in `Exec*=`, specifiers `%n %N %p %i %I`; `/etc` over `/lib`;
  targets, aliases (`default.target`), templates; `Wants`, `Requires`,
  `After`, `Before`, `Conflicts`, `WantedBy` and `.wants/`/`.requires/`
  directories; oneshot with `RemainAfterExit=`; dependency failure;
  ordering cycles reported by their members and broken;
  `Environment=`, `EnvironmentFile=`, `WorkingDirectory=`, `UMask=`,
  `StandardInput/Output/Error=`, `TTYPath=`.
- Phase 3: `ExecStop=` (with `$MAINPID`), `ExecReload=`, `KillSignal=`,
  `TimeoutStopSec=` then `SIGKILL`; stops in reverse order, stopping a
  unit stops what requires it; shutdown: all units, `kill(-1)` TERM
  then KILL, unmount, read-only `/`, `reboot(2)`. SIGINT (Ctrl+Alt+Del)
  and SIGTERM reboot, SIGUSR2 powers off, SIGHUP reloads.
- Phase 4: `simplectl` over request files in `/run/simpled/req` and
  `SIGUSR1` (root only: users may not signal PID 1); `start`, `stop`
  and `restart` wait for their jobs and say why one failed, unless
  `--no-block`, and `^C` leaves nothing behind. Each unit's state is in
  `/run/simpled/units/<unit>`, rewritten when it changes, so `status`,
  `is-active` and `list` work for every user. `reload reset-failed
  daemon-reload poweroff reboot halt`; `enable disable is-enabled mask
  unmask cat get-default set-default` on the files alone; systemctl's
  exit codes; `poweroff`/`reboot`/`halt` links, `-f` to skip the units.
  Checked on miviex, as root and as the user miviex.
- In miviex: `/init` is simpled, built from `ext/simpled`; gettys on
  tty1 and ttyS0 and `fstab.service` are units. Checked in QEMU: boot in
  order, a killed getty comes back, `restart`, `poweroff` powers off.

## Phase 5: logging

- Each service's stdout and stderr go to a pipe read by PID 1 and
  written with timestamps to `/var/log/simpled/<unit>.log`, one
  rotation at a size limit. Today the service writes the file itself:
  no timestamps, no limit. Needs non-blocking pipes on miviex.
- Lines from before `/var/log` is writable stay in a memory buffer and
  are flushed later; simpled's own messages go there too.
- `simplectl log <unit>` (with `-f`).

Done when every boot message and service line is in a file after boot,
and a service that floods its output cannot stall PID 1.

## Phase 6: once miviex has disks and sockets

- miviex phase 6: a disk root, `fsck` and remount read-write as units,
  logs that survive a reboot.
- miviex phase 9: `simplectl` over `/run/simpled.sock`; `Type=notify`
  (the `READY=1` subset of `sd_notify`).
- Optional, only when something needs it: `ExecStartPost=`,
  `BindsTo=`, rlimits (`LimitNOFILE=`), `Type=forking` with `PIDFile=`,
  simple `.timer` units.

## Needed from the miviex kernel

Found while bringing simpled up; simpled works around all of them.

- `ppoll` should apply its signal mask atomically.
- Pipes should honour `O_NONBLOCK` (phase 5 needs it).
- init should not get the console as its controlling terminal, as on
  Linux (or `TIOCNOTTY` should exist): until a getty takes the console,
  `^C` typed there reaches PID 1 as SIGINT, which means reboot.

Also seen, outside simpled, in shush (on Linux too unless noted): `$?`
after `;` on the same line is always 0; `$(...)` inside double quotes is
a syntax error; `x=$(...)` is not an assignment (on miviex, with a pipe
inside, a page fault at 0); `if` is not a keyword. And on miviex the
serial console drops the first characters of lines typed into shush.

## Not planned

cgroups, D-Bus, a binary journal, socket activation, udev, logind,
`.mount`/`.device`/`.path` units, sandboxing options. These carry most
of systemd's size, and miviex does not need them.
