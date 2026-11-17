"""Phase 4 boot hook: guard state machine, blast-radius rules, applet floor.

`tools/boot/rcS.harmony` is sourced by stock `/etc/init.d/rcS.local` four lines before
dropbear starts, so a mistake here can leave the hub with no way in. Every test below
exists because of a specific way that can happen. Nothing here touches a device: the
script is parameterised by HAH_ROOT and driven against a temp tree.
"""

from __future__ import annotations

import re
import shutil
import subprocess
from pathlib import Path

import pytest

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
    for d in ("etc", "cache", "data/ha-harmony"):
        (tmp_path / d).mkdir(parents=True)
    (tmp_path / "etc" / "ha-harmony-enable").write_text("")
    return tmp_path


def install_daemon(root: Path, body: str) -> None:
    d = root / "data" / "ha-harmony" / "hub"
    d.write_text(f"#!/bin/sh\n{body}\n")
    d.chmod(0o755)


def install_conf(root: Path, text: str = "token=benchtok\nport=8099\nhub_id=ha-harmony-test\n") -> None:
    (root / "data" / "ha-harmony" / "hub.conf").write_text(text)


def source_hook(root: Path, *, healthy: int = 1, limit: int = 3, restrict_path: bool = False) -> tuple[str, str]:
    """Source the hook the way rcS.local does and report DEMO_MODE afterwards.

    Returns (stdout, demo_mode) where demo_mode is "1" or "unset".
    """
    script = f'. "{HOOK}"; echo "__DEMO__=${{DEMO_MODE:-unset}}"'
    env = {
        "HAH_ROOT": str(root),
        "HAH_HEALTHY_SECS": str(healthy),
        "HAH_MAX_UNCLEAN": str(limit),
    }
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
        env["PATH"] = str(bindir)
    else:
        env["PATH"] = "/usr/bin:/bin"

    p = subprocess.run(
        ["/bin/sh", "-c", script], capture_output=True, text=True, env=env, timeout=60, check=False
    )
    assert p.returncode == 0, f"hook returned {p.returncode}: {p.stderr}"
    m = re.search(r"__DEMO__=(\S+)", p.stdout)
    assert m, f"no marker in output: {p.stdout!r}"
    return p.stdout, m.group(1)


# --- blast radius: the rules that keep the hub reachable -------------------------


def test_hook_parses_as_posix_sh() -> None:
    """A syntax error here aborts rcS itself, before dropbear starts."""
    p = subprocess.run(["/bin/sh", "-n", str(HOOK)], capture_output=True, text=True, check=False)
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


def test_synchronous_path_does_no_blocking_work() -> None:
    """Everything slow must be backgrounded; the sync path runs before dropbear.

    `sleep` is allowed only inside a backgrounded subshell, and the daemon itself must
    be launched with a trailing `&`.
    """
    code = re.sub(r"#.*", "", HOOK.read_text())
    for line in code.splitlines():
        if "sleep" in line:
            # the only sleep is in the watcher subshell, which is backgrounded
            assert line.strip().startswith("sleep"), f"unexpected sleep placement: {line!r}"
    assert re.search(r'"\$hah_daemon".*?&\s*$', code, re.DOTALL | re.MULTILINE), "daemon must be backgrounded"
    # No network or device access on the synchronous path.
    for forbidden in ("ifconfig", "udhcpc", "ping", "/dev/i2s", "nc ", "wget", "curl"):
        assert forbidden not in code, f"synchronous path must not touch {forbidden}"


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
    assert (root / "cache" / "ha-harmony.pid").exists()


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
    import time

    time.sleep(2.5)
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


def _wait_gone(p: Path, tries: int = 60) -> None:
    import time

    for _ in range(tries):
        if not p.exists():
            return
        time.sleep(0.25)
