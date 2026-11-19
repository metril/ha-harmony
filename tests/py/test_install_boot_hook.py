"""Boot-hook installer: ordering invariants, the rcS.local syntax trap, and blast radius.

The installer is the only tool permitted to write /etc/init.d, so most of these tests are
about what it must *never* do, and about the ordering that makes every half-finished install
a hub that still boots stock.
"""

from __future__ import annotations

import hashlib
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


DAEMON_BYTES = b"\x7fELF-ish"
DAEMON_MD5 = hashlib.md5(DAEMON_BYTES).hexdigest()


class FakeHub:
    """A filesystem-ish fake: records every argv, serves cat/test/mv/chmod/rm and `sh -n`."""

    def __init__(self, *, files: dict[str, bytes] | None = None, syntax_bad: set[str] | None = None):
        self.files: dict[str, bytes] = {
            ibh.RCS_LOCAL: STOCK_RCS_LOCAL,
            ibh.DAEMON: DAEMON_BYTES,
            ibh.NOWATCHDOG: b"",
        }
        if files is not None:
            self.files = dict(files)
        self.exec_bits = {ibh.DAEMON}
        self.log: list[list[str]] = []
        self.syntax_bad = syntax_bad or set()  # paths whose `sh -n` should fail
        self.order: list[str] = []  # high-level milestones, for ordering assertions
        self.inodes: dict[str, int] = {}
        self.next_inode = 100
        self.copyup = False  # True: an mv over an existing path yields a NEW inode (non-atomic)

    def run(self, argv):
        self.log.append(list(argv))
        if argv[:1] == ["cat"]:
            p = argv[1]
            return (0, self.files[p], b"") if p in self.files else (1, b"", b"No such file")
        if argv[:1] == ["sh"] and argv[1:2] and argv[1].endswith("selftest.sh"):
            self.selftest_rc = getattr(self, "selftest_rc", 0)
            return (self.selftest_rc, b"PASS fake\n", b"")
        if argv[:2] == ["ls", "-i"]:
            p = argv[2]
            if p not in self.files:
                return (1, b"", b"No such file")
            return (0, f"{self.inodes.setdefault(p, 64)} {p}\n".encode(), b"")
        if argv[:1] == ["md5sum"]:
            p = argv[1]
            if p not in self.files:
                return (1, b"", b"No such file")
            return (0, f"{hashlib.md5(self.files[p]).hexdigest()}  {p}\n".encode(), b"")
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
            existed = dst in self.files
            self.files[dst] = self.files.pop(src)
            if existed and self.copyup:
                self.next_inode += 1
                self.inodes[dst] = self.next_inode
            else:
                self.inodes[dst] = self.inodes.get(src, 64)
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
            self.next_inode += 1
            self.inodes[m.group(1)] = self.next_inode
            return (0, b"", b"")
        return (127, b"", b"unexpected run_input")

    def run_foreground(self, argv):
        self.log.append(list(argv))
        return 0


@pytest.fixture(autouse=True)
def _token_env(monkeypatch) -> None:
    monkeypatch.setenv("HAH_TOKEN", "s3cr3t-token-value")


@pytest.fixture
def hub() -> FakeHub:
    return FakeHub()


def install(hub: FakeHub, **kw) -> int:
    kw.setdefault("expect_md5", DAEMON_MD5)
    return ibh.HookInstaller(hub).install(kw.pop("token", "tok"), kw.pop("port", 8099), kw.pop("hub_id", None), **kw)


# --- blast radius ---------------------------------------------------------------


FORBIDDEN = (
    r"/etc/init\.d/rcS(?![.\w])",  # stock rcS itself: read never, write never
    r"/etc/inittab",
    r"/etc/rc\d",
    r"/dev/mtd",
    r"/etc/tdeenable",  # our SSH depends on it; never ours to touch
    r"(?<!test -e )/etc/nowatchdog",  # only `test -e` is allowed; watchdog policy is a separate, deliberate decision
    r"mtd_?debug|flash_erase|flashcp",
)


@pytest.mark.parametrize("action", ["install", "uninstall", "status"])
def test_never_touches_anything_outside_its_remit(action: str) -> None:
    hub = FakeHub()
    inst = ibh.HookInstaller(hub)
    if action == "install":
        inst.install("tok", 8099, None, expect_md5=DAEMON_MD5)
    elif action == "uninstall":
        inst.install("tok", 8099, None, expect_md5=DAEMON_MD5)
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
    inst.install("tok", 8099, None, expect_md5=DAEMON_MD5)
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
    hub = FakeHub(files={ibh.DAEMON: DAEMON_BYTES, ibh.NOWATCHDOG: b""})
    with pytest.raises(ibh.InstallError, match="not the expected stock firmware"):
        install(hub)


# --- preflight and idempotency --------------------------------------------------


def test_refuses_when_the_daemon_is_missing() -> None:
    hub = FakeHub(files={ibh.RCS_LOCAL: STOCK_RCS_LOCAL})
    with pytest.raises(ibh.InstallError, match="missing or not executable"):
        install(hub)
    assert ibh.MARKER not in hub.files


def test_there_is_no_way_to_install_without_a_daemon(capsys) -> None:
    """A missing-daemon install cannot rehearse the strike guard, so no flag may allow one.

    An `--allow-missing-daemon` flag once existed "for the T2 rehearsal". It was useless for
    that: the hook leaves stock alone, spending no strike, when the daemon is missing. The
    real rehearsal (run on hardware 2026-11-17) used a daemon that is present but exits 1.
    """
    with pytest.raises(SystemExit):
        ibh.main(["install", "--yes", "--expect-md5", DAEMON_MD5, "--allow-missing-daemon"], runner=FakeHub())
    assert "unrecognized arguments" in capsys.readouterr().err


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
    args = [cmd] + (["--expect-md5", DAEMON_MD5] if cmd == "install" else [])
    assert ibh.main(args, runner=FakeHub()) == 2
    assert "--yes" in capsys.readouterr().err


def test_status_needs_no_yes_and_reports_disarmed(capsys) -> None:
    assert ibh.main(["status"], runner=FakeHub()) == 0
    out = capsys.readouterr().out
    assert "disarmed" in out and "boots pure stock" in out


def test_status_reports_armed_after_install(capsys) -> None:
    hub = FakeHub()
    assert ibh.main(["install", "--yes", "--expect-md5", DAEMON_MD5], runner=hub) == 0
    capsys.readouterr()
    assert ibh.main(["status"], runner=hub) == 0
    out = capsys.readouterr().out
    assert "PRESENT (armed)" in out and "contains our block" in out


def test_errors_exit_nonzero(capsys) -> None:
    hub = FakeHub(syntax_bad={ibh.RCS_LOCAL})
    assert ibh.main(["install", "--yes", "--expect-md5", DAEMON_MD5], runner=hub) == 1
    assert "ERROR" in capsys.readouterr().err


# --- T3: md5 gate, token handling, disarm, status, rf keys -----------------------


def test_install_refuses_on_md5_mismatch_and_writes_nothing(hub: FakeHub) -> None:
    with pytest.raises(ibh.InstallError, match="md5"):
        install(hub, expect_md5="0" * 32)
    assert hub.files[ibh.RCS_LOCAL] == STOCK_RCS_LOCAL
    assert ibh.MARKER not in hub.files and ibh.CONF not in hub.files
    assert not any(c[0] in ("mv", "rm") or c[:2] == ["sh", "-c"] for c in hub.log)


def test_expect_md5_is_mandatory(hub: FakeHub) -> None:
    with pytest.raises(ibh.InstallError, match="expect-md5"):
        ibh.HookInstaller(hub).install("tok", 8099, None)


def test_rf_slot_lands_in_conf(hub: FakeHub) -> None:
    install(hub, rf_slot=2)
    conf = hub.files[ibh.CONF].decode()
    assert "rf=spi\nrf_slot=2\n" in conf
    with pytest.raises(ibh.InstallError):
        install(FakeHub(), rf_slot=16)


def test_rf_flag_enables_radio_without_a_slot(hub: FakeHub) -> None:
    install(hub, rf=True)
    conf = hub.files[ibh.CONF].decode()
    assert "rf=spi\n" in conf
    assert "rf_slot" not in conf
    hub2 = FakeHub()
    install(hub2)
    assert "rf=" not in hub2.files[ibh.CONF].decode()


def test_keep_lua_writes_the_switch(hub: FakeHub) -> None:
    install(hub, stop_lua=False)
    assert "stop_lua=0\n" in hub.files[ibh.CONF].decode()


def test_token_never_in_commands_stdout_or_status(hub: FakeHub, tmp_path, capsys) -> None:
    secret = "TOP-SECRET-TOKEN-9"
    tf = tmp_path / "tok"
    tf.write_text(secret + "\n")
    args = ["install", "--yes", "--expect-md5", DAEMON_MD5, "--token-file", str(tf), "--rf-slot", "2"]
    assert ibh.main(args, runner=hub) == 0
    assert ibh.main(["status"], runner=hub) == 0
    cap = capsys.readouterr()
    assert secret not in cap.out + cap.err
    assert "token=<redacted>" in cap.out
    assert not any(secret in " ".join(c) for c in hub.log), "token leaked into a command line"
    assert secret in hub.files[ibh.CONF].decode()  # it does reach the (600) conf, via stdin


def test_token_comes_from_env_or_file_never_argv(capsys) -> None:
    with pytest.raises(SystemExit):
        ibh.main(["install", "--yes", "--token", "x", "--expect-md5", DAEMON_MD5], runner=FakeHub())
    assert "unrecognized arguments" in capsys.readouterr().err


def test_no_token_anywhere_is_refused(monkeypatch, capsys) -> None:
    monkeypatch.delenv("HAH_TOKEN")
    hub = FakeHub()
    assert ibh.main(["install", "--yes", "--expect-md5", DAEMON_MD5], runner=hub) == 1
    assert ibh.CONF not in hub.files


def test_disarm_touches_only_the_marker(hub: FakeHub, capsys) -> None:
    install(hub)
    before = dict(hub.files)
    hub.log.clear()
    assert ibh.main(["disarm", "--yes"], runner=hub) == 0
    assert ibh.MARKER not in hub.files
    assert {k: v for k, v in hub.files.items()} == {k: v for k, v in before.items() if k != ibh.MARKER}
    writes = [c for c in hub.log if c[0] in ("rm", "mv", "chmod", "mkdir") or c[:2] == ["sh", "-c"]]
    assert writes == [["rm", "-f", ibh.MARKER]]


def test_disarm_needs_yes_and_is_idempotent(hub: FakeHub, capsys) -> None:
    assert ibh.main(["disarm"], runner=hub) == 2
    assert ibh.main(["disarm", "--yes"], runner=hub) == 0
    assert "already" in capsys.readouterr().out


def test_status_reports_md5_counter_and_both_log_tails(hub: FakeHub, capsys) -> None:
    install(hub)
    hub.files[ibh.COUNTER] = b"2\n"
    hub.files[ibh.LOG] = b"\n".join(f"hook{i}".encode() for i in range(20))
    hub.files[ibh.DAEMON_LOG] = b"daemon says hi\n"
    assert ibh.main(["status"], runner=hub) == 0
    out = capsys.readouterr().out
    assert DAEMON_MD5 in out
    assert "strike counter: 2" in out
    assert "hook19" in out and "hook0\n" not in out
    assert "daemon says hi" in out
    assert not any(c[0] in ("tail", "head", "wc") for c in hub.log)


# --- review fixes: validation, counter clear, atomicity, reboot switch ------------


@pytest.mark.parametrize("tok", ["a b", "a\nb", "a\r", "x;y", "$(id)", "", "t" * 300])
def test_bad_token_charset_refused_before_any_write(hub: FakeHub, tok: str) -> None:
    with pytest.raises(ibh.InstallError, match="token"):
        install(hub, token=tok)
    assert hub.log == []


@pytest.mark.parametrize("hid", ["a b", "a\nb", "x;touch y", "a=b", "", "$(x)"])
def test_bad_hub_id_refused(hub: FakeHub, hid: str) -> None:
    with pytest.raises(ibh.InstallError, match="hub id"):
        install(hub, hub_id=hid)
    assert hub.log == []


def test_real_shaped_token_and_base64_padding_accepted(hub: FakeHub) -> None:
    install(hub, token="0123456789abcdef0123456789abcdef")
    install(FakeHub(), token="YWJj+/Zg==")


def test_install_clears_the_strike_counter_before_arming(hub: FakeHub) -> None:
    hub.files[ibh.COUNTER] = b"2\n"
    install(hub)
    assert ibh.COUNTER not in hub.files
    rm = next(i for i, c in enumerate(hub.log) if c == ["rm", "-f", ibh.COUNTER])
    arm = next(i for i, c in enumerate(hub.log) if c[:1] == ["mv"] and c[2] == ibh.MARKER)
    assert rm < arm


def test_reboot_on_early_death_switch_written_only_when_off(hub: FakeHub) -> None:
    install(hub)
    assert "reboot_on_early_death" not in hub.files[ibh.CONF].decode()
    h2 = FakeHub()
    install(h2, reboot_on_early_death=False)
    assert "reboot_on_early_death=0\n" in h2.files[ibh.CONF].decode()


def test_nonatomic_rcs_local_replace_only_warns(capsys) -> None:
    """rcS sources rcS.local under -x, and unionfs inode numbers may differ spuriously."""
    hub = FakeHub()
    hub.copyup = True
    install(hub)
    assert ibh.MARKER in hub.files
    assert "WARNING" in capsys.readouterr().out
    assert "allow-nonatomic-rcs-local" not in ibh.build_parser().format_help()


def test_atomic_rename_installs_silently(hub: FakeHub, capsys) -> None:
    install(hub)
    assert ibh.MARKER in hub.files and "WARNING" not in capsys.readouterr().out
    assert any(c[:2] == ["ls", "-i"] for c in hub.log)


def test_install_refuses_when_the_stock_watchdog_is_enabled() -> None:
    hub = FakeHub()
    del hub.files[ibh.NOWATCHDOG]
    with pytest.raises(ibh.InstallError, match="watchdog"):
        install(hub)
    assert not any(c[0] in ("mv", "rm") for c in hub.log)
    assert ibh.MARKER not in hub.files


def test_selftest_writes_only_under_tmp_hah_selftest_and_cleans_up(capsys) -> None:
    hub = FakeHub()
    assert ibh.main(["selftest", "--yes"], runner=hub) == 0
    for c in hub.log:
        flat = " ".join(c)
        if c[0] in ("mkdir", "rm", "chmod", "mv") or c[:2] == ["sh", "-c"]:
            assert ibh.SELFTEST_DIR in flat, f"selftest wrote outside its dir: {flat}"
        assert not re.search(r"/etc/|/data|/cache|/dev/", flat.replace(ibh.SELFTEST_DIR, "")), flat
    assert hub.log[-1] == ["rm", "-rf", ibh.SELFTEST_DIR]
    uploads = [c for c in hub.log if c[:2] == ["sh", "-c"]]
    assert len(uploads) == 2
    assert hub.files[ibh.RCS_LOCAL] == STOCK_RCS_LOCAL and ibh.MARKER not in hub.files


def test_selftest_failure_exits_nonzero_and_still_cleans_up() -> None:
    hub = FakeHub()
    hub.selftest_rc = 1
    assert ibh.main(["selftest", "--yes"], runner=hub) == 1
    assert hub.log[-1] == ["rm", "-rf", ibh.SELFTEST_DIR]


def test_selftest_needs_yes() -> None:
    hub = FakeHub()
    assert ibh.main(["selftest"], runner=hub) == 2
    assert hub.log == []
