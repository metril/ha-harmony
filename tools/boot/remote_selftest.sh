# shellcheck shell=sh
# On-device dry run of start.sh under the remote's OWN BusyBox sh (1.13.4: no head/tail/wc/dd/
# find). Uploaded by `deploy_remote.py selftest` to /var/volatile/hah-selftest (tmpfs) and run
# from there; also run on the host by the tests. Writes ONLY under $T. Every start.sh run is
# rooted at $T/app via APP_DIR/APP_BIN/APP_LOG, so the real /data/app is never touched; the
# real bootcount is compared before and after as a tripwire. Nothing here is ever sourced.
#
# usage: sh remote_selftest.sh T "--slot 1 --powermode 0x30"
T="${1:?usage: remote_selftest.sh DIR ARGV}"
WANT="${2:?usage: remote_selftest.sh DIR ARGV}"
A="$T/app"
fails=0
# Waits: device defaults (whole seconds, BusyBox 1.13); the host tests shrink them via env.
ST_TICK="${ST_TICK:-1}"
ST_NAP="${ST_NAP:-3}"
REAL_COUNT_FILE="${REAL_COUNT_FILE:-/data/app/bootcount}"

pass() { echo "PASS  $1"; }
fail() {
	echo "FAIL  $1"
	fails=$((fails + 1))
}

real_count() {
	if [ -f "$REAL_COUNT_FILE" ]; then
		cat "$REAL_COUNT_FILE" 2>/dev/null
	else
		echo absent
	fi
}
REAL_BEFORE=$(real_count)

# fresh fake /data/app: stub app records argv and one line per launch
mkapp() {
	rm -rf "$A"
	mkdir -p "$A" || exit 2
	cp "$T/start.sh" "$A/start.sh"
	chmod 755 "$A/start.sh"
	printf '#!/bin/sh\necho "$@" > "%s/argv"\necho x >> "%s/ran"\n%s\nexit 0\n' "$T" "$T" "${1:-}" >"$A/harmony_remote"
	chmod 755 "$A/harmony_remote"
	rm -f "$T/argv" "$T/ran"
}

# one boot of start.sh, fully rooted at $A, with a short watcher tick
boot() {
	APP_DIR="$A" APP_BIN="$A/harmony_remote" APP_LOG="${BOOT_LOG:-$T/app.log}" \
		START_TICK="$ST_TICK" START_LOG_MAX_LINES="${BOOT_LOGMAX:-}" sh "$A/start.sh" >/dev/null 2>&1
}

launches() {
	n=0
	if [ -f "$T/ran" ]; then
		while read -r _l; do n=$((n + 1)); done <"$T/ran"
	fi
	echo "$n"
}

counter() {
	c=''
	[ -f "$A/bootcount" ] && read -r c <"$A/bootcount"
	echo "$c"
}

echo "remote selftest in $T (argv: $WANT)"

# 0. applet floor: everything the installer and start.sh run on the device
missing=''
for c in mv chmod mkdir rm cat md5sum ps sleep kill uname reboot ls cp sh; do
	type "$c" >/dev/null 2>&1 || missing="$missing $c"
done
if [ -z "$missing" ]; then pass "required commands present"; else fail "missing commands:$missing"; fi

# 0b. `sh -n` really parses (a no-op -n would let a broken rcS.local through)
printf 'if true; then\necho x\n' >"$T/bad.sh"
printf 'echo ok\n' >"$T/good.sh"
if sh -n "$T/good.sh" 2>/dev/null && ! sh -n "$T/bad.sh" 2>/dev/null; then
	pass "sh -n accepts good, rejects broken"
else
	fail "sh -n does not discriminate"
fi
r=$(sh -c ". '$T/bad.sh'; echo AFTER" 2>/dev/null)
echo "INFO  sourcing a syntax-broken file: shell printed '${r:-nothing}' afterwards (nothing = it aborted the caller)"

# 1. three failed boots launch; the fourth disables
mkapp
ok=1
for i in 1 2 3; do
	boot
	[ "$(counter)" = "$i" ] && [ "$(launches)" = "$i" ] && [ ! -e "$A/DISABLE" ] || ok=0
done
boot
if [ "$ok" = 1 ] && [ -e "$A/DISABLE" ] && [ "$(launches)" = 3 ] && [ "$(counter)" = 4 ]; then
	pass "three failed boots then disabled"
else
	fail "boot counter (ok=$ok launches=$(launches) counter=$(counter) disable=$([ -e "$A/DISABLE" ] && echo yes || echo no))"
fi

# 2. DISABLE: app not started, counter untouched
mkapp
: >"$A/DISABLE"
boot
if [ "$(launches)" = 0 ] && [ ! -e "$A/bootcount" ]; then pass "DISABLE: not started, not counted"; else fail "DISABLE (launches=$(launches))"; fi

# 3. mark-good resets
mkapp
boot
boot
boot
echo 0 >"$A/bootcount"
boot
if [ "$(launches)" = 4 ] && [ "$(counter)" = 1 ] && [ ! -e "$A/DISABLE" ]; then
	pass "mark-good resets the counter"
else
	fail "mark-good (launches=$(launches) counter=$(counter))"
fi

# 4. counter garbage / empty / 08 / 09 / spaces / huge: counts as clean, never an arithmetic error
for c in '' 08 09 banana '1 2' 0003 99999999999999999999 '-1'; do
	mkapp
	printf '%s\n' "$c" >"$A/bootcount"
	boot
	if [ "$(launches)" = 1 ] && [ "$(counter)" = 1 ] && [ ! -e "$A/DISABLE" ]; then
		pass "counter '$c' counts as clean"
	else
		fail "counter '$c' (launches=$(launches) counter=$(counter))"
	fi
done
mkapp
: >"$A/bootcount" # empty, no newline
boot
if [ "$(launches)" = 1 ] && [ "$(counter)" = 1 ]; then pass "counter file empty (no newline)"; else fail "empty counter file"; fi

# 5. argv exactly as configured
mkapp
boot
got=$(cat "$T/argv" 2>/dev/null)
if [ "$got" = "$WANT" ]; then pass "argv: $got"; else fail "argv (got '$got', want '$WANT')"; fi

# 6. counter unwritable (a directory): fail safe, app NOT launched, start.sh still exits cleanly
mkapp
mkdir "$A/bootcount"
boot
rc=$?
if [ "$rc" = 0 ] && [ "$(launches)" = 0 ]; then pass "unwritable counter: no launch, rc 0"; else fail "unwritable counter (rc=$rc launches=$(launches))"; fi

# 7. log unwritable: the app still starts (log falls back to /dev/null)
mkapp
BOOT_LOG="$T/no/such/dir/app.log" boot
if [ "$(launches)" = 1 ]; then pass "unwritable log: app still launched"; else fail "unwritable log (launches=$(launches))"; fi

# 8. log capped in place while the app keeps writing
mkapp "i=0; while [ \$i -lt 40 ]; do echo line\$i; i=\$((i+1)); done; sleep $ST_NAP; echo AFTER; sleep $ST_NAP"
rm -f "$T/capped.log"
BOOT_LOG="$T/capped.log" BOOT_LOGMAX=10 boot
nl=0
while read -r _l; do nl=$((nl + 1)); done <"$T/capped.log"
if [ "$nl" -ge 1 ] && [ "$nl" -lt 40 ]; then pass "log capped in place ($nl of 41 lines left)"; else fail "log cap (lines=$nl)"; fi

# 8b. every app line and start.sh's own line carry an uptime stamp
mkapp "echo HELLO-APP; echo HELLO-ERR >&2"
rm -f "$T/stamped.log"
BOOT_LOG="$T/stamped.log" boot
st=0
bad=0
if [ -r "$T/stamped.log" ]; then
	while IFS= read -r _l; do
		case "$_l" in
		"[up "*"s] "*) st=$((st + 1)) ;;
		*) bad=$((bad + 1)) ;;
		esac
	done <"$T/stamped.log"
fi
if [ "$st" -ge 3 ] && [ "$bad" = 0 ]; then pass "log lines carry uptime stamps ($st)"; else fail "log stamps (stamped=$st plain=$bad)"; fi

# 9. the hook line: armed launches the stub, DISABLE and non-executable do not
mkapp
line=$(cat "$T/hookline.sh")
fire() { APP_DIR="$A" APP_BIN="$A/harmony_remote" APP_LOG="$T/app.log" START_TICK="$ST_TICK" sh -c "$line
wait" >/dev/null 2>&1; }
: >"$A/DISABLE"
fire
d=$(launches)
rm -f "$A/DISABLE"
fire
a=$(launches)
chmod 644 "$A/start.sh"
fire
x=$(launches)
if [ "$d" = 0 ] && [ "$a" = 1 ] && [ "$x" = 1 ]; then pass "hook line guards (DISABLE, -x)"; else fail "hook line (disable=$d armed=$a nonexec=$x)"; fi

# 10. syntax of start.sh and of a hooked copy of the device's real rcS.local (never written back)
if sh -n "$T/start.sh"; then pass "sh -n start.sh"; else fail "sh -n start.sh"; fi
if sh -n "$T/rcS.local.hooked"; then pass "sh -n hooked rcS.local copy"; else fail "sh -n hooked rcS.local copy"; fi

# 11. tripwire: the real counter did not move
if [ "$(real_count)" = "$REAL_BEFORE" ]; then pass "real $REAL_COUNT_FILE untouched ($REAL_BEFORE)"; else fail "REAL bootcount changed"; fi

echo "remote selftest done: $fails failure(s)"
[ "$fails" = 0 ]
