"""Boot-hook installer: ordering invariants, the rcS.local syntax trap, and blast radius.

The installer is the only tool permitted to write /etc/init.d, so most of these tests are
about what it must *never* do, and about the ordering that makes every half-finished install
a hub that still boots stock.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))

import install_boot_hook as ibh

STOCK_RCS_LOCAL = b"""#!/bin/sh
# product specific configuration
echo 1 > /proc/sys/vm/overcommit_memory
/usr/sbin/hciattach ttyS0 bcsp
"""


class FakeHub:
    """A filesystem-ish fake: records every argv, serves cat/test/mv/chmod/rm and `sh -n`."""

    def __init__(self, *, files: dict[str, bytes] | None = None, syntax_bad: set[str] | None = None):
        self.files: dict[str, bytes] = {
            ibh.RCS_LOCAL: STOCK_RCS_LOCAL,
            ibh.DAEMON: b"\x7fELF-ish",
        }
        if files is not None:
            self.files = dict(files)
        self.exec_bits = {ibh.DAEMON}
        self.log: list[list[str]] = []
        self.syntax_bad = syntax_bad or set()  # paths whose `sh -n` should fail
        self.order: list[str] = []  # high-level milestones, for ordering assertions

    def run(self, argv):
        self.log.append(list(argv))
        if argv[:1] == ["cat"]:
            p = argv[1]
            return (0, self.files[p], b"") if p in self.files else (1, b"", b"No such file")
        if argv[:2] == ["test", "-e"]:
            return (0 if argv[2] in self.files else 1, b"", b"")
        if argv[:2] == ["test", "-x"]:
            # `test -x` is false for a file that does not exist, exec bit or not.
            ok = argv[2] in self.files and argv[2] in self.exec_bits
            return (0 if ok else 1, b"", b"")
        if argv[:2] == ["sh", "-n"]:
            p = argv[2]
            if p in self.syntax_bad:
                return (2, b"", b"syntax error: unexpected end of file")
            return (0, b"", b"")
        if argv[:1] == ["chmod"]:
            return (0, b"", b"")
        if argv[:1] == ["mkdir"]:
            return (0, b"", b"")
        if argv[:1] == ["mv"]:
            src, dst = argv[1], argv[2]
            if src not in self.files:
                return (1, b"", b"No such file")
            self.files[dst] = self.files.pop(src)
            if dst == ibh.MARKER:
                self.order.append("arm")
            elif dst == ibh.RCS_HARMONY:
                self.order.append("hook")
            elif dst == ibh.RCS_LOCAL:
                self.order.append("edit-rcs-local")
            elif dst == ibh.CONF:
                self.order.append("conf")
            elif dst.startswith(ibh.BACKUP_DIR):
                self.order.append("backup")
            return (0, b"", b"")
        if argv[:1] == ["rm"]:
            for p in argv[1:]:
                if p.startswith("-"):
                    continue
                self.files.pop(p, None)
                if p == ibh.MARKER:
                    self.order.append("disarm")
            return (0, b"", b"")
        return (127, b"", f"applet not found: {argv[0]}".encode())

    def run_input(self, argv, data):
        self.log.append(list(argv))
        m = re.match(r"cat > (\S+)$", argv[-1]) if argv[:2] == ["sh", "-c"] else None
        if m:
            self.files[m.group(1)] = data
            return (0, b"", b"")
        return (127, b"", b"unexpected run_input")

    def run_foreground(self, argv):
        self.log.append(list(argv))
        return 0


@pytest.fixture
def hub() -> FakeHub:
    return FakeHub()


def install(hub: FakeHub, **kw) -> int:
    return ibh.HookInstaller(hub).install(kw.pop("token", "tok"), kw.pop("port", 8099), kw.pop("hub_id", None), **kw)


# --- blast radius ---------------------------------------------------------------


FORBIDDEN = (
    r"/etc/init\.d/rcS(?![.\w])",  # stock rcS itself: read never, write never
    r"/etc/inittab",
    r"/etc/rc\d",
    r"/dev/mtd",
    r"/etc/tdeenable",  # our SSH depends on it; never ours to touch
    r"/etc/nowatchdog",  # watchdog policy is a separate, deliberate decision
    r"mtd_?debug|flash_erase|flashcp",
)


@pytest.mark.parametrize("action", ["install", "uninstall", "status"])
def test_never_touches_anything_outside_its_remit(action: str) -> None:
    hub = FakeHub()
    inst = ibh.HookInstaller(hub)
    if action == "install":
        inst.install("tok", 8099, None)
    elif action == "uninstall":
        inst.install("tok", 8099, None)
        inst.uninstall()
    else:
        inst.status()
    flat = [" ".join(a) for a in hub.log]
    for cmd in flat:
        for pat in FORBIDDEN:
            assert not re.search(pat, cmd), f"{action} touched a forbidden path: {cmd!r}"


def test_uses_no_applet_the_hub_lacks(hub: FakeHub) -> None:
    """The hub's BusyBox 1.13.4 has no head/find/awk/timeout/df/dd/wc/readlink."""
    inst = ibh.HookInstaller(hub)
    inst.install("tok", 8099, None)
    inst.uninstall()
    inst.status()
    missing = (
        "head", "find", "awk", "timeout", "df", "dd", "wc", "readlink", "sha256sum",
        "nohup", "setsid", "seq", "xargs", "sort", "uniq", "tar", "basename",
    )  # fmt: skip
    for argv in hub.log:
        assert argv[0] not in missing, f"invoked a missing applet: {argv}"
        joined = " ".join(argv)
        assert "grep -E" not in joined, f"grep -E is unsupported on the hub: {argv}"


# --- ordering: every intermediate state must boot stock -------------------------


def test_marker_is_created_last(hub: FakeHub) -> None:
    install(hub)
    assert hub.order[-1] == "arm", f"marker must be armed last, got order {hub.order}"
    assert hub.order.index("backup") < hub.order.index("hook") < hub.order.index("arm")
    assert hub.order.index("edit-rcs-local") < hub.order.index("arm")


def test_uninstall_disarms_first(hub: FakeHub) -> None:
    install(hub)
    hub.order.clear()
    ibh.HookInstaller(hub).uninstall()
    assert hub.order[0] == "disarm", f"marker must be removed first, got {hub.order}"


def test_backup_is_written_before_rcs_local_is_edited(hub: FakeHub) -> None:
    install(hub)
    backups = [p for p in hub.files if p.startswith(ibh.BACKUP_DIR)]
    assert len(backups) == 1
    assert hub.files[backups[0]] == STOCK_RCS_LOCAL, "backup must hold the original, pre-edit"


# --- the syntax trap: a broken rcS.local costs SSH before dropbear starts -------


def test_aborts_and_restores_when_our_edit_breaks_rcs_local() -> None:
    """The dangerous case. Must restore the original and never arm."""
    hub = FakeHub(syntax_bad=set())

    # make `sh -n` fail only *after* our block has been appended
    real_run = hub.run

    def run(argv):
        appended = ibh.BEGIN.encode() in hub.files.get(ibh.RCS_LOCAL, b"")
        if argv[:2] == ["sh", "-n"] and argv[2] == ibh.RCS_LOCAL and appended:
            hub.log.append(list(argv))
            return (2, b"", b"syntax error near unexpected token")
        return real_run(argv)

    hub.run = run  # type: ignore[method-assign]
    with pytest.raises(ibh.InstallError, match="stopped parsing"):
        install(hub)
    assert ibh.MARKER not in hub.files, "must not arm after a failed syntax check"
    assert hub.files[ibh.RCS_LOCAL] == STOCK_RCS_LOCAL, "must restore the original rcS.local"


def test_refuses_when_rcs_local_is_already_broken() -> None:
    hub = FakeHub(syntax_bad={ibh.RCS_LOCAL})
    with pytest.raises(ibh.InstallError, match="does not parse before we touch it"):
        install(hub)
    assert ibh.RCS_HARMONY not in hub.files, "must change nothing when preflight fails"
    assert ibh.MARKER not in hub.files


def test_removes_the_hook_when_it_does_not_parse_on_device() -> None:
    hub = FakeHub(syntax_bad={ibh.RCS_HARMONY})
    with pytest.raises(ibh.InstallError, match="does not parse on the device"):
        install(hub)
    assert ibh.RCS_HARMONY not in hub.files
    assert ibh.MARKER not in hub.files
    assert hub.files[ibh.RCS_LOCAL] == STOCK_RCS_LOCAL


def test_refuses_without_rcs_local() -> None:
    hub = FakeHub(files={ibh.DAEMON: b"x"})
    with pytest.raises(ibh.InstallError, match="not the expected stock firmware"):
        install(hub)


# --- preflight and idempotency --------------------------------------------------


def test_refuses_when_the_daemon_is_missing() -> None:
    hub = FakeHub(files={ibh.RCS_LOCAL: STOCK_RCS_LOCAL})
    with pytest.raises(ibh.InstallError, match="missing or not executable"):
        install(hub)
    assert ibh.MARKER not in hub.files


def test_t2_rehearsal_may_install_without_a_working_daemon() -> None:
    """T2 deliberately installs a broken daemon to prove the strike guard on hardware."""
    hub = FakeHub(files={ibh.RCS_LOCAL: STOCK_RCS_LOCAL})
    assert install(hub, require_daemon=False) == 0
    assert ibh.MARKER in hub.files


def test_installing_twice_does_not_duplicate_the_block(hub: FakeHub) -> None:
    install(hub)
    first = hub.files[ibh.RCS_LOCAL]
    install(hub)
    assert hub.files[ibh.RCS_LOCAL].count(ibh.BEGIN.encode()) == 1
    assert hub.files[ibh.RCS_LOCAL] == first


def test_uninstall_restores_rcs_local_byte_for_byte(hub: FakeHub) -> None:
    install(hub)
    assert hub.files[ibh.RCS_LOCAL] != STOCK_RCS_LOCAL
    ibh.HookInstaller(hub).uninstall()
    assert hub.files[ibh.RCS_LOCAL] == STOCK_RCS_LOCAL, "rcS.local must come back exactly"
    assert ibh.RCS_HARMONY not in hub.files
    assert ibh.MARKER not in hub.files


def test_uninstall_is_safe_on_a_partial_install(hub: FakeHub) -> None:
    """Must not raise when there is nothing (or only some) of ours present."""
    assert ibh.HookInstaller(hub).uninstall() == 0
    assert hub.files[ibh.RCS_LOCAL] == STOCK_RCS_LOCAL


def test_uninstall_keeps_the_token_unless_purge_is_asked(hub: FakeHub) -> None:
    install(hub)
    ibh.HookInstaller(hub).uninstall()
    assert ibh.CONF in hub.files
    install(hub)
    ibh.HookInstaller(hub).uninstall(purge=True)
    assert ibh.CONF not in hub.files


# --- what actually lands on the device ------------------------------------------


def test_installed_block_is_guarded_by_the_marker(hub: FakeHub) -> None:
    install(hub)
    body = hub.files[ibh.RCS_LOCAL].decode()
    assert STOCK_RCS_LOCAL.decode() in body, "stock content must be preserved verbatim"
    assert f"[ -r {ibh.MARKER} ]" in body, "block must be inert without the marker"
    assert f"[ -r {ibh.RCS_HARMONY} ]" in body, "and inert if the hook file is gone"
    assert f". {ibh.RCS_HARMONY}" in body, "must source (not exec) so DEMO_MODE propagates"


def test_hook_is_installed_from_the_repo_file_verbatim(hub: FakeHub) -> None:
    install(hub)
    assert hub.files[ibh.RCS_HARMONY] == ibh.HOOK_SRC.read_bytes()


def test_config_is_written_without_a_shell_syntax(hub: FakeHub) -> None:
    install(hub, token="s3cret", port=9000, hub_id="ha-harmony-x")
    conf = hub.files[ibh.CONF].decode()
    assert conf == "token=s3cret\nport=9000\nhub_id=ha-harmony-x\n"
    assert "export" not in conf and "$" not in conf, "hub.conf is parsed, not sourced"


def test_write_verifies_content_and_raises_on_mismatch(hub: FakeHub) -> None:
    """A silently truncated write to a boot file is the worst outcome; verify catches it."""
    real = hub.run

    def run(argv):
        if argv[:1] == ["mv"] and argv[2] == ibh.RCS_HARMONY:
            hub.log.append(list(argv))
            hub.files[ibh.RCS_HARMONY] = b"truncated"
            hub.files.pop(argv[1], None)
            return (0, b"", b"")
        return real(argv)

    hub.run = run  # type: ignore[method-assign]
    with pytest.raises(ibh.InstallError, match="verify failed"):
        install(hub)
    assert ibh.MARKER not in hub.files


# --- CLI ------------------------------------------------------------------------


@pytest.mark.parametrize("cmd", ["install", "uninstall"])
def test_write_commands_require_yes(cmd: str, capsys) -> None:
    args = [cmd] + (["--token", "t"] if cmd == "install" else [])
    assert ibh.main(args, runner=FakeHub()) == 2
    assert "--yes" in capsys.readouterr().err


def test_status_needs_no_yes_and_reports_disarmed(capsys) -> None:
    assert ibh.main(["status"], runner=FakeHub()) == 0
    out = capsys.readouterr().out
    assert "disarmed" in out and "boots pure stock" in out


def test_status_reports_armed_after_install(capsys) -> None:
    hub = FakeHub()
    assert ibh.main(["install", "--yes", "--token", "t"], runner=hub) == 0
    capsys.readouterr()
    assert ibh.main(["status"], runner=hub) == 0
    out = capsys.readouterr().out
    assert "PRESENT (armed)" in out and "contains our block" in out


def test_errors_exit_nonzero(capsys) -> None:
    hub = FakeHub(syntax_bad={ibh.RCS_LOCAL})
    assert ibh.main(["install", "--yes", "--token", "t"], runner=hub) == 1
    assert "ERROR" in capsys.readouterr().err
