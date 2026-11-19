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
from test_hub_token_file import CONF_FIXTURES

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
    "cat",
    "echo",
    "rm",
    "mkdir",
    "ls",
    "grep",
    "sed",
    "sleep",
    "kill",
    "ps",
    "mount",
    "hostname",
    "insmod",
    "ifup",
    "mv",
    "cp",
    "ln",
    "chmod",
    "test",
    "reboot",
    "md5sum",
    "sh",
    "touch",
    "date",
    "cut",
    "tr",
    "sort",
    "expr",
}
# Confirmed absent on the hub; using one in a boot script fails at boot, silently.
HUB_MISSING = {
    "timeout",
    "nohup",
    "setsid",
    "wc",
    "head",
    "find",
    "df",
    "dd",
    "sync",
    "tar",
    "sha256sum",
    "command",
    "readlink",
    "uniq",
    "seq",
    "pgrep",
    "pkill",
    "flock",
    "start-stop-daemon",
    "systemctl",
    "service",
    "awk",
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


def install_conf(
    root: Path, text: str = "token=benchtok\nport=8099\nhub_id=ha-harmony-test\n"
) -> None:
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
    p = subprocess.run(
        [*_CUR["shell"], "-n", str(HOOK)], capture_output=True, text=True, check=False
    )
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
    hits = [
        m.group(0)
        for m in re.finditer(special, sync)
        if "2>/dev/null" not in m.group(0).split("<")[0]
    ]
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
    assert f"--token-file {root}/data/ha-harmony/hub.conf" in argv
    assert "--token " not in argv


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
    assert not (root / "cache" / "ha-harmony-boot.n").exists(), (
        "junk config must not spend a strike"
    )
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
    assert not re.search(r"\b(pidof|pkill|killall -9|kill -KILL)\b", code)


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


@pytest.mark.parametrize(
    "junk", ["08", "09", "007", "garbage", "-1", "1 2", "99999999999999999999", ""]
)
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
    guards = "".join(f"{n}() {{ :; }}; " for n in ("killall", "reboot"))
    script_after = f'{guards}{pre}. "{HOOK}" >/dev/null 2>&1; {probe}'
    env = {
        "HAH_ROOT": str(root),
        "HAH_START_DELAY": "0",
        "HAH_POLL_SLEEP": "0.05",
        "HAH_HEALTHY_POLLS": "1",
        "PATH": f"{root}/_stub:/usr/bin:/bin",
    }
    runs = []
    for script in (script_after, f"{pre}{probe}"):
        pr = subprocess.Popen([*_CUR["shell"], "-c", script], stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, text=True, env=env, start_new_session=True)  # fmt: skip
        _PGIDS.append(pr.pid)
        runs.append(pr.communicate(timeout=60)[0])
    after, before = runs
    assert after == before, f"hook leaked caller state:\n{before}\n---\n{after}"


# --- config hardening -----------------------------------------------------------


@pytest.mark.parametrize(
    ("content", "want"),
    [(c, w) for _, c, w in CONF_FIXTURES],
    ids=[i for i, _, _ in CONF_FIXTURES],
)
def test_hook_verdict_matches_daemon_conf_fixtures(
    root: Path, content: str, want: str | None
) -> None:
    """Same fixtures as test_hub_token_file.py: the hook arms iff the daemon would accept."""
    install_daemon(root, ARGV_DAEMON)
    install_conf(root, content)
    _, demo = source_hook(root)
    if want is None:
        assert demo == "unset"
        assert not (root / "cache" / "ha-harmony-boot.n").exists()
    else:
        assert demo == "1"
        assert want not in _argv(root)


def test_token_containing_equals_survives_intact(root: Path) -> None:
    install_daemon(root, ARGV_DAEMON)
    install_conf(root, "token=abc123==\nport=8099\n")
    _, demo = source_hook(root)
    assert demo == "1"
    argv = _argv(root)
    assert "--token-file" in argv
    assert "abc123==" not in argv
    for log in ("cache/ha-harmony.log", "var/volatile/ha-harmony.log"):
        assert "abc123==" not in (root / log).read_text()


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
    assert "--token-file" in _argv(root)
    assert "lastline" not in _argv(root)


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
    install_conf(root, "token=benchtok\nrespawn_max=0\n")
    source_hook(root, healthy=1)
    log = _wait_text(root / "cache" / "ha-harmony.log", "watcher done")
    assert "daemon died after health mark" in log and "rebooting" not in log.replace(
        "no reboot", ""
    )
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
        "read() { for a; do v=$a; done; n=$((${n:-0}+1)); "
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
    failed = [ln for ln in out.splitlines() if ln.startswith("FAIL")]
    assert not failed, f"failing selftest cases: {failed}\n{out}"
    assert pr.returncode == 0 and "0 failure(s)" in out, out
    assert out.count("PASS") >= 19
    assert not (Path("/etc/ha-harmony-enable")).exists()


# --- bounded respawn after the health mark, wedge cover, log stamps ------------------

FAST = {"HAH_BACKOFF1": "0.05", "HAH_BACKOFF2": "0.05", "HAH_BACKOFF3": "0.05"}
# Counts launches; dies 0.6 s after start (well past the 0.1 s health window in these tests).
MORTAL = 'echo x >>"$(dirname "$0")/runs"; sleep 0.6; exit 1'
IMMORTAL = 'echo "$@" >"$(dirname "$0")/argv.txt"; echo x >>"$(dirname "$0")/runs"; sleep 30'


def _runs(root: Path) -> int:
    f = root / "data" / "ha-harmony" / "runs"
    return len(f.read_text().split()) if f.exists() else 0


def _wait_runs(root: Path, n: int, tries: int = 400) -> None:
    for _ in range(tries):
        if _runs(root) >= n:
            return
        time.sleep(0.02)
    raise AssertionError(f"wanted {n} launches, saw {_runs(root)}")


def _clog(root: Path) -> str:
    f = root / "cache" / "ha-harmony.log"
    return f.read_text() if f.exists() else ""


def test_death_after_health_respawns_then_budget_exhausts_without_reboot(root: Path) -> None:
    install_daemon(root, MORTAL)
    install_conf(root, "token=t\nrespawn_max=2\n")
    source_hook(root, extra_env=FAST)
    _wait_text(root / "cache" / "ha-harmony.log", "budget exhausted")
    time.sleep(0.5)  # stays down: no third respawn
    assert _runs(root) == 3, "initial launch + exactly respawn_max respawns"
    assert not (root / "reboot.log").exists(), "respawns never reboot"
    assert not (root / "cache" / "ha-harmony-boot.n").exists(), "strike counter untouched"
    assert (root / "etc" / "ha-harmony-enable").exists()
    assert "respawn 2 of 2" in _clog(root)


def test_default_budget_is_three(root: Path) -> None:
    install_daemon(root, MORTAL)
    install_conf(root)
    source_hook(root, extra_env=FAST)
    _wait_text(root / "cache" / "ha-harmony.log", "budget exhausted (3 of 3)")
    assert _runs(root) == 4


def test_respawn_max_zero_is_the_old_single_shot_behaviour(root: Path) -> None:
    install_daemon(root, MORTAL)
    install_conf(root, "token=t\nrespawn_max=0\n")
    source_hook(root, extra_env=FAST)
    _wait_text(root / "cache" / "ha-harmony.log", "no respawn")
    time.sleep(0.3)
    assert _runs(root) == 1
    assert not (root / "reboot.log").exists()


def test_respawn_max_zero_passes_no_heartbeat_flag(root: Path) -> None:
    install_daemon(root, IMMORTAL)
    install_conf(root, "token=t\nrespawn_max=0\n")
    source_hook(root)
    assert "--heartbeat" not in _argv(root)


def test_heartbeat_and_rf_debug_flags_in_argv(root: Path) -> None:
    install_daemon(root, IMMORTAL)
    install_conf(root, "token=t\nrf=spi\nrf_debug=1\n")
    source_hook(root)
    argv = _argv(root)
    assert f"--heartbeat {root}/var/volatile/ha-harmony.hb" in argv
    assert f"--health-file {root}/var/volatile/ha-harmony.healthy" in argv
    assert "--ota-allow-dev" not in argv  # off unless the conf says so
    assert "--rf-debug" in argv.split()


def test_ota_allow_dev_is_a_conf_key_and_validated(root: Path) -> None:
    install_daemon(root, IMMORTAL)
    install_conf(root, "token=t\nota_allow_dev=1\n")
    source_hook(root)
    assert "--ota-allow-dev" in _argv(root).split()


def test_bad_ota_allow_dev_value_is_stock(root: Path) -> None:
    install_daemon(root, IMMORTAL)
    install_conf(root, "token=t\nota_allow_dev=yes\n")
    _, demo = source_hook(root)
    assert demo == "unset"


@pytest.mark.parametrize("junk", ["respawn_max=x", "respawn_max=100", "respawn_max=-1", "rf_debug=2"])
def test_bad_respawn_or_debug_value_is_stock(root: Path, junk: str) -> None:
    install_daemon(root, IMMORTAL)
    install_conf(root, f"token=t\n{junk}\n")
    _, demo = source_hook(root)
    assert demo == "unset" and not (root / "cache" / "ha-harmony-boot.n").exists()


def test_early_death_with_respawn_enabled_keeps_strike_and_reboots_no_respawn(root: Path) -> None:
    install_daemon(root, 'echo x >>"$(dirname "$0")/runs"; exit 1')
    install_conf(root, "token=t\nrespawn_max=3\n")
    source_hook(root, extra_env=FAST)
    _wait_text(root / "cache" / "ha-harmony.log", "watcher done")
    time.sleep(0.3)
    assert _runs(root) == 1
    assert (root / "cache" / "ha-harmony-boot.n").read_text().strip() == "1"
    assert (root / "reboot.log").exists()


def _fake_clock(root: Path, secs: int) -> Path:
    f = root / "uptime"
    f.write_text(f"{secs}.00 1.00\n")
    return f


def test_stale_heartbeat_kills_and_respawns_the_wedged_daemon(root: Path) -> None:
    clock = _fake_clock(root, 1000)
    install_daemon(root, IMMORTAL)
    install_conf(root, "token=t\nrespawn_max=1\n")
    source_hook(root, extra_env={**FAST, "HAH_UPTIME_FILE": str(clock)})
    _wait_runs(root, 1)
    time.sleep(0.4)  # healthy, polling, heartbeat never written but the clock has not moved
    assert _runs(root) == 1
    _fake_clock(root, 1100)  # 100 s with no heartbeat
    _wait_runs(root, 2)
    _wait_text(root / "cache" / "ha-harmony.log", "heartbeat stale")
    time.sleep(0.3)
    assert _runs(root) == 2, "second launch is fresh; budget of 1 is spent"
    assert not (root / "reboot.log").exists()
    assert not (root / "cache" / "ha-harmony-boot.n").exists()


def test_fresh_heartbeat_is_left_alone(root: Path) -> None:
    clock = _fake_clock(root, 1000)
    install_daemon(root, IMMORTAL)
    install_conf(root, "token=t\n")
    source_hook(root, extra_env={**FAST, "HAH_UPTIME_FILE": str(clock)})
    _wait_runs(root, 1)
    (root / "var" / "volatile" / "ha-harmony.hb").write_text("1100\n")
    _fake_clock(root, 1100)
    time.sleep(0.5)
    assert _runs(root) == 1


def test_budget_refills_after_an_hour_of_uptime(root: Path) -> None:
    clock = _fake_clock(root, 1000)
    install_daemon(root, MORTAL)
    install_conf(root, "token=t\nrespawn_max=1\n")
    source_hook(root, extra_env={**FAST, "HAH_UPTIME_FILE": str(clock), "HAH_HB_STALE": "99999"})
    _wait_text(root / "cache" / "ha-harmony.log", "respawn 1 of 1")
    _wait_runs(root, 2)  # run 2 is the last respawn of the budget
    time.sleep(0.25)  # the watcher has taken this run's start stamp
    _fake_clock(root, 1000 + 3600)  # ... which then runs "an hour"
    _wait_runs(root, 3)  # refilled, so it is respawned once more
    _wait_text(root / "cache" / "ha-harmony.log", "budget refilled")
    assert not (root / "reboot.log").exists()


def test_log_lines_carry_uptime_stamps(root: Path) -> None:
    install_daemon(root, "echo DAEMON-LINE; sleep 30")
    install_conf(root)
    source_hook(root)
    vlog = root / "var" / "volatile" / "ha-harmony.log"
    _wait_text(vlog, "DAEMON-LINE")
    assert re.search(r"^\[up \d+s\] DAEMON-LINE$", vlog.read_text(), re.MULTILINE)
    _wait_text(root / "cache" / "ha-harmony.log", "healthy")
    lines = _clog(root).splitlines()
    assert lines and all(re.match(r"\[up \d+s( \d{4}-\d\d-\d\d [\d:]+)?\] ha-harmony: ", ln) for ln in lines)
    assert any(re.match(r"\[up \d+s \d{4}-", ln) for ln in lines), "background lines carry the date"


# --- signed update: relaunch into the new build, probation, rollback once ------------------------

NEW_OK = 'D=$(dirname "$0"); echo x >>"$D/runs_new"; sleep 30'
NEW_BAD = 'D=$(dirname "$0"); echo x >>"$D/runs_new"; exit 1'
# The build that applies an update: lives past the health mark (waits for `go`), then does what
# hub/ota/hub_ota.c does (pending marker, hub.prev hard link, atomic swap, restart marker) and exits 42.
# A second run of the same file (after a rollback) just serves.
OLD_APPLIER = (
    'D=$(dirname "$0"); echo x >>"$D/runs_old"\n'
    'if [ -e "$D/applied" ]; then sleep 30; exit 0; fi\n'
    'while [ ! -e "$D/go" ]; do sleep 0.02; done\n'
    ': >"$D/applied"; echo build=2 >"$D/ota-pending"\n'
    'ln "$D/hub" "$D/hub.tmp" && mv "$D/hub.tmp" "$D/hub.prev"\n'
    'mv "$D/hub.new" "$D/hub"; echo 2 >"$D/ota-restart"; exit 42'
)


def _hub_dir(root: Path) -> Path:
    return root / "data" / "ha-harmony"


def _install_new(root: Path, body: str) -> None:
    f = _hub_dir(root) / "hub.new"
    f.write_text(f"#!/bin/sh\n{body}\n")
    f.chmod(0o755)


def _count(root: Path, name: str) -> int:
    f = _hub_dir(root) / name
    return len(f.read_text().split()) if f.exists() else 0


def _wait_count(root: Path, name: str, n: int) -> None:
    for _ in range(500):
        if _count(root, name) >= n:
            return
        time.sleep(0.02)
    raise AssertionError(f"{name}: wanted {n}, saw {_count(root, name)}")


def test_good_update_relaunches_into_the_new_build_and_keeps_prev(root: Path) -> None:
    install_daemon(root, OLD_APPLIER)
    _install_new(root, NEW_OK)
    install_conf(root, "token=t\nrespawn_max=0\n")  # an update never needs the respawn budget
    source_hook(root, healthy=1)
    _wait_text(root / "cache" / "ha-harmony.log", "healthy after")
    (_hub_dir(root) / "go").touch()
    log = _wait_text(root / "cache" / "ha-harmony.log", "updated build healthy")
    assert "launching the new build" in log
    assert _count(root, "runs_new") == 1 and _count(root, "runs_old") == 1
    assert not (_hub_dir(root) / "ota-pending").exists()
    assert not (_hub_dir(root) / "ota-restart").exists()
    assert "runs_old" in (_hub_dir(root) / "hub.prev").read_text()  # the build we came from
    assert not (_hub_dir(root) / "ota-rollback").exists()
    assert not (root / "reboot.log").exists()


def test_bad_update_is_rolled_back_once_and_reported(root: Path) -> None:
    install_daemon(root, OLD_APPLIER)
    _install_new(root, NEW_BAD)
    install_conf(root, "token=t\nrespawn_max=0\n")
    source_hook(root, healthy=1)
    _wait_text(root / "cache" / "ha-harmony.log", "healthy after")
    (_hub_dir(root) / "go").touch()
    log = _wait_text(root / "cache" / "ha-harmony.log", "restored hub.prev")
    assert "updated build died" in log and "died_early" in log
    _wait_count(root, "runs_old", 2)  # the previous build is serving again
    rb = (_hub_dir(root) / "ota-rollback").read_text().split("\n")
    assert rb[0] == "build=2" and rb[1] == "reason=died_early"
    assert not (_hub_dir(root) / "ota-pending").exists()
    assert not (_hub_dir(root) / "hub.prev").exists()  # consumed: a second rollback is impossible
    assert 'runs_old' in (_hub_dir(root) / "hub").read_text()
    assert _count(root, "runs_new") == 1 and _count(root, "runs_old") == 2
    assert not (root / "reboot.log").exists()


def _arm_pending_boot(root: Path, new_body: str, old_body: str, counter: str | None = "1") -> None:
    """The state a fresh build leaves when its boot never got healthy: new hub, hub.prev, marker."""
    install_daemon(root, new_body)
    prev = _hub_dir(root) / "hub.prev"
    prev.write_text(f"#!/bin/sh\n{old_body}\n")
    prev.chmod(0o755)
    (_hub_dir(root) / "ota-pending").write_text("build=3\n")
    if counter is not None:
        (root / "cache" / "ha-harmony-boot.n").write_text(counter + "\n")


def test_boot_after_unhealthy_fresh_build_restores_prev_and_resets_strikes(root: Path) -> None:
    _arm_pending_boot(root, "exit 1", 'echo "$@" >"$(dirname "$0")/argv.txt"; sleep 30', counter="2")
    install_conf(root, "token=t\nrespawn_max=0\n")
    _, demo = source_hook(root, healthy=1)
    assert demo == "1"  # the previous build was started, not stock
    rb = (_hub_dir(root) / "ota-rollback").read_text().split("\n")
    assert rb[0] == "build=3" and rb[1] == "reason=boot_unclean"
    assert not (_hub_dir(root) / "ota-pending").exists() and not (_hub_dir(root) / "hub.prev").exists()
    log = _wait_text(root / "cache" / "ha-harmony.log", "healthy after")
    assert "restored hub.prev" in log and "boot strike 1 of 3" in log  # slate wiped, not strike 3
    _wait_for(_hub_dir(root) / "argv.txt")  # the restored (previous) build is the one running
    assert (root / "etc" / "ha-harmony-enable").exists()


def test_rollback_happens_once_only_then_normal_strikes_apply(root: Path) -> None:
    bad = "exit 1"
    _arm_pending_boot(root, bad, bad, counter="1")
    install_conf(root, "token=t\nrespawn_max=0\nreboot_on_early_death=0\n")
    source_hook(root, healthy=1)
    _wait_text(root / "cache" / "ha-harmony.log", "watcher done")
    first = (_hub_dir(root) / "ota-rollback").read_text()
    assert "reason=boot_unclean" in first
    (_hub_dir(root) / "ota-rollback").unlink()
    # the restored build is bad too: boots 2 and 3 are ordinary strikes, never another rollback
    source_hook(root, healthy=1)  # strike 2
    _wait_text(root / "cache" / "ha-harmony.log", "watcher done")
    assert not (_hub_dir(root) / "ota-rollback").exists()
    assert (root / "cache" / "ha-harmony-boot.n").read_text().strip() == "2"
    _, demo = source_hook(root, healthy=1)  # strike 3: back to stock semantics, hook disables itself
    assert demo == "unset"
    assert not (root / "etc" / "ha-harmony-enable").exists()
    assert not (_hub_dir(root) / "ota-rollback").exists()


def test_power_loss_before_the_swap_leaves_the_old_binary_and_only_clears_the_marker(root: Path) -> None:
    install_daemon(root, IMMORTAL)
    before = (_hub_dir(root) / "hub").read_text()
    (_hub_dir(root) / "ota-pending").write_text("build=4\n")  # armed, then the power went
    (_hub_dir(root) / "hub.new").write_text("half a binary")
    install_conf(root)
    _, demo = source_hook(root, healthy=1)
    assert demo == "1"
    assert (_hub_dir(root) / "hub").read_text() == before
    assert not (_hub_dir(root) / "ota-pending").exists()
    assert not (_hub_dir(root) / "ota-rollback").exists()  # nothing to roll back to, nothing to report
    assert "no previous build to restore" in _wait_text(root / "cache" / "ha-harmony.log", "no previous build")


def test_power_loss_after_the_link_before_the_rename_is_harmless(root: Path) -> None:
    install_daemon(root, IMMORTAL)
    before = (_hub_dir(root) / "hub").read_text()
    os.link(_hub_dir(root) / "hub", _hub_dir(root) / "hub.prev")  # hub.prev IS hub (same inode)
    (_hub_dir(root) / "ota-pending").write_text("build=4\n")
    install_conf(root)
    _, demo = source_hook(root, healthy=1)
    assert demo == "1"
    assert (_hub_dir(root) / "hub").read_text() == before
    assert not (_hub_dir(root) / "ota-pending").exists()
    assert not (_hub_dir(root) / "ota-rollback").exists()


def test_a_junk_pending_marker_cannot_inject_into_the_rollback_record(root: Path) -> None:
    _arm_pending_boot(root, "exit 1", "sleep 30", counter=None)
    (_hub_dir(root) / "ota-pending").write_text("build=1; touch /tmp/pwned\nrest\n")
    install_conf(root, "token=t\nrespawn_max=0\n")
    source_hook(root, healthy=1)
    rb = (_hub_dir(root) / "ota-rollback").read_text().split("\n")
    assert rb[0] == "build=0" and rb[1] == "reason=boot_unclean"


# --- review fixes: update inside the health window, marker/ota-build semantics, health file -------------

OLD_EARLY = (  # applies immediately: inside the first health window
    'D=$(dirname "$0"); echo x >>"$D/runs_old"\n'
    'if [ -e "$D/applied" ]; then sleep 30; exit 0; fi\n'
    ': >"$D/applied"; echo build=2 >"$D/ota-pending"; echo prev=1 >>"$D/ota-pending"; echo 2 >"$D/ota-build"\n'
    'ln "$D/hub" "$D/hub.tmp" && mv "$D/hub.tmp" "$D/hub.prev"\n'
    'mv "$D/hub.new" "$D/hub"; echo 2 >"$D/ota-restart"; exit 42'
)


def test_update_inside_the_health_window_is_a_planned_relaunch_not_a_death(root: Path) -> None:
    install_daemon(root, OLD_EARLY)
    _install_new(root, NEW_OK)
    install_conf(root, "token=t\nrespawn_max=0\nreboot_on_early_death=1\n")
    source_hook(root, healthy=1)
    log = _wait_text(root / "cache" / "ha-harmony.log", "healthy after")
    assert "inside the health window - launching the new build" in log
    assert "died within" not in log and not (root / "reboot.log").exists()
    assert _count(root, "runs_new") == 1
    assert not (_hub_dir(root) / "ota-pending").exists()  # cleared at the health mark of the new build
    assert not (_hub_dir(root) / "ota-rollback").exists()
    assert (root / "var" / "volatile" / "ha-harmony.healthy").exists()


def test_update_inside_the_window_that_dies_rolls_back_once_and_restores_ota_build(root: Path) -> None:
    install_daemon(root, OLD_EARLY)
    _install_new(root, NEW_BAD)
    install_conf(root, "token=t\nrespawn_max=0\nreboot_on_early_death=0\n")
    source_hook(root, healthy=1)
    _wait_text(root / "cache" / "ha-harmony.log", "restored hub.prev")
    _wait_count(root, "runs_old", 2)
    assert (_hub_dir(root) / "ota-build").read_text().strip() == "1"  # the previous build id is back
    assert "reason=died_early" in (_hub_dir(root) / "ota-rollback").read_text()
    assert _count(root, "runs_new") == 1 and not (root / "reboot.log").exists()


def test_boot_rollback_restores_ota_build_from_the_marker(root: Path) -> None:
    _arm_pending_boot(root, "exit 1", "sleep 30", counter="1")
    (_hub_dir(root) / "ota-pending").write_text("build=5\nprev=4\n")
    (_hub_dir(root) / "ota-build").write_text("5\n")
    install_conf(root, "token=t\nrespawn_max=0\n")
    source_hook(root, healthy=1)
    assert (_hub_dir(root) / "ota-build").read_text().strip() == "4"
    assert "build=5" in (_hub_dir(root) / "ota-rollback").read_text()


def test_cut_before_the_rename_never_installs_an_older_prev(root: Path) -> None:
    """hub.prev is hard-linked to the running binary before the marker is armed: rolling back is a no-op."""
    install_daemon(root, IMMORTAL)
    before = (_hub_dir(root) / "hub").read_text()
    os.link(_hub_dir(root) / "hub", _hub_dir(root) / "hub.prev")
    (_hub_dir(root) / "ota-pending").write_text("build=9\nprev=8\n")
    (_hub_dir(root) / "ota-build").write_text("9\n")
    install_conf(root)
    source_hook(root, healthy=1)
    assert (_hub_dir(root) / "hub").read_text() == before
    assert not (_hub_dir(root) / "ota-rollback").exists()
    assert (_hub_dir(root) / "ota-build").read_text().strip() == "8"  # the old binary is what runs


def test_health_file_appears_at_the_health_mark_and_goes_while_an_update_is_unproven(root: Path) -> None:
    install_daemon(root, OLD_APPLIER)
    _install_new(root, NEW_OK)
    install_conf(root, "token=t\nrespawn_max=0\n")
    source_hook(root, healthy=1)
    _wait_text(root / "cache" / "ha-harmony.log", "healthy after")
    hf = root / "var" / "volatile" / "ha-harmony.healthy"
    assert hf.exists()
    (_hub_dir(root) / "go").touch()
    _wait_text(root / "cache" / "ha-harmony.log", "updated build healthy")
    assert hf.exists()  # proven again
