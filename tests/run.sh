#!/bin/sh
# Runs simpled on a scratch root, first as an ordinary process, then as
# PID 1 of a new PID namespace, and checks what it does.

top=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
R=$T/root
U=$R/etc/simpled/system
pass=0
fail=0

cleanup() {
	[ -n "${pid:-}" ] && kill -9 "$pid" 2>/dev/null
	pkill -9 -f "sleep 1001" 2>/dev/null
	rm -rf "$T"
}
trap cleanup EXIT

ok() {
	pass=$((pass + 1))
	echo "ok   $1"
}

no() {
	fail=$((fail + 1))
	echo "FAIL $1"
}

check() {
	name=$1
	shift
	if "$@"; then ok "$name"; else no "$name"; fi
}

# waitfor command...: retries for up to 5 seconds
waitfor() {
	i=0
	until "$@"; do
		i=$((i + 1))
		[ $i -gt 50 ] && return 1
		sleep 0.1
	done
}

ctl() {
	"$top/simplectl" -r "$R" "$@"
}

active() {
	[ "$(ctl is-active "$1" 2>/dev/null)" = "${2:-active}" ]
}

unit() {
	cat > "$U/$1"
}

mkdir -p "$U/test.target.wants" "$R/var/log" "$R/run"
cp "$top"/units/* "$U"
ln -s test.target "$U/default.target"

unit test.target <<EOF
[Unit]
Description=Test target
Wants=app.service crash.service dependent.service cyc-a.service cyc-b.service
Wants=stubborn.service env.service
EOF
ln -s ../inst@.service "$U/test.target.wants/inst@one.service"

unit prep.service <<EOF
[Unit]
Description=Preparation
[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/bin/sh -c 'echo prep > $T/prep'
ExecStop=/bin/sh -c 'test -e $T/app.stop && echo after > $T/prep.stop'
EOF

unit app.service <<EOF
[Unit]
Description=The app
Requires=prep.service
After=prep.service
[Service]
ExecStart=/bin/sh -c 'test -e $T/prep && echo ordered > $T/app.order; \\
	exec sleep 1001'
ExecStop=/bin/sh -c 'echo \$MAINPID > $T/app.stop'
X-Ignored=yes
Bogus=1
EOF

unit crash.service <<EOF
[Unit]
StartLimitBurst=3
StartLimitIntervalSec=1min
[Service]
ExecStart=/bin/sh -c 'echo x >> $T/crash.count; exit 1'
Restart=on-failure
RestartSec=50ms
EOF

unit broken.service <<EOF
[Service]
Type=oneshot
ExecStart=/bin/false
EOF

unit dependent.service <<EOF
[Unit]
Requires=broken.service
After=broken.service
[Service]
ExecStart=/bin/sh -c 'touch $T/dependent.ran; exec sleep 1001'
EOF

unit cyc-a.service <<EOF
[Unit]
After=cyc-b.service
[Service]
ExecStart=/bin/sleep 1001
EOF

unit cyc-b.service <<EOF
[Unit]
After=cyc-a.service
[Service]
ExecStart=/bin/sleep 1001
EOF

unit stubborn.service <<EOF
[Service]
ExecStart=/bin/sh -c 'trap "" TERM; while :; do sleep 0.1; done'
TimeoutStopSec=500ms
EOF

unit env.service <<EOF
[Service]
Type=oneshot
Environment="A=hello world" B=2
EnvironmentFile=$T/envfile
EnvironmentFile=-$T/missing
ExecStart=/bin/sh -c 'echo "\$A|\$B|\$C|\$PATH"'
EOF
echo 'C="from file"' > "$T/envfile"

unit inst@.service <<EOF
[Unit]
Description=Instance %i
[Service]
ExecStart=/bin/sh -c 'echo %i %n %p > $T/inst; exec sleep 1001'
EOF

L=$R/usr/local/lib/simpled/system
mkdir -p "$L"
cat > "$L/late.service" <<EOF
[Service]
ExecStart=/bin/sleep 1001
[Install]
WantedBy=test.target
EOF

echo "== simpled as a process"
"$top/simpled" -r "$R" > "$T/log" 2>&1 &
pid=$!

check "default.target is an alias of test.target" waitfor active test.target
check "Requires+After: oneshot ran before app" waitfor test -s "$T/app.order"
check "app is running" waitfor active app.service
check "instance of a template, specifiers" \
	waitfor grep -qx "one inst@one.service inst" "$T/inst"
check "environment from Environment= and EnvironmentFile=" \
	waitfor grep -qx "hello world|2|from file|/bin:/sbin:/usr/bin:/usr/sbin" \
	"$R/var/log/simpled/env.service.log"
check "restart on failure stops at the start limit" waitfor active crash.service failed
check "  ... after exactly 3 starts" test "$(wc -l < "$T/crash.count")" -eq 3
check "a failed Requires= unit fails its dependent" waitfor active dependent.service failed
check "  ... which never ran" test ! -e "$T/dependent.ran"
check "an ordering cycle is reported precisely" \
	grep -Eq "ordering cycle: cyc-(a|b).service -> .*, starting" "$T/log"
check "  ... and broken" waitfor active cyc-a.service
check "  ... both ways" waitfor active cyc-b.service
check "unknown keys are reported with file:line" grep -q "app.service:10: unknown key Bogus" "$T/log"

check "status of a running unit returns 0" sh -c "'$top/simplectl' -r '$R' status app >/dev/null"
ctl status app > "$T/status"
check "  ... and shows it running" grep -q "active (running)" "$T/status"
ctl is-active crash >/dev/null
check "is-active of a failed unit returns 3" test $? -eq 3

ctl stop stubborn
check "a unit ignoring SIGTERM is killed after TimeoutStopSec" waitfor active stubborn.service inactive
check "  ... and it says so" grep -q "stubborn.service: stop timed out" "$T/log"

ctl stop app
check "stop runs ExecStop with \$MAINPID" waitfor test -s "$T/app.stop"
check "  ... and the unit is inactive" waitfor active app.service inactive
ctl start app
check "start brings it back" waitfor active app.service
oldpid=$(sed -n 's/.*Main PID: //p' "$T/status")
ctl restart app
check "restart gives a new main process" \
	waitfor sh -c "'$top/simplectl' -r '$R' status app | grep -q 'Main PID:' &&
	! '$top/simplectl' -r '$R' status app | grep -q 'Main PID: $oldpid\$'"

check "enable links into WantedBy= targets" sh -c "'$top/simplectl' -r '$R' enable late >/dev/null"
check "  ... is-enabled says enabled" test "$(ctl is-enabled late)" = enabled
check "  ... symlink is there" test -L "$U/test.target.wants/late.service"
ctl disable late >/dev/null
check "disable removes it" test "$(ctl is-enabled late)" = disabled
ctl mask late >/dev/null
check "mask" test "$(ctl is-enabled late)" = masked
ctl start late >/dev/null
check "  ... a masked unit does not start" test $? -eq 5
ctl unmask late >/dev/null
check "unmask" test "$(ctl is-enabled late)" = disabled

sed -i 's/Description=The app/Description=The renamed app/' "$U/app.service"
ctl daemon-reload
check "daemon-reload rereads unit files" sh -c "'$top/simplectl' -r '$R' status app | grep -q 'renamed app'"
check "  ... and keeps the running process" active app.service

rm -f "$T/app.stop"
kill -TERM $pid
check "SIGTERM: everything stops and simpled exits 0" waitfor sh -c "! kill -0 $pid 2>/dev/null"
wait $pid
check "  ... exit status 0" test $? -eq 0
check "  ... units stop in reverse order (app before prep)" grep -qx after "$T/prep.stop"
check "  ... no unit process is left" sh -c '! pgrep -f "[s]leep 1001" >/dev/null'
pid=

echo "== simpled as PID 1"
if unshare -Urfpmu --mount-proc true 2>/dev/null; then
	R2=$T/root2
	U=$R2/etc/simpled/system
	mkdir -p "$U" "$R2/var/log" "$R2/run"
	cp "$top"/units/* "$U"
	unit default.target <<EOF
[Unit]
Wants=orphans.service shut.service
EOF
	# leaves a grandchild behind for PID 1 to adopt and reap
	unit orphans.service <<EOF
[Service]
Type=oneshot
ExecStart=/bin/sh -c '(sleep 0.2 &) ; echo spawned'
EOF
	unit shut.service <<EOF
[Service]
Type=oneshot
ExecStart=/bin/sh -c 'sleep 1; echo \$\$ > $T/ns.pid; $top/simplectl -r $R2 poweroff'
EOF
	timeout 20 unshare -Urfpmu --mount-proc "$top/simpled" -r "$R2" > "$T/log1" 2>&1
	st=$?
	check "PID 1: poweroff goes through reboot(2) (status $st)" test $st -ne 124
	check "  ... units were stopped first" grep -q "all units stopped" "$T/log1"
	check "  ... then everything else was killed" grep -q "sending SIGKILL" "$T/log1"
	check "  ... after a clean run" sh -c "! grep -qi 'reboot:\\|ordering\\|failed' '$T/log1'"
else
	echo "skip PID 1 tests: no unprivileged user namespaces"
fi

echo "$pass passed, $fail failed"
[ $fail -eq 0 ] || { echo "--- simpled log:"; cat "$T/log" "$T/log1" 2>/dev/null; }
[ $fail -eq 0 ]
