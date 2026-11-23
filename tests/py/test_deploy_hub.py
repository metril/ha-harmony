"""deploy_hub against a fake, stateful hub behind a fake SSH transport (no network, no sleeps)."""

import hashlib
import re
import shlex
import sys
from datetime import UTC, datetime
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import deploy_hub as dh

# Real-shaped BusyBox output (no readlink/lsof/df on the stock device).
PS_STOCK = """\
  PID USER       VSZ STAT COMMAND
    1 root      1568 S    init
    2 root         0 SW<  [kthreadd]
   88 root      2204 S    /usr/sbin/dropbear -p 22
  101 root     14232 S    /usr/bin/hal -f -s
  102 root         0 Z    [hal]
  140 root     21480 S    lua main.lua
  141 root     30120 S    ./luaworks --syslog --datadir=/data
  150 root      1320 R    ps
"""
FD_LINES = (
    "/proc/101 l-wx------    1 root     root            64 Jan  1 00:01 5 -> /dev/i2s\n"
    "/proc/101 lrwx------    1 root     root            64 Jan  1 00:01 6 -> /dev/rfspi\n"
)
DF_OUT = (
    "Filesystem           1K-blocks      Used Available Use% Mounted on\n"
    "mtd4                      5120      3900      1220  76% /mnt/data\n"
)
DF_WRAPPED = (
    "Filesystem           1K-blocks      Used Available Use% Mounted on\n"
    "a-very-long-filesystem-name-that-wraps\n"
    "                          5120      3900      1220  76% /mnt/data\n"
)
INIT_ONLY = b"  PID USER VSZ STAT COMMAND\n    1 root 1568 S    init\n"


def stock_procs() -> list[list]:
    """[pid, stat, cmd, devices-held] rows."""
    return [
        [1, "S", "init", []],
        [2, "SW<", "[kthreadd]", []],
        [88, "S", "/usr/sbin/dropbear -p 22", []],
        [101, "S", "/usr/bin/hal -f -s", ["/dev/i2s", "/dev/rfspi"]],
        [102, "Z", "[hal]", []],
        [140, "S", "lua main.lua", []],
        [141, "S", "./luaworks --syslog --datadir=/data", []],
    ]


class FakeHub:
    """Interprets exactly the commands deploy_hub may emit; anything else fails the test."""

    def __init__(self, free_kb=1220, df=True, corrupt=(), stuck=(), up_after=4,
                 reboot_fires=True, stock_after_ps=0, upload_rc=0, sim_rc=0):  # fmt: skip
        self.procs = stock_procs()
        self.files: dict[str, bytes] = {}
        self.free_kb, self.df, self.corrupt, self.stuck = free_kb, df, set(corrupt), set(stuck)
        self.up_after, self.reboot_fires = up_after, reboot_fires
        self.stock_after_ps, self.upload_rc, self.sim_rc = stock_after_ps, upload_rc, sim_rc
        self.uptime_s = 3600.0  # seconds since boot; drops when a reboot fires
        self.down = 0  # remaining uname polls that fail (ssh unavailable)
        self.booting_ps = 0  # remaining ps polls that show only init
        self.log: list[list[str]] = []
        self.fg: list[list[str]] = []
        self.detach_alive = True
        self.detached: list[str] = []

    def _boot(self) -> None:
        self.procs = stock_procs()
        self.files = {p: d for p, d in self.files.items() if not p.startswith("/tmp/")}
        self.booting_ps = self.stock_after_ps

    def _ps(self) -> bytes:
        lines = ["  PID USER       VSZ STAT COMMAND"]
        lines += [f"{pid:5d} root      1000 {stat:<4} {cmd}" for pid, stat, cmd, _ in self.procs]
        return ("\n".join(lines) + "\n").encode()

    def _df(self) -> bytes:
        return DF_OUT.replace("1220", str(self.free_kb)).encode()

    def run(self, argv):
        self.log.append(argv)
        a = argv
        if self.down > 0:
            if a == ["uname", "-a"]:
                self.down -= 1
                if self.down == 0:
                    self._boot()
            return 255, b"", b"ssh: connect refused"
        if a == ["uname", "-a"]:
            return 0, b"Linux harmony 2.6.31 mips GNU/Linux\n", b""
        if a == ["ps"]:
            if self.booting_ps > 0:
                self.booting_ps -= 1
                return 0, INIT_ONLY, b""
            return 0, self._ps(), b""
        if a[:2] == ["sh", "-c"] and a[2] == dh.FD_SCAN:
            out = "".join(
                f"/proc/{pid} lrwx------ 1 root root 64 Jan 1 00:00 {3 + i} -> {d}\n"
                for pid, _, _, devs in self.procs
                for i, d in enumerate(devs)
            )
            return (0, out.encode(), b"") if out else (1, b"", b"")
        if a[0] == "kill":
            pid = int(a[1])
            if pid not in self.stuck:
                self.procs = [p for p in self.procs if p[0] != pid]
            return 0, b"", b""
        if a[:2] == ["df", "-k"]:
            return (0, self._df(), b"") if self.df else (127, b"", b"df: not found")
        if a[0] == dh.REMOTE_BUSYBOX and a[1:3] == ["df", "-k"]:
            ok = dh.REMOTE_BUSYBOX in self.files
            return (0, self._df(), b"") if ok else (127, b"", b"not found")
        if a[:2] == ["ls", "-l"]:
            hits = [p for p in self.files if p == a[2] or p.startswith(a[2] + "/")]
            out = "".join(f"-rwxr-xr-x 1 root root {len(self.files[p])} {p}\n" for p in hits)
            return (0, out.encode(), b"") if hits else (1, b"", b"No such file")
        if a == ["mkdir", "-p", dh.REMOTE_DIR] or a[0] == "chmod":
            return 0, b"", b""
        if a[0] == "md5sum":
            data = self.files.get(a[1])
            if data is None:
                return 1, b"", b"No such file"
            if a[1] in self.corrupt:
                data += b"!"
            return 0, f"{hashlib.md5(data).hexdigest()}  {a[1]}\n".encode(), b""
        if a[0] == "mv":
            if a[1] not in self.files:
                return 1, b"", b"No such file"
            self.files[a[2]] = self.files.pop(a[1])
            return 0, b"", b""
        if a[:2] == ["sh", "-c"] and a[2].startswith("p=$(") and a[2].endswith("echo $h"):
            self.detached.append(a[2])
            return (0, b"4242\n", b"") if self.detach_alive else (1, b"", b"")
        if a == ["cat", dh.DETACH_LOG]:
            return 0, b"hub: cannot listen on port 8099\n", b""
        if a[:2] == ["rm", "-f"]:
            self.files.pop(a[2], None)
            return 0, b"", b""
        if a[:2] == ["rm", "-rf"]:
            self.files = {p: d for p, d in self.files.items() if not p.startswith(a[2] + "/")}
            return 0, b"", b""
        if a[:2] == ["test", "-x"]:
            return (0 if a[2] in self.files else 1), b"", b""
        if a[0] == dh.REMOTE_SIM:
            return self.sim_rc, b"sent key\n", b"" if self.sim_rc == 0 else b"connect failed"
        if a == ["cat", "/proc/uptime"]:
            return 0, f"{self.uptime_s:.2f} 0.30\n".encode(), b""
        if a == ["reboot"]:
            if self.reboot_fires:
                self.down = self.up_after
                self.uptime_s = 12.0  # a restarted hub reports a fresh uptime
            return 255, b"", b""
        raise AssertionError(f"unexpected command: {a}")

    def run_input(self, argv, data):
        self.log.append(argv)
        m = re.fullmatch(r"cat > (\S+)", argv[2]) if argv[:2] == ["sh", "-c"] else None
        assert m, f"unexpected stdin command: {argv}"
        if self.upload_rc == 0:
            self.files[shlex.split(m[1])[0]] = data
        return self.upload_rc, b"", b"" if self.upload_rc == 0 else b"No space left"

    def run_foreground(self, argv):
        self.log.append(argv)
        self.fg.append(argv)
        return 0


class Clock:
    def __init__(self) -> None:
        self.t = 0.0

    def __call__(self) -> float:
        return self.t

    def sleep(self, s: float) -> None:
        self.t += s


def make(hub: FakeHub | None = None, runner=None):
    hub = hub or FakeHub()
    clock, lines = Clock(), []
    d = dh.Deployer(
        runner or hub, out=lines.append, sleep=clock.sleep, clock=clock,
        now=lambda: datetime(2026, 11, 17, 12, 0, tzinfo=UTC),
    )  # fmt: skip
    return d, hub, lines


@pytest.fixture
def artifacts(tmp_path):
    out = {}
    for name, data in (("hub", b"H" * 4000), ("remote_sim", b"S" * 1000),
                       ("config.json", b'{"a": 1}'), ("busybox", b"B" * 2000)):  # fmt: skip
        f = tmp_path / name
        f.write_bytes(data)
        out[name] = f
    return out


def push_all(d, art, **kw):
    return d.push(
        art["hub"],
        remote_sim=art["remote_sim"],
        config=art["config.json"],
        busybox=art["busybox"],
        **kw,
    )


# --- parsers (real BusyBox shapes) ----------------------------------------------------------


def test_parse_ps_real_shape():
    procs = dh.parse_ps(PS_STOCK)
    by = {p.pid: p for p in procs}
    assert by[101].name == "hal" and by[101].alive
    assert by[102].name == "hal" and not by[102].alive  # zombie [hal]
    assert by[140].name == "lua" and by[141].name == "luaworks"
    assert by[2].name == "kthreadd"
    assert len(procs) == 8  # header skipped


def test_parse_fd_scan_and_no_readlink():
    assert dh.parse_fd_scan(FD_LINES) == [(101, "/dev/i2s"), (101, "/dev/rfspi")]
    assert dh.parse_fd_scan("") == []
    assert dh.parse_fd_scan("garbage\n/proc/x y -> /dev/i2s\n") == []
    for banned in ("readlink", "lsof", "fuser", "nohup", "setsid"):
        assert banned not in dh.FD_SCAN
    assert "ls -l" in dh.FD_SCAN and "/fd" in dh.FD_SCAN


def test_parse_df():
    assert dh.parse_df_kb(DF_OUT) == 1220
    assert dh.parse_df_kb(DF_WRAPPED) == 1220
    assert dh.parse_df_kb("df: not found") is None


# --- status ---------------------------------------------------------------------------------


def test_status_stock_up():
    d, _hub, lines = make()
    assert d.status() == 0
    text = "\n".join(lines)
    assert "hal: pid 101" in text and "luaworks: pid 141" in text
    assert "/dev/i2s: held by hal[101]" in text and "/dev/rfspi: held by hal[101]" in text
    assert "zombies: hal[102]" in text
    assert "1220 KiB" in text and "our files: none" in text and "stock stack: UP" in text


def test_status_no_df_and_ours_present():
    d, hub, lines = make(FakeHub(df=False))
    hub.files[dh.REMOTE_HUB] = b"x"
    d.status()
    text = "\n".join(lines)
    assert "unknown" in text and "our files: present" in text and dh.REMOTE_HUB in text


# --- push -----------------------------------------------------------------------------------


def test_push_happy_path(artifacts):
    d, hub, lines = make()
    assert push_all(d, artifacts) == 0
    assert hub.files[dh.REMOTE_HUB] == b"H" * 4000
    assert hub.files[dh.REMOTE_SIM] == b"S" * 1000
    assert hub.files[dh.REMOTE_CONFIG] == b'{"a": 1}'
    assert hub.files[dh.REMOTE_BUSYBOX] == b"B" * 2000
    assert any("push complete" in ln for ln in lines)
    assert ["chmod", "755", dh.REMOTE_HUB + ".new"] in hub.log
    assert ["mv", dh.REMOTE_HUB + ".new", dh.REMOTE_HUB] in hub.log


def test_push_rf_probe_is_md5_verified_and_executable(artifacts, tmp_path):
    """rf_probe rides the same verified path as the other binaries, not a hand scp."""
    probe = tmp_path / "rf_probe"
    probe.write_bytes(b"P" * 1500)
    d, hub, lines = make()
    assert d.push(artifacts["hub"], rf_probe=probe, busybox=artifacts["busybox"]) == 0
    assert hub.files[dh.REMOTE_RF_PROBE] == b"P" * 1500
    assert ["chmod", "755", dh.REMOTE_RF_PROBE] in hub.log
    assert any(dh.REMOTE_RF_PROBE in ln and "md5" in ln for ln in lines)


def test_push_rf_probe_md5_mismatch_fails(artifacts, tmp_path):
    probe = tmp_path / "rf_probe"
    probe.write_bytes(b"P" * 1500)
    d, hub, _ = make(FakeHub(corrupt={dh.REMOTE_RF_PROBE}))
    with pytest.raises(dh.DeployError):
        d.push(artifacts["hub"], rf_probe=probe, busybox=artifacts["busybox"])
    assert dh.REMOTE_RF_PROBE not in hub.files


def test_push_md5_mismatch_fails_and_removes(artifacts):
    d, hub, lines = make(FakeHub(corrupt={dh.REMOTE_SIM}))
    with pytest.raises(dh.DeployError, match="MISMATCH"):
        push_all(d, artifacts)
    assert dh.REMOTE_SIM not in hub.files  # unverified file not left behind
    assert not any("push complete" in ln for ln in lines)  # no partial success claimed


def test_push_upload_failure(artifacts):
    d, _hub, lines = make(FakeHub(upload_rc=1))
    with pytest.raises(dh.DeployError, match="upload failed"):
        d.push(artifacts["hub"], check_space=False)
    assert not any("push complete" in ln for ln in lines)


def test_push_insufficient_space_refuses_before_writing(artifacts):
    d, hub, _ = make(FakeHub(free_kb=300))
    with pytest.raises(dh.DeployError, match="insufficient"):
        push_all(d, artifacts)
    assert not any(k.startswith(dh.REMOTE_DIR) for k in hub.files)
    assert ["mkdir", "-p", dh.REMOTE_DIR] not in hub.log
    d2, _, _ = make(FakeHub(free_kb=600))
    assert push_all(d2, artifacts) == 0  # 600 KiB free still clears the 512 KiB margin


def test_push_margin_configurable(artifacts):
    d, _, _ = make(FakeHub(free_kb=1220))
    with pytest.raises(dh.DeployError, match="insufficient"):
        push_all(d, artifacts, margin_kb=1218)


def test_push_unknown_space_fails_closed(artifacts):
    d, hub, _ = make(FakeHub(df=False))
    with pytest.raises(dh.DeployError, match="cannot determine"):
        d.push(artifacts["hub"])  # no busybox staged -> no df
    assert not any(k.startswith(dh.REMOTE_DIR) for k in hub.files)
    assert d.push(artifacts["hub"], check_space=False) == 0
    d3, hub3, _ = make(FakeHub(df=False))  # a staged BusyBox supplies df
    assert d3.push(artifacts["hub"], busybox=artifacts["busybox"]) == 0
    assert dh.REMOTE_BUSYBOX in hub3.files


def test_push_missing_local_file_touches_nothing(tmp_path):
    d, hub, _ = make()
    with pytest.raises(dh.DeployError, match="not found"):
        d.push(tmp_path / "nope")
    assert hub.log == []


# --- quiesce --------------------------------------------------------------------------------


def test_quiesce_happy_banner_and_order():
    d, hub, lines = make()
    assert d.quiesce() == 0
    kills = [int(c[1]) for c in hub.log if c[0] == "kill"]
    assert kills.index(141) < kills.index(101) and kills.index(140) < kills.index(101)
    assert 88 not in kills  # dropbear untouched
    text = "\n".join(lines)
    assert "QUIESCED at 2026-11-17T12:00:00+00:00" in text and "DOWN" in text
    assert not [p for p in hub.procs if p[3]]  # nothing holds a device


def test_quiesce_refuses_when_device_still_held():
    hub = FakeHub()
    hub.procs.append([200, "S", "/data/ha-harmony/hub", ["/dev/i2s"]])  # not ours to kill
    d, _, lines = make(hub)
    with pytest.raises(dh.DeployError, match="still held"):
        d.quiesce()
    assert not any("QUIESCED" in ln for ln in lines)


def test_quiesce_refuses_when_hal_will_not_die():
    d, hub, lines = make(FakeHub(stuck={101}))
    with pytest.raises(dh.DeployError, match="still running"):
        d.quiesce(timeout=6)
    assert not any("QUIESCED" in ln for ln in lines)
    assert not any(c[0] == "kill" and c[1] == "-9" for c in hub.log)


# --- run ------------------------------------------------------------------------------------


def test_run_refuses_while_hal_alive():
    d, hub, _ = make()
    hub.files[dh.REMOTE_HUB] = b"x"
    with pytest.raises(dh.DeployError, match="hal is still alive"):
        d.run_hub(["--port", "8099"])
    assert hub.fg == []


def test_run_foreground_after_quiesce_and_missing_hub():
    d, hub, _ = make()
    d.quiesce()
    with pytest.raises(dh.DeployError, match="run push first"):
        d.run_hub([])
    hub.files[dh.REMOTE_HUB] = b"x"
    assert d.run_hub(["--token", "t; rm -rf /"]) == 0
    assert hub.fg == [["sh", "-c", dh.HOLDER, dh.REMOTE_HUB, "--token", "t; rm -rf /"]]


def test_run_help_allowed_while_hal_alive():
    d, hub, _ = make()
    hub.files[dh.REMOTE_HUB] = b"x"
    assert d.run_hub(["--help"]) == 0


def test_run_refuses_when_any_holder_remains():
    hub = FakeHub()
    hub.procs = [p for p in hub.procs if p[0] != 101]  # hal gone, but something holds i2s
    hub.procs.append([300, "S", "stale-thing", ["/dev/i2s"]])
    hub.files[dh.REMOTE_HUB] = b"x"
    d, _, _ = make(hub)
    with pytest.raises(dh.DeployError, match="still held"):
        d.run_hub([])
    assert hub.fg == []


# --- press / clean --------------------------------------------------------------------------


def test_press_and_failure():
    d, hub, _ = make()
    assert d.press(23) == 0
    assert [dh.REMOTE_SIM, "--rf", dh.RF_SOCKET, "--press", "23"] in hub.log
    d2, _, _ = make(FakeHub(sim_rc=1))
    with pytest.raises(dh.DeployError, match="remote_sim exited 1"):
        d2.press(23)
    with pytest.raises(dh.DeployError, match="outside"):
        d.press(1, sock="/etc/x.sock")


def test_clean(artifacts):
    d, hub, _ = make()
    push_all(d, artifacts)
    assert d.clean() == 0
    assert not any(p.startswith(dh.REMOTE_DIR) for p in hub.files)
    assert dh.REMOTE_BUSYBOX not in hub.files


# --- restore --------------------------------------------------------------------------------


def test_restore_reboots_waits_for_ssh_and_confirms_stock():
    hub = FakeHub(up_after=5)
    d, _, lines = make(hub)
    d.quiesce()
    assert d.restore() == 0
    assert ["reboot"] in hub.log
    assert hub.log.count(["uname", "-a"]) >= 6  # polled while ssh was unavailable
    text = "\n".join(lines)
    assert "RESTORED" in text and "outage" in text
    assert [p[0] for p in hub.procs if p[2].startswith("/usr/bin/hal")] == [101]


def test_restore_never_restarts_hal_by_hand():
    d, hub, _ = make()
    d.restore()
    assert not [c for c in hub.log if "hal" in " ".join(c) or "luaworks" in " ".join(c)]


def test_restore_waits_for_slow_stock_start():
    d, _hub, lines = make(FakeHub(stock_after_ps=6))
    d.quiesce()
    assert d.restore() == 0
    assert any("RESTORED" in ln for ln in lines)


def test_restore_ssh_never_returns():
    d, _hub, lines = make(FakeHub(up_after=10**6))
    with pytest.raises(dh.DeployError, match="did not return"):
        d.restore(up_timeout=60)
    assert not any("RESTORED" in ln for ln in lines)


def test_restore_cannot_confirm_stock():
    hub = FakeHub()
    d, _, lines = make(hub)
    d.quiesce()
    hub.stock_after_ps = 10**6  # boots, ssh returns, but hal never shows up
    with pytest.raises(dh.DeployError, match="CANNOT CONFIRM"):
        d.restore(settle_timeout=30)
    assert not any("RESTORED" in ln for ln in lines)


def test_restore_reboot_that_never_fires():
    """Stayed reachable AND uptime did not drop: that is a reboot that did not happen."""
    d, _hub, _ = make(FakeHub(reboot_fires=False))
    with pytest.raises(dh.DeployError, match="cannot confirm the reboot"):
        d.restore(down_timeout=20)


def test_restore_accepts_a_reboot_with_no_visible_offline_window():
    """The real hub came back before the first poll, which must not read as a failed reboot."""
    hub = FakeHub(reboot_fires=False)  # ssh never appears to drop...
    d, _h, lines = make(hub)
    original = hub.run

    def run(argv):  # ...but the uptime drops, which is positive proof it restarted
        if argv == ["reboot"]:
            hub.uptime_s = 9.0
        return original(argv)

    hub.run = run
    assert d.restore(down_timeout=5) == 0
    assert any("without a visible gap" in ln for ln in lines)


# --- cli ------------------------------------------------------------------------------------


def test_cli_write_steps_need_yes_and_help_marks_them(capsys):
    d, hub, _ = make()
    for cmd in (
        ["quiesce"],
        ["restore"],
        ["clean"],
        ["run"],
        ["press", "1"],
        ["push", "--hub", "x"],
    ):
        assert dh.main(cmd, deployer=d) == 2
    assert hub.log == []
    with pytest.raises(SystemExit):
        dh.main(["--help"])
    out = capsys.readouterr().out
    assert "[read-only] stock processes" in out
    assert out.count("[WRITE]") >= 6


def test_cli_status_and_error_exit(capsys, artifacts):
    d, _hub, _ = make(FakeHub(corrupt={dh.REMOTE_HUB + ".new"}))
    assert dh.main(["status"], deployer=d) == 0
    cmd = ["push", "--yes", "--hub", str(artifacts["hub"]), "--no-space-check"]
    assert dh.main(cmd, deployer=d) == 1
    assert "MISMATCH" in capsys.readouterr().err


def test_cli_run_passes_args_after_double_dash():
    d, hub, _ = make()
    d.quiesce()
    hub.files[dh.REMOTE_HUB] = b"x"
    assert dh.main(["run", "--yes", "--", "--port", "8099"], deployer=d) == 0
    assert hub.fg == [["sh", "-c", dh.HOLDER, dh.REMOTE_HUB, "--port", "8099"]]


# --- safety: allow-list over the recorded ssh command log -----------------------------------

DENY = re.compile(
    r"nohup|setsid|\bdd\b|nandwrite|flash_erase|mtd_debug|flashcp|ubiformat|ubi\w+|/dev/mtd|"
    r"/etc/|crontab|tdeenable|/mnt/data|\bmv\b|\bsed -i"
)
NUM = r"\d+"


def violations(remote: str) -> list[str]:
    """Allow-list check of one remote command line; empty list means permitted."""
    try:
        a0 = shlex.split(remote)
    except ValueError:
        a0 = []
    if len(a0) == 3 and a0[0] == "mv":  # only an atomic replace inside the write root
        ok = a0[1] == a0[2] + ".new" and dh.under_write_root(a0[2])
        return [] if ok and a0[2] == dh.REMOTE_HUB else [f"not allowed: {remote!r}"]
    if re.fullmatch(
        re.escape(f"p=$(sh -c {shlex.quote(dh.HOLDER)} {dh.REMOTE_HUB}")
        + r"[^;&|`$]* </dev/null >"
        + re.escape(dh.DETACH_LOG)
        + r" 2>&1 & echo \$!\); sleep \d+; "
        + re.escape(f"read h <{dh.DETACH_PID} && kill -0 $h && echo $h"),
        a0[2] if a0[:2] == ["sh", "-c"] and len(a0) == 3 else "",
    ):
        return []
    if DENY.search(remote):
        return [f"denied token: {remote!r}"]
    if "&" in re.sub(r"&&|>&\d|2>&1", "", remote):
        return [f"backgrounding: {remote!r}"]
    try:
        a = shlex.split(remote)
    except ValueError:
        return [f"unparseable: {remote!r}"]
    bad = [f"not allowed: {remote!r}"]
    if not a:
        return bad
    prog, rest = a[0], a[1:]
    if a in (["uname", "-a"], ["ps"], ["reboot"], ["cat", "/proc/uptime"]):
        return []
    if prog == "ls" and rest[:1] == ["-l"] and len(rest) == 2:
        return [] if rest[1] in (dh.REMOTE_DIR, dh.REMOTE_BUSYBOX) else bad
    if a in (["mkdir", "-p", dh.REMOTE_DIR], ["rm", "-rf", dh.REMOTE_DIR]):
        return []
    if prog == "chmod" and len(rest) == 2 and rest[0] == "755":
        return [] if dh.under_write_root(rest[1]) else bad
    if a == ["cat", dh.DETACH_LOG]:
        return []
    if a[:4] == ["sh", "-c", dh.HOLDER, dh.REMOTE_HUB]:  # foreground run under the /dev/i2s holder
        return []
    if prog == "rm" and len(rest) == 2 and rest[0] == "-f":
        ok = dh.under_write_root(rest[1]) and rest[1] not in dh.WRITE_ROOTS
        return [] if ok else bad
    if prog == "md5sum" and len(rest) == 1:
        return [] if dh.under_write_root(rest[0]) else bad
    if a in (["df", "-k", "/data"], [dh.REMOTE_BUSYBOX, "df", "-k", "/data"]):
        return []
    if prog == "kill" and len(rest) == 1 and re.fullmatch(NUM, rest[0]):
        return []
    if a == ["test", "-x", dh.REMOTE_HUB] or prog == dh.REMOTE_HUB:
        return []  # the daemon itself; args are shell-quoted so cannot inject
    if prog == dh.REMOTE_SIM:
        ok = len(rest) == 4 and rest[0] == "--rf" and dh.under_write_root(rest[1])
        return [] if ok and rest[2] == "--press" and re.fullmatch(NUM, rest[3]) else bad
    if prog == "sh" and rest[:1] == ["-c"] and len(rest) == 2:
        if rest[1] == dh.FD_SCAN:
            return []
        m = re.fullmatch(r"cat > (.+)", rest[1])
        if m and dh.under_write_root(shlex.split(m[1])[0]):
            return []
    return bad


def sh(script: str) -> str:
    return shlex.join(["sh", "-c", script])


# The hub's stock BusyBox lacks these outright, or lacks the flag. `grep -E` in particular once
# made the fd scan fail on every process and report "not held" while `hal` held both devices --
# a false negative that would let `quiesce` pass and `run` open /dev/i2s against a live `hal`,
# which wedges unkillably in ath_i2s_close. Anything on-device must stay inside this floor.
MISSING_APPLETS = (
    r"grep\s+-[A-DF-Za-z]*E",  # no extended regex
    r"\bhead\b",
    r"\bsort\b",
    r"\buniq\b",
    r"\breadlink\b",
    r"\bbasename\b",
    r"\bxargs\b",
    r"\bseq\b",
    r"\bsync\b",
    r"\btar\b",
    r"\bsha256sum\b",
    r"\bfind\b",
    r"\bcommand -v\b",
    r"\blsof\b",
    r"\bfuser\b",
)


def missing_applets(remote: str) -> list[str]:
    """Report any use of an applet or flag the hub's stock BusyBox does not have.

    A bare `df` is allowed: the tool probes it and falls back to the staged BusyBox when it is
    absent, and an absent-applet probe is harmless. What is *not* allowed is depending on an
    applet's output without a fallback -- which is what `grep -E` did.
    """
    return [p for p in MISSING_APPLETS if re.search(p, remote)]


def test_fd_scan_avoids_applets_the_hub_lacks() -> None:
    assert missing_applets(dh.FD_SCAN) == []
    assert "grep -e" in dh.FD_SCAN  # basic grep with repeated -e, not -E


def test_missing_applet_checker_catches_regressions() -> None:
    assert missing_applets('grep -E " -> /dev/(i2s|rfspi)$"')
    assert missing_applets("ls -l /proc/1/fd | head -5")
    assert missing_applets("readlink /proc/1/fd/3")
    assert missing_applets("command -v dd")
    assert missing_applets("ls /data | sort")
    assert missing_applets("ps") == []
    assert missing_applets("df -k /data") == []  # probed with a fallback; see the docstring


KNOWN_BAD = (
    "dd if=/data/x of=/dev/mtd3", "nandwrite -p /dev/mtd3 x", "flash_erase /dev/mtd1",
    "mtd_debug write /dev/mtd1 0 4 x", "flashcp x /dev/mtd0", "ubiformat /dev/mtd1",
    "tee /dev/mtd0", sh("cat x >/dev/mtd2"), sh("cat x > /dev/mtdblock2"),
    sh("cat x > /etc/init.d/S99ha"), sh("echo x > /etc/inittab"), sh("touch /etc/tdeenable"),
    sh("cat x > /etc/rc.local"), "crontab -l", sh("echo '* * * * * x' > /var/spool/cron/root"),
    "nohup /data/ha-harmony/hub", "setsid /data/ha-harmony/hub", sh("/data/ha-harmony/hub &"),
    "/data/ha-harmony/hub &", sh("cat x > /data/other/file"), sh("cat x > /mnt/data/x"),
    "rm -rf /data", "rm -rf /", "rm -f /etc/passwd", "mkdir -p /etc/x", "chmod 755 /etc/x",
    "mv a /data/ha-harmony/b", "sed -i s/a/b/ /data/ha-harmony/config.json",
    sh("cat x > /data/ha-harmony/../../etc/x"), "kill -9 101", "cp a /dev/i2s",
)  # fmt: skip


def test_checker_rejects_known_bad_and_accepts_good():
    for bad in KNOWN_BAD:
        assert violations(bad), bad
    for good in (
        "uname -a", "ps", "reboot", "kill 101", "md5sum /data/ha-harmony/hub",
        "chmod 755 /data/ha-harmony/hub", "rm -f /tmp/ha-harmony-busybox",
        "rm -rf /data/ha-harmony", sh("cat > /data/ha-harmony/hub"),
        sh("cat > /tmp/ha-harmony-busybox"), sh(dh.FD_SCAN), "df -k /data",
        "/data/ha-harmony/hub --port 8099",
        "/data/ha-harmony/remote_sim --rf /tmp/ha-harmony-rf.sock --press 23",
    ):  # fmt: skip
        assert not violations(good), good


class Recording(dh.HubSsh):
    """Real HubSsh argv construction (quoting, -tt); records the exact ssh argv."""

    def __init__(self, fake: FakeHub, log: list[list[str]]) -> None:
        super().__init__("h")
        self.fake, self.log = fake, log

    def run(self, argv):
        self.log.append(self._ssh(argv))
        return self.fake.run(argv)

    def run_input(self, argv, data):
        self.log.append(self._ssh(argv))
        return self.fake.run_input(argv, data)

    def run_foreground(self, argv):
        cmd = self._ssh(argv)
        cmd.insert(1, "-tt")  # same as HubSsh.run_foreground
        self.log.append(cmd)
        return self.fake.run_foreground(argv)


def test_checker_fails_when_a_persistence_command_is_added(monkeypatch):
    """A regression that arms the daemon at boot must trip the log check."""
    hub, log = FakeHub(), []
    orig = dh.Deployer.clean

    def evil_clean(self):
        self.r.run_input(["sh", "-c", "cat > /etc/init.d/S99ha-harmony"], b"#!/bin/sh\n")
        return orig(self)

    monkeypatch.setattr(dh.Deployer, "clean", evil_clean)
    d, _, _ = make(hub, runner=Recording(hub, log))
    d.clean()
    flagged = [c[-1] for c in log if violations(c[-1])]
    assert flagged == [sh("cat > /etc/init.d/S99ha-harmony")]
    # and an injected background launch is caught the same way
    assert violations(sh("/data/ha-harmony/hub --port 8099 &"))


def test_every_emitted_command_is_allow_listed(artifacts):
    log: list[list[str]] = []

    def go(hub: FakeHub, fn) -> None:
        d, _, _ = make(hub, runner=Recording(hub, log))
        try:
            fn(d)
        except dh.DeployError:
            pass  # failure paths are exercised for their commands, not their outcome

    h = FakeHub()
    go(h, lambda d: d.status())
    go(h, lambda d: push_all(d, artifacts))
    go(h, lambda d: d.run_hub(["--port", "8099"]))  # refused: hal alive
    go(h, lambda d: d.quiesce())
    go(h, lambda d: d.run_hub(["--port", "8099", "--token", "a b; rm -rf /"]))
    go(h, lambda d: d.press(23))
    go(h, lambda d: d.prune())
    go(h, lambda d: d.run_detached(["--port", "8099"]))  # refused: hal alive
    h2 = FakeHub()
    go(
        h2,
        lambda d: (
            d.quiesce(),
            h2.files.update({dh.REMOTE_HUB: b"x"}),
            d.run_detached(["--token", "a b"]),
        ),
    )
    go(FakeHub(corrupt={dh.REMOTE_HUB + ".new"}), lambda d: push_all(d, artifacts))
    go(h, lambda d: d.restore())
    go(h, lambda d: d.clean())
    go(FakeHub(corrupt={dh.REMOTE_HUB + ".new"}), lambda d: push_all(d, artifacts))
    go(FakeHub(free_kb=10), lambda d: push_all(d, artifacts))
    go(FakeHub(upload_rc=1), lambda d: push_all(d, artifacts))
    go(FakeHub(stuck={101}), lambda d: d.quiesce(timeout=4))
    go(FakeHub(up_after=10**6), lambda d: d.restore(up_timeout=20))
    assert len(log) > 80
    assert any(c[1] == "-tt" for c in log)  # the foreground run gets a tty
    bad = [(c[-1], v) for c in log if (v := violations(c[-1]))]
    assert not bad, bad
    # Every command must also stay within the stock BusyBox's applet floor; validated against the
    # real hub, where `grep -E` silently broke device-holder detection.
    unsupported = [(c[-1], m) for c in log if (m := missing_applets(c[-1]))]
    assert not unsupported, unsupported
    assert all(c[0] == "ssh" and "BatchMode=yes" in c for c in log)


def test_internal_write_guard_rejects_outside_paths():
    for p in (
        "/etc/init.d/x",
        "/data/other",
        "/dev/mtd0",
        "/tmp/../etc/x",
        "/data/ha-harmony/../x",
    ):
        with pytest.raises(dh.DeployError):
            dh._need_write_path(p)
    assert dh._need_write_path("/data/ha-harmony/hub")


# --- atomic push, prune, run --detach ----------------------------------------------------------


def test_push_hub_uploads_new_verifies_then_moves(artifacts):
    d, hub, _ = make()
    hub.files[dh.REMOTE_HUB] = b"OLD"
    assert d.push(artifacts["hub"], check_space=False) == 0
    new = dh.REMOTE_HUB + ".new"
    seq = [c for c in hub.log if any(new in x for x in c)]
    upload = next(i for i, c in enumerate(seq) if c[:2] == ["sh", "-c"])
    md5 = next(i for i, c in enumerate(seq) if c == ["md5sum", new])
    chmod = next(i for i, c in enumerate(seq) if c == ["chmod", "755", new])
    mv = next(i for i, c in enumerate(seq) if c == ["mv", new, dh.REMOTE_HUB])
    assert upload < md5 < chmod < mv
    assert hub.files[dh.REMOTE_HUB] == b"H" * 4000 and new not in hub.files


def test_push_md5_failure_leaves_old_hub_untouched(artifacts):
    d, hub, _ = make(FakeHub(corrupt={dh.REMOTE_HUB + ".new"}))
    hub.files[dh.REMOTE_HUB] = b"OLD"
    with pytest.raises(dh.DeployError, match="MISMATCH"):
        d.push(artifacts["hub"], check_space=False)
    assert hub.files[dh.REMOTE_HUB] == b"OLD"
    assert dh.REMOTE_HUB + ".new" not in hub.files
    assert not any(c[0] == "mv" for c in hub.log)


def test_push_upload_failure_leaves_old_hub_untouched(artifacts):
    d, hub, _ = make(FakeHub(upload_rc=1))
    hub.files[dh.REMOTE_HUB] = b"OLD"
    with pytest.raises(dh.DeployError):
        d.push(artifacts["hub"], check_space=False)
    assert hub.files[dh.REMOTE_HUB] == b"OLD"


def test_push_space_check_counts_full_new_size_while_old_exists(artifacts):
    """hub.new coexists with hub, so the whole new binary must fit in current free space."""
    d, hub, _ = make(FakeHub(free_kb=4))  # 4 KiB free < 4000 B + 512 KiB margin
    hub.files[dh.REMOTE_HUB] = b"OLD"
    with pytest.raises(dh.DeployError, match="insufficient"):
        d.push(artifacts["hub"], busybox=artifacts["busybox"])
    assert hub.files[dh.REMOTE_HUB] == b"OLD"
    assert dh.REMOTE_HUB + ".new" not in hub.files


def test_prune_deletes_only_the_two_bench_binaries():
    d, hub, _ = make()
    for f in (dh.REMOTE_SIM, dh.REMOTE_RF_PROBE, dh.REMOTE_HUB, dh.REMOTE_CONFIG,
              dh.REMOTE_DIR + "/state.json", dh.REMOTE_DIR + "/backup/x"):  # fmt: skip
        hub.files[f] = b"x"
    assert dh.main(["prune", "--yes"], deployer=d) == 0
    assert set(hub.files) == {dh.REMOTE_HUB, dh.REMOTE_CONFIG, dh.REMOTE_DIR + "/state.json",
                              dh.REMOTE_DIR + "/backup/x"}  # fmt: skip
    rms = [c for c in hub.log if c[0] == "rm"]
    assert rms == [["rm", "-f", dh.REMOTE_SIM], ["rm", "-f", dh.REMOTE_RF_PROBE]]


def test_prune_needs_yes():
    d, hub, _ = make()
    assert dh.main(["prune"], deployer=d) == 2
    assert hub.log == []


def test_run_detach_command_shape():
    d, hub, lines = make()
    d.quiesce()
    hub.files[dh.REMOTE_HUB] = b"x"
    rc = dh.main(["run", "--yes", "--detach", "--", "--port", "8099", "--rf-spi"], deployer=d)
    assert rc == 0
    assert hub.fg == [], "detach must not use the TTY foreground path"
    assert hub.detached == [
        (
            f"p=$(sh -c {shlex.quote(dh.HOLDER)} {dh.REMOTE_HUB} --port 8099 --rf-spi </dev/null "
            f">{dh.DETACH_LOG} 2>&1 & echo $!); sleep 3; read h <{dh.DETACH_PID} && kill -0 $h && echo $h"
        )
    ]
    assert "pidof" not in hub.detached[0], "must report the pid we started, not any hub"
    assert any("pid 4242" in ln for ln in lines)


def test_run_detach_refused_while_hal_alive_and_reports_dead_daemon():
    d, hub, _ = make()
    hub.files[dh.REMOTE_HUB] = b"x"
    with pytest.raises(dh.DeployError, match="hal"):
        d.run_detached([])
    assert hub.detached == []
    d.quiesce()
    hub.detach_alive = False
    with pytest.raises(dh.DeployError, match="cannot listen"):
        d.run_detached(["--port", "8099"])


def test_run_detach_quotes_hostile_args():
    d, hub, _ = make()
    d.quiesce()
    hub.files[dh.REMOTE_HUB] = b"x"
    d.run_detached(["--token", "a b; rm -rf /"])
    assert "'a b; rm -rf /'" in hub.detached[0]


@pytest.mark.parametrize("shell", [["/bin/sh"], ["busybox", "sh"]])
def test_holder_records_the_hubs_own_pid_not_the_holders(tmp_path, shell):
    import shutil
    import subprocess
    import time

    if shell[0] != "/bin/sh" and not shutil.which(shell[0]):
        pytest.skip("no busybox")
    pidf, out = tmp_path / "pid", tmp_path / "hubpid"
    hub = tmp_path / "hub"
    hub.write_text(f"#!/bin/sh\necho $$ >{out}\nsleep 30\n")
    hub.chmod(0o755)
    script = dh.HOLDER.replace(dh.DETACH_PID, str(pidf)).replace("/dev/i2s", str(tmp_path / "i2s"))
    (tmp_path / "i2s").write_text("")
    p = subprocess.Popen([*shell, "-c", script, str(hub)], start_new_session=True)
    try:
        for _ in range(100):
            if (
                pidf.exists()
                and pidf.read_text().strip()
                and out.exists()
                and out.read_text().strip()
            ):
                break
            time.sleep(0.05)
        assert pidf.read_text().strip() == out.read_text().strip()
        assert pidf.read_text().strip() != str(p.pid), "must not be the parked holder's pid"
    finally:
        import os
        import signal

        os.killpg(p.pid, signal.SIGKILL)


# --- restart --------------------------------------------------------------------------------


def test_restart_writes_marker_then_sigterms_the_hook_supervised_daemon():
    d, _hub, lines = make(_RestartHub())
    assert d.restart() == 0
    script = next(c[2] for c in d.r.log if c[:2] == ["sh", "-c"] and "planned-restart" in c[2])
    assert script.index("planned-restart") < script.index('kill "$p"')
    assert "kill -9" not in script and "killall" not in script
    assert dh.PLANNED_MARKER == "/tmp/ha-harmony-planned-restart"
    assert any("pid 321" in line for line in lines)


def test_restart_without_a_live_daemon_fails_cleanly():
    d, _hub, _lines = make(_RestartHub(rc=3))
    with pytest.raises(dh.DeployError, match="no live daemon"):
        d.restart()


def test_restart_needs_yes():
    assert dh.main(["restart"]) == 2


class _RestartHub(FakeHub):
    def __init__(self, rc=0):
        super().__init__()
        self.rc = rc

    def run(self, argv):
        if argv[:2] == ["sh", "-c"] and "planned-restart" in argv[2]:
            self.log.append(argv)
            return (0, b"321\n", b"") if self.rc == 0 else (self.rc, b"", b"")
        return super().run(argv)


def test_restart_aborts_before_kill_if_marker_write_fails_and_hints_backoff():
    assert "|| exit 5; kill" in dh.RESTART_SCRIPT
    d, _hub, _lines = make(_RestartHub(rc=3))
    with pytest.raises(dh.DeployError, match="backoff"):
        d.restart()
