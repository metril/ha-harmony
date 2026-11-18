# shellcheck shell=sh
# shellcheck disable=SC2059,SC2317,SC1090,SC2046,SC2209,SC2016
# On-hub dry run of rcS.harmony under the hub's OWN BusyBox sh. Not sourced by anything on
# the device; uploaded to /tmp/hah-selftest by `install_boot_hook.py selftest`.
#
# Writes ONLY under $HAH_SELFTEST_DIR (default /tmp/hah-selftest). Every hook path is rooted
# there through HAH_ROOT, so /etc, /data and /cache are never touched. killall() and reboot()
# are shell functions, so the real ones cannot run. BusyBox floor: no timeout/head/wc/grep -E.
#
# usage: sh hook_selftest.sh /path/to/rcS.harmony
HOOK="${1:?usage: hook_selftest.sh /path/to/rcS.harmony}"
T="${HAH_SELFTEST_DIR:-/tmp/hah-selftest}"
R="$T/root"
fails=0

pass() { echo "PASS  $1"; }
fail() {
	echo "FAIL  $1"
	fails=$((fails + 1))
}

# fresh fake device root: daemon stub records its argv, conf given as $1 (printf format)
mkroot() {
	rm -rf "$R"
	mkdir -p "$R/etc" "$R/cache" "$R/data/ha-harmony" "$R/var/volatile"
	: >"$R/etc/nowatchdog"
	: >"$R/etc/ha-harmony-enable"
	printf '#!/bin/sh\necho "$@" > "%s/argv"\nexit 0\n' "$T" >"$R/data/ha-harmony/hub"
	chmod 755 "$R/data/ha-harmony/hub"
	printf "$1" >"$R/data/ha-harmony/hub.conf"
}

# Source the hook in a subshell of THIS shell; print DEMO_MODE. Hung cases are killed after
# ~10 s (no `timeout` applet on the hub) and reported as HANG.
run_hook() {
	rm -f "$T/out" "$T/argv"
	(
		killall() { :; }
		reboot() { echo reboot >>"$T/reboot.log"; }
		HAH_ROOT="$R" HAH_START_DELAY=0 HAH_HEALTHY_SECS=1 HAH_POLL_SECS=1
		HAH_POLL_SLEEP="${HAH_SELFTEST_NAP:-}" HAH_HEALTHY_POLLS="${HAH_SELFTEST_POLLS:-}"
		HAH_LOG_MAX_LINES="${SELFTEST_LOGMAX:-}"
		export HAH_ROOT HAH_START_DELAY HAH_HEALTHY_SECS HAH_POLL_SECS HAH_POLL_SLEEP HAH_HEALTHY_POLLS
		export HAH_LOG_MAX_LINES
		HAH_BACKOFF1="${HAH_SELFTEST_NAP:-1}" HAH_BACKOFF2="${HAH_SELFTEST_NAP:-1}" HAH_BACKOFF3="${HAH_SELFTEST_NAP:-1}"
		export HAH_BACKOFF1 HAH_BACKOFF2 HAH_BACKOFF3
		[ -n "${SELFTEST_CLOCK:-}" ] && export HAH_UPTIME_FILE="$SELFTEST_CLOCK"
		. "$HOOK"
		echo "DEMO=${DEMO_MODE:-unset}"
	) >"$T/out" 2>&1 &
	hp=$!
	hn=0
	while kill -0 "$hp" 2>/dev/null; do
		hn=$((hn + 1))
		if [ "$hn" -gt 20 ]; then
			kill "$hp" 2>/dev/null
			echo HANG >>"$T/out"
			break
		fi
		sleep "${HAH_SELFTEST_TICK:-1}"
	done
	sleep "${HAH_SELFTEST_SETTLE:-2}" # let the backgrounded daemon stub write its argv
	DEMO='unset'
	[ -r "$T/out" ] && while read -r l; do
		case "$l" in
		DEMO=*) DEMO="${l#DEMO=}" ;;
		esac
	done <"$T/out"
}

mkdir -p "$T" || exit 2
echo "selftest in $T (hook $HOOK)"

# 1. conf loop terminates, including a last line with no trailing newline
mkroot 'port=8099\ntoken=lastline'
run_hook
if [ "$DEMO" = 1 ] && [ "$(cat "$T/argv" 2>/dev/null)" = "--token-file $R/data/ha-harmony/hub.conf --port 8099 --heartbeat $R/var/volatile/ha-harmony.hb" ]; then
	pass "conf without trailing newline"
else
	fail "conf without trailing newline (DEMO=$DEMO argv=$(cat "$T/argv" 2>/dev/null))"
fi

# 2. counter values that are arithmetic errors or garbage count as clean
for c in '' 08 09 garbage '1 2'; do
	mkroot 'token=abc\n'
	printf '%s\n' "$c" >"$R/cache/ha-harmony-boot.n"
	run_hook
	n=$(cat "$R/cache/ha-harmony-boot.n" 2>/dev/null)
	if [ "$DEMO" = 1 ] && [ "$n" = 1 ]; then
		pass "counter '$c' counts as clean"
	else
		fail "counter '$c' (DEMO=$DEMO counter=$n)"
	fi
done

# 3. strike storage unavailable (no cache dir): stock must boot
mkroot 'token=abc\n'
rm -rf "$R/cache"
run_hook
if [ "$DEMO" = unset ]; then pass "missing cache dir boots stock"; else fail "missing cache dir (DEMO=$DEMO)"; fi

# 4. line counting with `read` (the log cap; byte size from `ls` is not portable: on this
#    hub `ls -ln` prints "-rw-r--r-- 1 0 0 0 0 50 name" and `ls -l` has no date columns)
rm -f "$T/sz"
i=0
while [ "$i" -lt 5 ]; do
	echo 123456789 >>"$T/sz"
	i=$((i + 1))
done
nl=0
while IFS= read -r l; do
	nl=$((nl + 1))
	[ "$nl" -gt 3 ] && break
done <"$T/sz" 2>/dev/null
if [ "$nl" = 4 ]; then pass "read-based line count stops over the cap"; else fail "line count (got '$nl'; raw ls: $(ls -ln "$T/sz" 2>&1))"; fi

# 5. optional argv pieces expand correctly (${var:+...})
mkroot 'token=abc==\nhub_id=my-hub\nrf=spi\nrf_slot=2\nport=8100\n'
run_hook
want="--token-file $R/data/ha-harmony/hub.conf --port 8100 --hub-id my-hub --rf-spi --rf-slot 2 --heartbeat $R/var/volatile/ha-harmony.hb"
if [ "$DEMO" = 1 ] && [ "$(cat "$T/argv" 2>/dev/null)" = "$want" ]; then
	pass "argv quoting with rf, hub_id and '=' in token"
else
	fail "argv quoting (DEMO=$DEMO argv=$(cat "$T/argv" 2>/dev/null))"
fi

# 5b. the token never appears in the daemon argv
if ! grep -q 'abc==' "$T/argv" 2>/dev/null; then pass "token absent from daemon argv"; else fail "token leaked into argv"; fi

# 6. junk config leaves stock alone, no strike
mkroot 'token=abc\nrf_slot=2x\n'
run_hook
if [ "$DEMO" = unset ] && [ ! -e "$R/cache/ha-harmony-boot.n" ]; then
	pass "bad value: stock, no strike"
else
	fail "bad value (DEMO=$DEMO)"
fi

# 6b. token over 256 chars: stock, no strike; exactly 256 arms
long=x
i=0
while [ "$i" -lt 8 ]; do # 2^8 = 256 chars, then one more
	long="$long$long"
	i=$((i + 1))
done
long="${long}x"
mkroot "token=${long}\n"
run_hook
if [ "$DEMO" = unset ] && [ ! -e "$R/cache/ha-harmony-boot.n" ]; then
	pass "token over 256: stock, no strike"
else
	fail "token over 256 (DEMO=$DEMO)"
fi

# 7. printf '\r' yields one carriage return
cr=$(printf '\r')
if [ -n "$cr" ]; then pass "printf \\r"; else fail "printf \\r"; fi

# 8. nowatchdog absent: stock
mkroot 'token=abc\n'
rm -f "$R/etc/nowatchdog"
run_hook
if [ "$DEMO" = unset ]; then pass "stock watchdog enabled: stock"; else fail "watchdog guard (DEMO=$DEMO)"; fi

# 9. the daemon log is capped in place while the daemon keeps writing (O_APPEND)
mkroot 'token=abc\nrespawn_max=0\n' # no respawn: a leftover watcher must not relaunch later stubs
printf '#!/bin/sh\ni=0\nwhile [ $i -lt 30 ]; do echo line$i; i=$((i+1)); done\nsleep "%s"\necho AFTER\nsleep "%s"\n' "${HAH_SELFTEST_GAP:-4}" "${HAH_SELFTEST_GAP:-4}" >"$R/data/ha-harmony/hub"
SELFTEST_LOGMAX=10
export SELFTEST_LOGMAX
run_hook
sleep "${HAH_SELFTEST_GAP:-4}"
nl=0
while IFS= read -r l; do
	nl=$((nl + 1))
done <"$R/var/volatile/ha-harmony.log" 2>/dev/null
if [ "$nl" -lt 30 ] && [ "$nl" -ge 1 ]; then pass "log capped in place ($nl lines left of 30+)"; else fail "log cap (lines=$nl)"; fi
unset SELFTEST_LOGMAX

# --- bounded respawn (after the health mark), wedge cover ---------------------------------
GAP="${HAH_SELFTEST_GAP:-4}"
TICK="${HAH_SELFTEST_TICK:-1}"
CLOG() { echo "$R/cache/ha-harmony.log"; }

# wait_log PATTERN: poll the hook log up to ~200 ticks for a line containing PATTERN
wait_log() {
	wl=0
	while [ "$wl" -lt 200 ]; do
		if [ -r "$(CLOG)" ]; then
			while IFS= read -r l; do
				case "$l" in
				*"$1"*) return 0 ;;
				esac
			done <"$(CLOG)"
		fi
		wl=$((wl + 1))
		sleep "$TICK"
	done
	return 1
}

runs() {
	n=0
	[ -r "$T/runs" ] && while IFS= read -r l; do n=$((n + 1)); done <"$T/runs"
	echo "$n"
}

# stub that counts launches and dies $GAP seconds after each start
mortal() {
	printf '#!/bin/sh\necho x >> "%s/runs"\nsleep "%s"\nexit 1\n' "$T" "$GAP" >"$R/data/ha-harmony/hub"
	chmod 755 "$R/data/ha-harmony/hub"
	rm -f "$T/runs" "$T/reboot.log"
}

# 10. death after the health mark respawns; the budget runs out; no reboot; strike untouched
mkroot 'token=abc\nrespawn_max=1\n'
mortal
run_hook
wait_log "budget exhausted"
sleep "$GAP"
if [ "$(runs)" = 2 ] && [ ! -e "$T/reboot.log" ] && [ ! -e "$R/cache/ha-harmony-boot.n" ]; then
	pass "respawn_max=1: one respawn, then inert; no reboot, strikes untouched"
else
	fail "respawn budget (runs=$(runs) reboot=$([ -e "$T/reboot.log" ] && echo yes || echo no) counter=$(cat "$R/cache/ha-harmony-boot.n" 2>/dev/null))"
fi

# 11. respawn_max=0 is the old behaviour
mkroot 'token=abc\nrespawn_max=0\n'
mortal
run_hook
wait_log "no respawn"
sleep "$GAP"
if [ "$(runs)" = 1 ] && [ ! -e "$T/reboot.log" ]; then pass "respawn_max=0: no respawn"; else fail "respawn_max=0 (runs=$(runs))"; fi

# 12. stale heartbeat: the daemon is alive but silent, so it is killed and respawned
mkroot 'token=abc\nrespawn_max=1\n'
mortal
printf '1000.00 1.00\n' >"$T/clock"
SELFTEST_CLOCK="$T/clock"
export SELFTEST_CLOCK
run_hook
wait_log "healthy"
sleep "${HAH_SELFTEST_NAP:-1}"
sleep "${HAH_SELFTEST_NAP:-1}"
printf '1100.00 1.00\n' >"$T/clock"
if wait_log "heartbeat stale" && [ ! -e "$T/reboot.log" ] && [ ! -e "$R/cache/ha-harmony-boot.n" ]; then
	pass "stale heartbeat: killed, no reboot, strikes untouched"
else
	fail "stale heartbeat"
fi
unset SELFTEST_CLOCK
sleep "$GAP"
sleep "$GAP"

sleep "${HAH_SELFTEST_FINAL:-3}" # let the 1 s watchers finish before the caller deletes the directory
echo "selftest done: $fails failure(s)"
[ "$fails" = 0 ]
