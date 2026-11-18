"""Phase 4 boot hook: guard state machine, blast-radius rules, applet floor.

`tools/boot/rcS.harmony` is sourced by stock `/etc/init.d/rcS.local` four lines before
dropbear starts, so a mistake here can leave the hub with no way in. Every test below
exists because of a specific way that can happen. Nothing here touches a device: the
script is parameterised by HAH_ROOT and driven against a temp tree.
"""

from __future__ import annotations

import os
import re
import shutil
import signal
import subprocess
import time
from pathlib import Path

import pytest

# The hub runs BusyBox ash; also exercise the hook under a host busybox when there is one.
_BB = shutil.which("busybox")
SHELLS = ["/bin/sh"] + ([f"{_BB}:sh"] if _BB else [])
_CUR = {"shell": ["/bin/sh"]}


@pytest.fixture(autouse=True, params=SHELLS, ids=lambda x: "busybox" if ":" in x else "sh")
def _shell(request):
    sh = request.param
    _CUR["shell"] = sh.split(":") if ":" in sh else [sh]
    yield


_PGIDS: list[int] = []


@pytest.fixture(autouse=True)
def _reap():
    """No stub daemon or watcher may outlive its test: each run is its own process group."""
    yield
    while _PGIDS:
        try:
            os.killpg(_PGIDS.pop(), signal.SIGKILL)
        except ProcessLookupError:
            pass


HOOK = Path(__file__).resolve().parents[2] / "tools" / "boot" / "rcS.harmony"

# BusyBox 1.13.4 on the hub. Anything not here either does not exist or is a builtin
# we have confirmed: see docs/plans/2026-11-17-phase4-persistence.md section 1.
HUB_APPLETS = {
    "cat", "echo", "rm", "mkdir", "ls", "grep", "sed", "sleep", "kill", "ps",
    "mount", "hostname", "insmod", "ifup", "mv", "cp", "ln", "chmod", "test",
    "reboot", "md5sum", "sh", "touch", "date", "cut", "tr", "sort", "expr",
}
# Confirmed absent on the hub; using one in a boot script fails at boot, silently.
HUB_MISSING = {
    "timeout", "nohup", "setsid", "wc", "head", "find", "df", "dd", "sync",
    "tar", "sha256sum", "command", "readlink", "uniq", "seq", "pgrep", "pkill",
    "flock", "start-stop-daemon", "systemctl", "service", "awk",
}


@pytest.fixture
def root(tmp_path: Path) -> Path:
    """A fake device root with the directories the hook expects."""
    for d in ("etc", "cache", "data/ha-harmony", "var/volatile"):
        (tmp_path / d).mkdir(parents=True)
    (tmp_path / "etc" / "nowatchdog").write_text("")
    (tmp_path / "etc" / "ha-harmony-enable").write_text("")
    return tmp_path


def install_daemon(root: Path, body: str) -> None:
    d = root / "data" / "ha-harmony" / "hub"
    d.write_text(f"#!/bin/sh\n{body}\n")
    d.chmod(0o755)


def install_conf(root: Path, text: str = "token=benchtok\nport=8099\nhub_id=ha-harmony-test\n") -> None:
    (root / "data" / "ha-harmony" / "hub.conf").write_text(text)


def source_hook(
    root: Path,
    *,
    healthy: int = 1,
    limit: int = 3,
    restrict_path: bool = False,
    delay: int = 0,
    extra_env: dict[str, str] | None = None,
    prelude: str = "",
) -> tuple[str, str]:
    """Source the hook the way rcS.local does and report DEMO_MODE afterwards.

    Returns (stdout, demo_mode) where demo_mode is "1" or "unset".
    """
    # Shell FUNCTIONS, not just PATH stubs: busybox ash runs its own applets in preference to
    # PATH, and a real `killall`/`reboot` must never reach the host.
    guards = "".join(f'{n}() {{ echo "$@" >>"{root}/{n}.log"; }}; ' for n in ("killall", "reboot"))
    script = f'{guards}{prelude}. "{HOOK}"; echo "__DEMO__=${{DEMO_MODE:-unset}}"'
    env = {
        "HAH_ROOT": str(root),
        "HAH_HEALTHY_SECS": str(healthy),
        "HAH_MAX_UNCLEAN": str(limit),
        "HAH_START_DELAY": str(delay),
        "HAH_POLL_SLEEP": "0.05",  # ms-scale: the hub uses whole seconds
        "HAH_HEALTHY_POLLS": str(healthy * 2),
        **(extra_env or {}),
    }
    # A logging killall stub always shadows the real one: never signal host processes.
    stub = root / "_stub"
    stub.mkdir(exist_ok=True)
    for name in ("killall", "reboot"):
        (stub / name).write_text(f'#!/bin/sh\necho "$@" >>"{root}/{name}.log"\n')
        (stub / name).chmod(0o755)
    if restrict_path:
        # Only applets the hub actually has, so any use of a missing one fails loudly.
        bindir = root / "_bin"
        bindir.mkdir(exist_ok=True)
        for applet in HUB_APPLETS:
            real = shutil.which(applet)
            if real:
                target = bindir / applet
                if not target.exists():
                    target.symlink_to(real)
        env["PATH"] = f"{stub}:{bindir}"
    else:
        env["PATH"] = f"{stub}:/usr/bin:/bin"

    proc = subprocess.Popen(
        [*_CUR["shell"], "-c", script], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        env=env, start_new_session=True,
    )  # fmt: skip
    _PGIDS.append(proc.pid)
    pout, perr = proc.communicate(timeout=60)
    p = subprocess.CompletedProcess(proc.args, proc.returncode, pout, perr)
    assert p.returncode == 0, f"hook returned {p.returncode}: {p.stderr}"
    m = re.search(r"__DEMO__=(\S+)", p.stdout)
    assert m, f"no marker in output: {p.stdout!r}"
    return p.stdout, m.group(1)


# --- blast radius: the rules that keep the hub reachable -------------------------


def test_hook_parses_as_posix_sh() -> None:
    """A syntax error here aborts rcS itself, before dropbear starts."""
    p = subprocess.run([*_CUR["shell"], "-n", str(HOOK)], capture_output=True, text=True, check=False)
    assert p.returncode == 0, p.stderr


def test_hook_never_calls_exit() -> None:
    """`exit` in a sourced file terminates rcS: no dropbear, no hal, no luaworks."""
    code = re.sub(r"#.*", "", HOOK.read_text())
    assert not re.search(r"\bexit\b", code), "sourced hook must use `return`, never `exit`"


def test_hook_uses_no_applet_the_hub_lacks() -> None:
    code = re.sub(r"#.*", "", HOOK.read_text())
    words = set(re.findall(r"[A-Za-z][A-Za-z0-9_.-]*", code))
    offenders = words & HUB_MISSING
    assert not offenders, f"hook uses applets absent on the hub: {sorted(offenders)}"


def _sync_code() -> str:
    """The hook source before its backgrounded subshell opens, comments stripped."""
    code = re.sub(r"#.*", "", HOOK.read_text())
    m = re.search(r"^\t+\($", code, re.MULTILINE)
    assert m, "background subshell opener not found"
    return code[: m.start()]


def test_synchronous_path_does_no_blocking_work() -> None:
    sync = _sync_code()
    for word in ("sleep", "wait", "reboot", "killall", "ifconfig", "udhcpc", "ping", "/dev/i2s",
                 "nc ", "wget", "curl"):  # fmt: skip
        assert not re.search(rf"(?<![\w-]){re.escape(word)}(?![\w-])", sync), (
            f"synchronous path must not use {word!r}"
        )
    assert not re.search(r'"\$hah_daemon" --', sync), "daemon exec must be inside the background"
    code = re.sub(r"#.*", "", HOOK.read_text())
    assert re.search(r'"\$hah_daemon".*?&\s*$', code, re.DOTALL | re.MULTILINE)


def test_sync_path_has_no_redirect_on_a_special_builtin_or_arithmetic_on_file_data() -> None:
    """A failed redirect on `:` `.` `export` `set` `eval` `exec` `unset`... exits a sourced shell."""
    sync = _sync_code()
    special = r"(?:^|[;&|(]\s*|\n\s*)(:|\.|export|set|eval|exec|readonly|unset|shift|break|continue|return|trap)\b[^\n]*[<>]"
    hits = [m.group(0) for m in re.finditer(special, sync) if "2>/dev/null" not in m.group(0).split("<")[0]]
    # `unset -f ... 2>/dev/null` is the only allowed shape, and it is at the very end (after the subshell).
    assert not [h for h in hits if "/dev/null" not in h], hits
    assert "$((" not in sync, "no arithmetic on the synchronous path"
    for line in sync.splitlines():
        stripped = line.strip()
        assert not re.match(r":\s*[<>]", stripped), f"bare `: >file` on sync path: {line!r}"


def test_config_is_parsed_not_sourced() -> None:
    """Sourcing hub.conf would execute arbitrary commands in rcS's own shell."""
    code = re.sub(r"#.*", "", HOOK.read_text())
    assert not re.search(r"^\s*\.\s+\"?\$hah_conf", code, re.MULTILINE)
    assert not re.search(r"source\s+\"?\$hah_conf", code)


def test_a_command_in_the_config_file_is_not_executed(root: Path) -> None:
    install_daemon(root, "sleep 30")
    install_conf(root, f"token=t\nport=8099\nevil=$(touch {root}/pwned)\n")
    source_hook(root)
    assert not (root / "pwned").exists(), "config file content was executed"


# --- the guard state machine ----------------------------------------------------


def test_clean_boot_starts_daemon_and_suppresses_hal(root: Path) -> None:
    install_daemon(root, "sleep 30")
    install_conf(root)
    _, demo = source_hook(root)
    assert demo == "1", "DEMO_MODE must be set so rcS:84 skips stock hal"
    assert (root / "cache" / "ha-harmony-boot.n").read_text().strip() == "1"
    _wait_for(root / "cache" / "ha-harmony.pid")


def test_healthy_run_clears_the_strike_counter(root: Path) -> None:
    install_daemon(root, "sleep 30")
    install_conf(root)
    source_hook(root, healthy=1)
    counter = root / "cache" / "ha-harmony-boot.n"
    assert counter.read_text().strip() == "1"
    _wait_gone(counter)
    assert not counter.exists(), "watcher must clear strikes once the daemon survives"


def test_a_daemon_that_dies_leaves_the_strike_standing(root: Path) -> None:
    install_daemon(root, "exit 1")
    install_conf(root)
    source_hook(root, healthy=1)
    counter = root / "cache" / "ha-harmony-boot.n"
    _wait_text(root / "cache" / "ha-harmony.log", "watcher done")
    assert counter.read_text().strip() == "1", "a dead daemon must not clear its strike"


def test_three_unclean_boots_disable_the_hook(root: Path) -> None:
    """The headline guarantee: a crash-loop converges on stock, not on a brick."""
    install_daemon(root, "exit 1")
    install_conf(root)
    marker = root / "etc" / "ha-harmony-enable"

    # boots 1 and 2: we take the boot, daemon dies, strike stands
    for expected in ("1", "2"):
        _, demo = source_hook(root, healthy=1, limit=3)
        assert demo == "1"
        assert marker.exists()
        assert (root / "cache" / "ha-harmony-boot.n").read_text().strip() == expected

    # boot 3: hand the boot back to stock and switch ourselves off
    out, demo = source_hook(root, healthy=1, limit=3)
    assert demo == "unset", "on the disabling boot stock hal must be allowed to start"
    assert not marker.exists(), "hook must remove its own marker"
    assert "DISABLED" in out
    assert not (root / "cache" / "ha-harmony-boot.n").exists()


def test_a_corrupt_counter_is_treated_as_clean(root: Path) -> None:
    install_daemon(root, "sleep 30")
    install_conf(root)
    (root / "cache" / "ha-harmony-boot.n").write_text("garbage\x00\n")
    _, demo = source_hook(root)
    assert demo == "1"
    assert (root / "cache" / "ha-harmony-boot.n").read_text().strip() == "1"


# --- refusing to go inert -------------------------------------------------------


@pytest.mark.parametrize(
    "setup",
    ["no_daemon", "no_conf", "no_token", "daemon_not_executable"],
    ids=["missing daemon", "missing config", "config without token", "daemon not executable"],
)
def test_incomplete_install_leaves_stock_running(root: Path, setup: str) -> None:
    """Never suppress hal unless we can actually replace it."""
    if setup != "no_daemon":
        install_daemon(root, "sleep 30")
    if setup == "daemon_not_executable":
        (root / "data" / "ha-harmony" / "hub").chmod(0o644)
    if setup == "no_token":
        install_conf(root, "port=8099\n")
    elif setup != "no_conf":
        install_conf(root)

    _, demo = source_hook(root)
    assert demo == "unset", "an incomplete install must leave the hub fully stock"
    assert (root / "etc" / "ha-harmony-enable").exists(), "and must not disable itself"


def test_runs_inside_the_hub_applet_floor(root: Path) -> None:
    """Same clean boot, but with PATH holding only applets the hub really has."""
    install_daemon(root, "sleep 30")
    install_conf(root)
    _, demo = source_hook(root, restrict_path=True)
    assert demo == "1"
    assert (root / "cache" / "ha-harmony-boot.n").read_text().strip() == "1"
    _wait_for(root / "cache" / "ha-harmony.pid")


def _wait_gone(p: Path, tries: int = 250) -> None:
    for _ in range(tries):
        if not p.exists():
            return
        time.sleep(0.02)


def _wait_for(p: Path, tries: int = 250) -> None:
    for _ in range(tries):
        if p.exists() and p.stat().st_size:
            return
        time.sleep(0.02)
    raise AssertionError(f"{p} never appeared")


def _wait_text(p: Path, text: str, tries: int = 250) -> str:
    for _ in range(tries):
        if p.exists() and text in p.read_text():
            return p.read_text()
        time.sleep(0.02)
    raise AssertionError(f"{text!r} never appeared in {p}: {p.read_text() if p.exists() else ''}")


# --- conf keys, argv, deferred launcher, log placement ---------------------------

ARGV_DAEMON = 'echo "$@" > "$(dirname "$0")/argv.txt"; sleep 30'


def _argv(root: Path) -> str:
    f = root / "data" / "ha-harmony" / "argv.txt"
    _wait_for(f)
    return f.read_text()


def test_rf_conf_keys_become_daemon_argv(root: Path) -> None:
    install_daemon(root, ARGV_DAEMON)
    install_conf(root, "token=t\nport=8099\nrf=spi\nrf_slot=2\n")
    _, demo = source_hook(root)
    assert demo == "1"
    argv = _argv(root)
    assert "--rf-spi --rf-slot 2" in argv
    assert "--token t" in argv


def test_no_rf_key_means_no_rf_flags(root: Path) -> None:
    install_daemon(root, ARGV_DAEMON)
    install_conf(root)
    source_hook(root)
    assert "--rf" not in _argv(root)


@pytest.mark.parametrize(
    "junk",
    ["rf=bogus", "rf=", "rf_slot=", "rf_slot=2x", "rf_slot=-1", "rf_slot=$(id)", "stop_lua=maybe"],
)
def test_bad_conf_value_leaves_stock_alone_with_no_strike(root: Path, junk: str) -> None:
    install_daemon(root, "sleep 30")
    install_conf(root, f"token=t\nrf=spi\n{junk}\n")
    _, demo = source_hook(root)
    assert demo == "unset"
    assert not (root / "cache" / "ha-harmony-boot.n").exists(), "junk config must not spend a strike"
    assert (root / "etc" / "ha-harmony-enable").exists()
    assert not (root / "data" / "ha-harmony" / "argv.txt").exists()


def test_daemon_output_never_goes_to_cache(root: Path) -> None:
    install_daemon(root, "echo DAEMON-CHATTER; echo DAEMON-ERR >&2; sleep 30")
    install_conf(root)
    source_hook(root)
    vlog = root / "var" / "volatile" / "ha-harmony.log"
    _wait_text(vlog, "DAEMON-ERR")
    assert "DAEMON-CHATTER" in vlog.read_text()
    for f in (root / "cache").iterdir():
        assert "DAEMON" not in f.read_text(), f"daemon output leaked into {f}"
    assert "starting" in (root / "cache" / "ha-harmony.log").read_text()


def test_hook_log_is_truncated_per_boot(root: Path) -> None:
    install_daemon(root, "sleep 30")
    install_conf(root)
    (root / "cache" / "ha-harmony.log").write_text("OLD-BOOT-LINE\n")
    source_hook(root)
    assert "OLD-BOOT-LINE" not in (root / "cache" / "ha-harmony.log").read_text()


def test_launch_is_deferred_not_synchronous(root: Path) -> None:
    """With a long delay the hook still returns at once and has started nothing."""
    install_daemon(root, ARGV_DAEMON)
    install_conf(root)
    t0 = time.monotonic()
    _, demo = source_hook(root, delay=30)
    assert time.monotonic() - t0 < 10, "the synchronous path waited"
    assert demo == "1"
    assert not (root / "data" / "ha-harmony" / "argv.txt").exists()
    assert not (root / "cache" / "ha-harmony.pid").exists()
    # and statically: no sleep before the backgrounded subshell opens
    assert "sleep" not in _sync_code()


def test_stop_lua_is_one_switch_default_on(root: Path) -> None:
    install_daemon(root, "sleep 30")
    install_conf(root)
    source_hook(root)
    _wait_for(root / "cache" / "ha-harmony.pid")
    assert (root / "killall.log").read_text().split() == ["lua", "luaworks", "luadraws"]


def test_stop_lua_off_leaves_lua_alone(root: Path) -> None:
    install_daemon(root, "sleep 30")
    install_conf(root, "token=t\nstop_lua=0\n")
    source_hook(root)
    _wait_for(root / "cache" / "ha-harmony.pid")
    assert not (root / "killall.log").exists()


def test_launcher_uses_only_hub_tools_for_lua_stop() -> None:
    code = re.sub(r"#.*", "", HOOK.read_text())
    assert "killall" in code
    assert not re.search(r"\b(pidof|pkill|killall -9|kill -9|kill -KILL)\b", code)


# --- brick-safety audit: nothing on the sync path may kill or hang rcS ------------

SENTINEL = "RCS_CONTINUES"


def run_sourced(root: Path, **kw) -> tuple[str, str]:
    """Source the hook; the shell must reach the sentinel afterwards (rcS would continue)."""
    kw["prelude"] = kw.get("prelude", "")
    out, demo = source_hook(root, **kw)
    return out, demo


@pytest.mark.skipif(os.geteuid() == 0, reason="root ignores directory permissions")
def test_unwritable_cache_boots_stock(root: Path) -> None:
    install_daemon(root, "sleep 30")
    install_conf(root)
    (root / "cache").chmod(0o555)
    try:
        _, demo = source_hook(root)
    finally:
        (root / "cache").chmod(0o755)
    assert demo == "unset", "strikes cannot be stored: stock must boot"
    assert (root / "etc" / "ha-harmony-enable").exists()


def test_missing_cache_dir_boots_stock_and_sourcing_survives(root: Path) -> None:
    install_daemon(root, "sleep 30")
    install_conf(root)
    shutil.rmtree(root / "cache")
    _, demo = source_hook(root)  # source_hook asserts the shell finished and printed
    assert demo == "unset"


def test_failed_counter_write_never_sets_demo_mode(root: Path) -> None:
    install_daemon(root, ARGV_DAEMON)
    install_conf(root)
    (root / "cache" / "ha-harmony-boot.n").mkdir()  # cannot be written or read as a file
    _, demo = source_hook(root)
    assert demo == "unset"
    assert not (root / "data" / "ha-harmony" / "argv.txt").exists()


@pytest.mark.parametrize("junk", ["08", "09", "007", "garbage", "-1", "1 2", "99999999999999999999", ""])
def test_odd_counter_values_are_not_arithmetic_errors(root: Path, junk: str) -> None:
    install_daemon(root, "sleep 30")
    install_conf(root)
    (root / "cache" / "ha-harmony-boot.n").write_text(junk + "\n")
    _, demo = source_hook(root)
    assert demo == "1", f"counter {junk!r} must count as clean, not kill the shell"
    assert (root / "cache" / "ha-harmony-boot.n").read_text().strip() == "1"


def test_marker_removal_failure_boots_stock_and_keeps_counter(root: Path) -> None:
    install_daemon(root, "sleep 30")
    install_conf(root)
    marker = root / "etc" / "ha-harmony-enable"
    marker.unlink()
    marker.mkdir()  # `rm -f` cannot remove a non-empty directory
    (marker / "x").write_text("")
    (root / "cache" / "ha-harmony-boot.n").write_text("2\n")
    out, demo = source_hook(root)
    assert demo == "unset", "never DEMO_MODE when we could not disarm"
    assert "cannot remove" in out
    assert (root / "cache" / "ha-harmony-boot.n").read_text().strip() == "2"
    assert not (root / "killall.log").exists()


def test_sourcing_leaks_no_state_into_the_caller(root: Path) -> None:
    install_daemon(root, "exit 0")
    install_conf(root)
    probe = 'echo "S:$PWD|$#|$1|$(umask)|$-|$IFS" | od -c | cat; trap'
    pre = "cd /; umask 027; set -- keep me; IFS=:; trap 'echo T' USR1; "
    guards = "".join(f'{n}() {{ :; }}; ' for n in ("killall", "reboot"))
    script_after = f"{guards}{pre}. \"{HOOK}\" >/dev/null 2>&1; {probe}"
    env = {"HAH_ROOT": str(root), "HAH_START_DELAY": "0", "HAH_POLL_SLEEP": "0.05", "HAH_HEALTHY_POLLS": "1", "PATH": f"{root}/_stub:/usr/bin:/bin"}
    runs = []
    for script in (script_after, f"{pre}{probe}"):
        pr = subprocess.Popen([*_CUR["shell"], "-c", script], stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, text=True, env=env, start_new_session=True)  # fmt: skip
        _PGIDS.append(pr.pid)
        runs.append(pr.communicate(timeout=60)[0])
    after, before = runs
    assert after == before, f"hook leaked caller state:\n{before}\n---\n{after}"


# --- config hardening -----------------------------------------------------------


def test_token_containing_equals_survives_intact(root: Path) -> None:
    install_daemon(root, ARGV_DAEMON)
    install_conf(root, "token=abc123==\nport=8099\n")
    _, demo = source_hook(root)
    assert demo == "1"
    assert "--token abc123==" in _argv(root)


@pytest.mark.parametrize(
    "line",
    [
        "token=ok\nhub_id=x;touch PWN\n",
        "token=ok\nhub_id=a b\n",
        "token=ok\nhub_id=$(touch PWN)\n",
        "token=ok\nhub_id=\n",
        "token=a b\n",
        "token=ok;touch PWN\n",
        "token=ok\r\n",
        "token=ok\nport=80a\n",
        "token=ok\nport=123456\n",
        "token=ok\nrf=spi\nrf_slot=123\n",
        "token=\n",
    ],
)
def test_injection_and_crlf_in_conf_leave_stock_alone(root: Path, line: str) -> None:
    install_daemon(root, ARGV_DAEMON)
    install_conf(root, line)
    _, demo = source_hook(root)
    assert demo == "unset"
    assert not (root / "PWN").exists() and not (Path.cwd() / "PWN").exists()
    assert not (root / "cache" / "ha-harmony-boot.n").exists()


def test_conf_without_trailing_newline_is_read(root: Path) -> None:
    install_daemon(root, ARGV_DAEMON)
    install_conf(root, "port=8099\ntoken=lastline")
    _, demo = source_hook(root)
    assert demo == "1"
    assert "--token lastline" in _argv(root)


# --- early death: reboot switch ---------------------------------------------------


def test_early_death_reboots_by_default_only_with_a_persisted_strike(root: Path) -> None:
    install_daemon(root, "exit 1")
    install_conf(root)
    source_hook(root, healthy=2)
    log = _wait_text(root / "cache" / "ha-harmony.log", "watcher done")
    assert "rebooting (reboot_on_early_death)" in log
    _wait_for(root / "reboot.log")
    assert (root / "cache" / "ha-harmony-boot.n").read_text().strip() == "1"


def test_reboot_switch_off(root: Path) -> None:
    install_daemon(root, "exit 1")
    install_conf(root, "token=t\nreboot_on_early_death=0\n")
    source_hook(root, healthy=2)
    # The watcher's own final lines say whether it decided against rebooting: no race.
    log = _wait_text(root / "cache" / "ha-harmony.log", "watcher done")
    assert "died within" in log and "not rebooting" in log
    assert not (root / "reboot.log").exists()


def test_no_reboot_after_the_health_mark(root: Path) -> None:
    install_daemon(root, "sleep 0.25")  # health window is 0.1 s, dies later
    install_conf(root)
    source_hook(root, healthy=1)
    log = _wait_text(root / "cache" / "ha-harmony.log", "watcher done")
    assert "daemon died after health mark" in log and "rebooting" not in log.replace("no reboot", "")
    assert not (root / "reboot.log").exists()


def test_bad_reboot_switch_value_is_stock(root: Path) -> None:
    install_daemon(root, "sleep 30")
    install_conf(root, "token=t\nreboot_on_early_death=2\n")
    _, demo = source_hook(root)
    assert demo == "unset"


# --- daemon log cap ----------------------------------------------------------------


def test_daemon_log_is_capped_without_corrupting_the_append_stream(root: Path) -> None:
    install_daemon(
        root,
        'i=0; while [ $i -lt 60 ]; do echo "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"; i=$((i+1)); done;'
        " sleep 0.3; echo AFTER-TRUNCATE; sleep 30",
    )
    install_conf(root)
    source_hook(root, healthy=1, extra_env={"HAH_LOG_MAX_LINES": "10"})
    vlog = root / "var" / "volatile" / "ha-harmony.log"
    _wait_text(vlog, "AFTER-TRUNCATE")
    data = vlog.read_bytes()
    assert b"AFTER-TRUNCATE" in data
    assert b"\0" not in data, "truncation must not leave a hole under the daemon's O_APPEND fd"
    assert len(data) < 500, "log was not capped"


# --- review round 2: conf-loop hang guards, watchdog guard ---------------------------


def test_conf_loop_terminates_when_read_never_clears_its_variable(root: Path) -> None:
    """A shell whose `read` leaves the old value at EOF must not spin the loop forever."""
    install_daemon(root, "sleep 30")
    install_conf(root, "token=abc\n")
    stub = (
        'read() { for a; do v=$a; done; n=$((${n:-0}+1)); '
        '[ "$n" = 1 ] && { eval "$v=\\"token=abc\\""; return 0; }; return 1; }; '
    )
    _, demo = source_hook(root, prelude=stub)
    assert demo == "1"


def test_conf_loop_is_hard_capped(root: Path) -> None:
    """Second guard: a `read` that never ends is cut off after a bounded number of lines."""
    install_daemon(root, "sleep 30")
    install_conf(root, "token=abc\n")
    stub = 'read() { for a; do v=$a; done; eval "$v=\\"x=y\\""; return 0; }; '
    out, demo = source_hook(root, prelude=stub)  # must return, not spin
    assert demo == "unset" and "too many lines" in out


def test_conf_loop_resets_the_line_variable_each_iteration() -> None:
    code = re.sub(r"#.*", "", HOOK.read_text())
    assert "while hah_line=''; IFS= read -r hah_line" in code


def test_stock_watchdog_enabled_leaves_stock_alone_with_no_strike(root: Path) -> None:
    install_daemon(root, ARGV_DAEMON)
    install_conf(root)
    (root / "etc" / "nowatchdog").unlink()
    out, demo = source_hook(root)
    assert demo == "unset" and "watchdog" in out
    assert not (root / "cache" / "ha-harmony-boot.n").exists()
    assert (root / "etc" / "ha-harmony-enable").exists()


def test_onhub_selftest_script_passes_under_host_shells(tmp_path: Path) -> None:
    """The on-hub dry run is itself tested here (fast knobs); it writes only under its own dir."""
    script = HOOK.parent / "hook_selftest.sh"
    work = tmp_path / "st"
    env = {
        **os.environ,
        "HAH_SELFTEST_DIR": str(work),
        "HAH_SELFTEST_NAP": "0.05",
        "HAH_SELFTEST_POLLS": "2",
        "HAH_SELFTEST_SETTLE": "0.15",
        "HAH_SELFTEST_TICK": "0.02",
        "HAH_SELFTEST_GAP": "0.4",
        "HAH_SELFTEST_FINAL": "0.2",
    }
    pr = subprocess.Popen([*_CUR["shell"], str(script), str(HOOK)], stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True, env=env, start_new_session=True)  # fmt: skip
    _PGIDS.append(pr.pid)
    out = pr.communicate(timeout=60)[0]
    assert pr.returncode == 0 and "0 failure(s)" in out, out
    assert out.count("PASS") >= 12 and "FAIL" not in out
    assert not (Path("/etc/ha-harmony-enable")).exists()
