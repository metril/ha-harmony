# shellcheck shell=sh
# shellcheck disable=SC2059,SC2317,SC1090,SC2046,SC2209
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
		export HAH_ROOT HAH_START_DELAY HAH_HEALTHY_SECS HAH_POLL_SECS HAH_POLL_SLEEP HAH_HEALTHY_POLLS
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
if [ "$DEMO" = 1 ] && [ "$(cat "$T/argv" 2>/dev/null)" = "--token lastline --port 8099" ]; then
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

# 4. `ls -ln` size field used by the log cap
rm -f "$T/sz"
i=0
while [ "$i" -lt 5 ]; do
	echo 123456789 >>"$T/sz"
	i=$((i + 1))
done
set -- $(ls -ln "$T/sz")
if [ "$5" = 50 ]; then pass "ls -ln size field"; else fail "ls -ln size field (got '$5')"; fi

# 5. optional argv pieces expand correctly (${var:+...})
mkroot 'token=abc==\nhub_id=my-hub\nrf=spi\nrf_slot=2\nport=8100\n'
run_hook
want="--token abc== --port 8100 --hub-id my-hub --rf-spi --rf-slot 2"
if [ "$DEMO" = 1 ] && [ "$(cat "$T/argv" 2>/dev/null)" = "$want" ]; then
	pass "argv quoting with rf, hub_id and '=' in token"
else
	fail "argv quoting (DEMO=$DEMO argv=$(cat "$T/argv" 2>/dev/null))"
fi

# 6. junk config leaves stock alone, no strike
mkroot 'token=abc\nrf_slot=2x\n'
run_hook
if [ "$DEMO" = unset ] && [ ! -e "$R/cache/ha-harmony-boot.n" ]; then
	pass "bad value: stock, no strike"
else
	fail "bad value (DEMO=$DEMO)"
fi

# 7. printf '\r' yields one carriage return
cr=$(printf '\r')
if [ -n "$cr" ]; then pass "printf \\r"; else fail "printf \\r"; fi

# 8. nowatchdog absent: stock
mkroot 'token=abc\n'
rm -f "$R/etc/nowatchdog"
run_hook
if [ "$DEMO" = unset ]; then pass "stock watchdog enabled: stock"; else fail "watchdog guard (DEMO=$DEMO)"; fi

sleep "${HAH_SELFTEST_FINAL:-3}" # let the 1 s watchers finish before the caller deletes the directory
echo "selftest done: $fails failure(s)"
[ "$fails" = 0 ]
