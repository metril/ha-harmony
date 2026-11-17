"""deploy_remote against a fake, stateful remote behind a fake SSH transport (no network)."""

import hashlib
import os
import re
import shlex
import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import deploy_hub as dh
import deploy_remote as dr

RCS = (
    "#!/bin/sh\n"
    "killall -9 usbhid 2>/dev/null\n"
    "/usr/bin/usbeth start &\n"
    "/data/dropbearmulti dropbear -R -p 0.0.0.0:22\n"
    "exit 0\n"
)
PS = (
    "  PID USER       VSZ STAT COMMAND\n"
    "    1 root      1568 S    init\n"
    "  210 root      9000 S    /data/app/harmony_remote\n"
)
BASE_FILES = {
    dr.RCS_LOCAL: RCS.encode(),
    dr.POWERSOURCE: b"battery\n",
    dr.SOC: b"87\n",
}


class FakeRemote:
    """Interprets exactly the commands deploy_remote may emit; anything else fails the test."""

    def __init__(self, rcs=RCS, corrupt=(), upload_rc=0, up_after=3, reboot_fires=True):
        self.files: dict[str, bytes] = dict(BASE_FILES)
        self.files[dr.RCS_LOCAL] = rcs.encode()
        self.corrupt, self.upload_rc = set(corrupt), upload_rc
        self.up_after, self.reboot_fires = up_after, reboot_fires
        self.modes: dict[str, str] = {}
        self.down = 0
        self.log: list[list[str]] = []
        self.fg: list[list[str]] = []
        self.written: dict[str, bytes] = {}  # every payload ever uploaded, by path

    def run(self, argv):
        self.log.append(argv)
        a = argv
        if self.down > 0:
            if a == ["uname", "-a"]:
                self.down -= 1
            return 255, b"", b"ssh: connect refused"
        if a == ["uname", "-a"]:
            return 0, b"Linux harmony 2.6.37 armv5tejl GNU/Linux\n", b""
        if a == ["ps"]:
            return 0, PS.encode(), b""
        if a[0] == "cat":
            return (0, self.files[a[1]], b"") if a[1] in self.files else (1, b"", b"No such file")
        if a[:2] == ["test", "-e"]:
            return (0 if a[2] in self.files else 1), b"", b""
        if a[:2] == ["test", "-x"]:
            return (0 if a[2] in self.files and self.modes.get(a[2]) == "755" else 1), b"", b""
        if a[:2] == ["ls", "-l"]:
            hits = [p for p in self.files if p.startswith(a[2] + "/")]
            out = "".join(f"-rwxr-xr-x 1 root root {len(self.files[p])} {p}\n" for p in hits)
            return (0, out.encode(), b"") if hits else (1, b"", b"No such file")
        if a == ["mkdir", "-p", dr.APP_DIR]:
            return 0, b"", b""
        if a[0] == "md5sum":
            data = self.files.get(a[1])
            if data is None:
                return 1, b"", b"No such file"
            if a[1] in self.corrupt:
                data += b"!"
            return 0, f"{hashlib.md5(data).hexdigest()}  {a[1]}\n".encode(), b""
        if a[0] == "chmod":
            self.modes[a[2]] = a[1]
            return 0, b"", b""
        if a[0] == "mv":
            self.files[a[2]] = self.files.pop(a[1])
            self.modes[a[2]] = self.modes.pop(a[1], "644")
            return 0, b"", b""
        if a[:2] == ["rm", "-f"]:
            self.files.pop(a[2], None)
            return 0, b"", b""
        if a[:2] == ["rm", "-rf"]:
            pre = a[2] + "/"
            self.files = {p: d for p, d in self.files.items() if not p.startswith(pre)}
            return 0, b"", b""
        if a == ["reboot"]:
            if self.reboot_fires:
                self.down = self.up_after
            return 255, b"", b""
        raise AssertionError(f"unexpected command: {a}")

    def run_input(self, argv, data):
        self.log.append(argv)
        m = re.fullmatch(r"cat > (\S+)", argv[2]) if argv[:2] == ["sh", "-c"] else None
        assert m, f"unexpected stdin command: {argv}"
        path = shlex.split(m[1])[0]
        if self.upload_rc == 0:
            self.files[path] = data
            self.written[path] = data
        return self.upload_rc, b"", b"" if self.upload_rc == 0 else b"No space left"

    def run_foreground(self, argv):
        self.log.append(argv)
        self.fg.append(argv)
        return 0


class Clock:
    t = 0.0

    def __call__(self):
        return self.t

    def sleep(self, s):
        self.t += s


def make(dev=None, runner=None):
    dev = dev or FakeRemote()
    clock, lines = Clock(), []
    d = dr.RemoteDeployer(runner or dev, out=lines.append, sleep=clock.sleep, clock=clock)
    return d, dev, lines


def hooked(dev) -> str:
    return dev.files[dr.RCS_LOCAL].decode()


@pytest.fixture
def art(tmp_path):
    out = []
    for name, data in (("harmony_remote", b"A" * 3000), ("rf_probe", b"R" * 500)):
        f = tmp_path / name
        f.write_bytes(data)
        out.append(f)
    return out


# --- helpers --------------------------------------------------------------------------------


def test_insert_hook_between_dropbear_and_exit():
    out = dr.insert_hook(RCS)
    lines = out.splitlines()
    assert lines[:4] == RCS.splitlines()[:4]
    assert lines[4] == dr.hook_line() and lines[5] == "exit 0"
    assert out.count(dr.HOOK_MARKER) == 1 and out.endswith("exit 0\n")


@pytest.mark.parametrize(
    "bad",
    [
        "",
        "#!/bin/sh\nexit 0\n",  # no dropbear
        "#!/bin/sh\n/data/dropbearmulti dropbear\n",  # no trailing exit 0
        "#!/bin/sh\nexit 0\n/data/dropbearmulti dropbear\n",  # dropbear after exit
        "#!/bin/sh\n# dropbear\nexit 0\n",  # commented out
        "#!/bin/sh\n/data/dropbearmulti dropbear\nexit 0\necho trailing\n",
    ],
)
def test_insert_hook_refuses_unknown_shapes(bad):
    with pytest.raises(dh.DeployError, match="unknown shape"):
        dr.insert_hook(bad)


# --- status ---------------------------------------------------------------------------------


def test_status_reports_everything():
    d, dev, lines = make()
    dev.files[dr.BOOTCOUNT] = b"2\n"
    assert d.status() == 0
    text = "\n".join(lines)
    assert "2.6.37" in text and "pid 210" in text
    assert "boot hook: not installed" in text and "DISABLE: absent" in text
    assert "boot counter: 2" in text
    assert "powersource: battery" in text and "battery soc: 87" in text


def test_status_after_install_shows_hook_and_disable():
    d, dev, lines = make()
    d.install_hook()
    dev.files[dr.DISABLE] = b""
    lines.clear()
    d.status()
    text = "\n".join(lines)
    assert "boot hook: installed" in text and "DISABLE: PRESENT" in text
    assert "boot counter: absent" in text


def test_status_is_read_only():
    d, dev, _ = make()
    d.status()
    assert all(c[0] in ("uname", "ps", "cat", "test", "ls") for c in dev.log)
    assert dev.written == {}


# --- push -----------------------------------------------------------------------------------


def test_push_happy_path_atomic_and_chmod(art):
    d, dev, lines = make()
    assert d.push(art) == 0
    assert dev.files[f"{dr.APP_DIR}/harmony_remote"] == b"A" * 3000
    assert dev.files[f"{dr.APP_DIR}/rf_probe"] == b"R" * 500
    assert dev.modes[f"{dr.APP_DIR}/harmony_remote"] == "755"
    assert not [p for p in dev.files if p.endswith(".new")]  # temps all renamed
    assert any("push complete: 2" in ln for ln in lines)
    # staged to a temp, verified, then moved
    i = dev.log.index(["md5sum", f"{dr.APP_DIR}/rf_probe.new"])
    assert dev.log[i + 2] == ["mv", f"{dr.APP_DIR}/rf_probe.new", f"{dr.APP_DIR}/rf_probe"]


def test_push_md5_mismatch_leaves_nothing(art):
    d, dev, lines = make(FakeRemote(corrupt={f"{dr.APP_DIR}/harmony_remote.new"}))
    with pytest.raises(dh.DeployError, match="MISMATCH"):
        d.push(art)
    assert not [p for p in dev.files if p.startswith(dr.APP_DIR)]
    assert not any("push complete" in ln for ln in lines)


def test_push_upload_failure(art):
    d, _, lines = make(FakeRemote(upload_rc=1))
    with pytest.raises(dh.DeployError, match="upload failed"):
        d.push(art)
    assert not any("push complete" in ln for ln in lines)


def test_push_missing_local_touches_nothing(tmp_path):
    d, dev, _ = make()
    with pytest.raises(dh.DeployError, match="not found"):
        d.push([tmp_path / "nope"])
    assert dev.log == []


# --- run ------------------------------------------------------------------------------------


def test_run_foreground_and_missing_app(art):
    d, dev, _ = make()
    with pytest.raises(dh.DeployError, match="run push first"):
        d.run_app("harmony_remote", [])
    d.push(art)
    assert d.run_app("harmony_remote", ["--page", "a b"]) == 0
    assert dev.fg == [[f"{dr.APP_DIR}/harmony_remote", "--page", "a b"]]


# --- install-hook ---------------------------------------------------------------------------


def test_install_hook_happy_path_order_and_atomicity():
    d, dev, _ = make()
    assert d.install_hook() == 0
    assert dev.files[dr.RCS_BACKUP] == RCS.encode()
    assert dev.files[dr.START_SH] == dr.start_script().encode()
    text = hooked(dev)
    assert text == dr.insert_hook(RCS) and text.count(dr.HOOK_MARKER) == 1
    assert dev.modes[dr.START_SH] == "755" and dev.modes[dr.RCS_LOCAL] == "755"
    # rcS.local is never written directly: temp, verify, mv
    uploads = [c[2] for c in dev.log if c[:2] == ["sh", "-c"]]
    assert f"cat > {dr.RCS_TMP}" in uploads and f"cat > {dr.RCS_LOCAL}" not in uploads
    i = dev.log.index(["md5sum", dr.RCS_TMP])
    assert dev.log[i + 2] == ["mv", dr.RCS_TMP, dr.RCS_LOCAL]
    assert dr.RCS_TMP not in dev.files


def test_install_hook_is_idempotent():
    d, dev, lines = make()
    d.install_hook()
    once = hooked(dev)
    lines.clear()
    assert d.install_hook() == 0
    assert hooked(dev) == once and once.count(dr.HOOK_MARKER) == 1
    assert dev.files[dr.RCS_BACKUP] == RCS.encode()
    assert any("already installed" in ln for ln in lines)


def test_install_hook_refuses_unexpected_rcs_and_prints_it():
    odd = "#!/bin/sh\necho custom\nexit 0\n"
    d, dev, _ = make(FakeRemote(rcs=odd))
    with pytest.raises(dh.DeployError, match="unknown shape") as e:
        d.install_hook()
    assert "echo custom" in str(e.value)
    assert dev.written == {} and dev.files[dr.RCS_LOCAL] == odd.encode()


def test_install_hook_refuses_duplicate_hook_lines():
    twice = dr.insert_hook(RCS).replace("exit 0\n", dr.hook_line() + "\nexit 0\n")
    d, dev, _ = make(FakeRemote(rcs=twice))
    with pytest.raises(dh.DeployError, match="2 hook lines"):
        d.install_hook()
    assert dev.written == {}


def test_backup_never_overwritten():
    d, dev, lines = make()
    dev.files[dr.RCS_BACKUP] = b"PRECIOUS ORIGINAL\n"
    d.install_hook()
    assert dev.files[dr.RCS_BACKUP] == b"PRECIOUS ORIGINAL\n"
    assert dr.RCS_BACKUP not in dev.written and dr.RCS_BACKUP + ".new" not in dev.written
    assert any("already exists" in ln for ln in lines)


def test_hook_present_without_backup_refuses():
    d, dev, _ = make(FakeRemote(rcs=dr.insert_hook(RCS)))
    with pytest.raises(dh.DeployError, match="backup|rcS.local.orig"):
        d.install_hook()
    assert dev.written == {}


def test_install_hook_md5_mismatch_on_rcs_keeps_original():
    d, dev, _ = make(FakeRemote(corrupt={dr.RCS_TMP}))
    with pytest.raises(dh.DeployError, match="MISMATCH"):
        d.install_hook()
    assert dev.files[dr.RCS_LOCAL] == RCS.encode()  # untouched
    assert dr.RCS_TMP not in dev.files  # temp cleaned up


# --- mark-good / uninstall ------------------------------------------------------------------


def test_mark_good_resets_counter():
    d, dev, _ = make()
    dev.files[dr.BOOTCOUNT] = b"3\n"
    assert d.mark_good() == 0
    assert dev.files[dr.BOOTCOUNT] == b"0\n"


def test_uninstall_restores_and_keeps_files():
    d, dev, lines = make()
    d.install_hook()
    assert d.uninstall() == 0
    assert dev.files[dr.RCS_LOCAL] == RCS.encode()
    assert dr.START_SH in dev.files and dr.RCS_BACKUP in dev.files
    assert dr.RCS_TMP not in dev.files
    assert any("left /data/app" in ln for ln in lines)
    i = dev.log.index(["md5sum", dr.RCS_TMP])
    assert dev.log[i + 2] == ["mv", dr.RCS_TMP, dr.RCS_LOCAL]
    assert dev.log[-1] == ["md5sum", dr.RCS_LOCAL]  # verified after the move


def test_uninstall_purge_removes_app_dir():
    d, dev, _ = make()
    d.install_hook()
    d.uninstall(purge=True)
    assert not [p for p in dev.files if p.startswith(dr.APP_DIR)]
    assert dev.files[dr.RCS_LOCAL] == RCS.encode()


def test_uninstall_without_backup_refuses():
    d, dev, _ = make()
    with pytest.raises(dh.DeployError, match="missing"):
        d.uninstall()
    assert dev.written == {}


def test_uninstall_md5_mismatch_keeps_hooked_file():
    d, dev, _ = make()
    d.install_hook()
    dev.corrupt.add(dr.RCS_TMP)
    with pytest.raises(dh.DeployError, match="MISMATCH"):
        d.uninstall()
    assert dr.HOOK_MARKER in hooked(dev)


# --- reboot ---------------------------------------------------------------------------------


def test_reboot_waits_for_ssh_and_reports():
    d, dev, lines = make()
    assert d.reboot() == 0
    assert ["reboot"] in dev.log
    text = "\n".join(lines)
    assert "ssh is back" in text and "boot counter" in text


def test_reboot_that_never_fires_or_never_returns():
    d, _, _ = make(FakeRemote(reboot_fires=False))
    with pytest.raises(dh.DeployError, match="cannot confirm"):
        d.reboot()
    d2, _, _ = make(FakeRemote(up_after=10**6))
    with pytest.raises(dh.DeployError, match="did not return"):
        d2.reboot(up_timeout=20)


# --- start.sh: the boot counter, run for real -----------------------------------------------


@pytest.fixture
def boot(tmp_path):
    app_dir = tmp_path / "app"
    app_dir.mkdir()
    ran = tmp_path / "ran"
    app = app_dir / "harmony_remote"
    app.write_text(f"#!/bin/sh\necho started >> {ran}\n")
    app.chmod(0o755)
    script = app_dir / "start.sh"
    script.write_text(dr.start_script())
    script.chmod(0o755)
    env = {
        **os.environ,
        "APP_DIR": str(app_dir),
        "APP_LOG": str(tmp_path / "app.log"),
    }
    env.pop("APP_BIN", None)

    class Boot:
        dir, runs, disable, count = app_dir, ran, app_dir / "DISABLE", app_dir / "bootcount"

        @staticmethod
        def start() -> None:
            subprocess.run(["sh", str(script)], env=env, check=True, timeout=10)

        @staticmethod
        def launches() -> int:
            return ran.read_text().count("started") if ran.exists() else 0

    return Boot


def test_start_sh_launches_three_boots_then_disables(boot):
    for n in (1, 2, 3):
        boot.start()
        assert boot.count.read_text().strip() == str(n)
        assert boot.launches() == n and not boot.disable.exists()
    boot.start()  # the fourth unhealthy boot
    assert boot.disable.exists() and boot.launches() == 3
    assert boot.count.read_text().strip() == "4"


def test_start_sh_respects_disable_without_counting(boot):
    boot.disable.write_text("")
    boot.start()
    assert boot.launches() == 0 and not boot.count.exists()


def test_start_sh_mark_good_resets_the_counter(boot):
    for _ in range(3):
        boot.start()
    boot.count.write_text("0\n")  # what `mark-good` (or the app) writes
    boot.start()
    assert boot.launches() == 4 and not boot.disable.exists()
    assert boot.count.read_text().strip() == "1"


def test_start_sh_tolerates_garbage_counter_and_missing_app(boot):
    boot.count.write_text("banana\n")
    boot.start()
    assert boot.count.read_text().strip() == "1" and boot.launches() == 1
    (boot.dir / "harmony_remote").unlink()
    boot.start()  # no app: counts, exits cleanly, nothing launched
    assert boot.launches() == 1 and boot.count.read_text().strip() == "2"


def test_hook_line_guards_on_executable_and_disable(boot):
    line = dr.hook_line(str(boot.dir))

    def fire() -> None:
        subprocess.run(["sh", "-c", f"{line}\nwait"], check=True, timeout=10,
                       env={**os.environ, "APP_DIR": str(boot.dir), "APP_LOG": str(boot.dir / "log")})  # fmt: skip

    boot.disable.write_text("")
    fire()
    assert boot.launches() == 0
    boot.disable.unlink()
    fire()
    assert boot.launches() == 1
    (boot.dir / "start.sh").chmod(0o644)
    fire()
    assert boot.launches() == 1  # not executable: skipped


def test_hooked_rcs_local_is_valid_shell_and_exits_zero():
    hooked_text = dr.insert_hook(RCS)
    p = subprocess.run(["sh", "-n", "-c", hooked_text], capture_output=True, check=False)
    assert p.returncode == 0, p.stderr


# --- cli ------------------------------------------------------------------------------------


def test_cli_write_steps_need_yes_and_help_marks_them(capsys):
    d, dev, _ = make()
    for cmd in (["push", "x"], ["run"], ["install-hook"], ["mark-good"], ["uninstall"], ["reboot"]):
        assert dr.main(cmd, deployer=d) == 2
    assert dev.log == []
    with pytest.raises(SystemExit):
        dr.main(["--help"])
    out = capsys.readouterr().out
    assert "[read-only]" in out and out.count("[WRITE]") >= 6


def test_cli_status_error_exit_and_run_args(capsys, art):
    d, _dev, _ = make(FakeRemote(corrupt={f"{dr.APP_DIR}/rf_probe.new"}))
    assert dr.main(["status"], deployer=d) == 0
    assert dr.main(["push", "--yes", *map(str, art)], deployer=d) == 1
    assert "MISMATCH" in capsys.readouterr().err
    d2, dev2, _ = make()
    dr.main(["push", "--yes", str(art[0])], deployer=d2)
    assert dr.main(["run", "--yes", "--", "--port", "8099"], deployer=d2) == 0
    assert dev2.fg == [[f"{dr.APP_DIR}/harmony_remote", "--port", "8099"]]


# --- brick safety: allow-list over every recorded ssh command ------------------------------

DENY = re.compile(
    r"/dev/mtd|/dev/mtdblock|mtdblock|/dev/rffw|/dev/mspfw|/dev/watchdog|1-0034|charger|"
    r"/sys/power/state|\bdd\b|nandwrite|flash_erase|flashcp|ubiformat|nohup|setsid|\bsed -i"
)
READ_ONLY_SYSFS = (dr.POWERSOURCE, dr.SOC)
ALLOWED_READS = (dr.RCS_LOCAL, dr.RCS_BACKUP, dr.BOOTCOUNT, *READ_ONLY_SYSFS)


def violations(remote: str) -> list[str]:
    if DENY.search(remote):
        return [f"denied token: {remote!r}"]
    try:
        a = shlex.split(remote)
    except ValueError:
        return [f"unparseable: {remote!r}"]
    bad = [f"not allowed: {remote!r}"]
    if not a:
        return bad
    prog, rest = a[0], a[1:]
    if a in (["uname", "-a"], ["ps"], ["reboot"]):
        return []
    if prog == "cat" and len(rest) == 1:
        return [] if rest[0] in ALLOWED_READS else bad
    if prog == "test" and len(rest) == 2 and rest[0] in ("-e", "-x"):
        return [] if dr.under_write_root(rest[1]) else bad
    if prog == "ls" and rest == ["-l", dr.APP_DIR]:
        return []
    if a == ["mkdir", "-p", dr.APP_DIR] or a == ["rm", "-rf", dr.APP_DIR]:
        return []
    if prog == "md5sum" and len(rest) == 1:
        return [] if dr.under_write_root(rest[0]) else bad
    if prog == "chmod" and len(rest) == 2 and rest[0] == "755":
        return [] if dr.under_write_root(rest[1]) else bad
    if prog == "rm" and len(rest) == 2 and rest[0] == "-f":
        return [] if dr.under_write_root(rest[1]) else bad
    if prog == "mv" and len(rest) == 2:
        return [] if all(dr.under_write_root(p) for p in rest) else bad
    if prog == "sh" and rest[:1] == ["-c"] and len(rest) == 2:
        m = re.fullmatch(r"cat > (.+)", rest[1])
        if m and dr.under_write_root(shlex.split(m[1])[0]):
            return []
    if dr.under_write_root(prog):  # running our own app: args are shell-quoted by ssh
        return []
    return bad


class Recording(dh.HubSsh):
    """Real HubSsh argv construction (quoting, -tt); records the exact ssh argv."""

    def __init__(self, fake, log):
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
        cmd.insert(1, "-tt")
        self.log.append(cmd)
        return self.fake.run_foreground(argv)


def test_checker_rejects_known_bad_and_accepts_good():
    q = lambda s: shlex.join(["sh", "-c", s])
    for bad in (
        "dd if=/dev/zero of=/data/app/x", "cat /dev/mtd0", "cat /dev/mtdblock7", "cat /dev/rffw",
        q("cat > /dev/mspfw"), "cat /dev/watchdog", q("echo 1 > /sys/bus/i2c/devices/1-0034/kill"),
        "cat /sys/class/power_supply/charger/online", q("echo mem > /sys/power/state"),
        q("cat > /etc/inittab"), q("cat > /etc/init.d/rcS"), q("cat > /data/other"),
        "rm -rf /data", "rm -f /etc/passwd", "mv /etc/init.d/rcS.local /tmp/x", "kill -9 1",
        q("cat > /data/app/../../etc/x"), "chmod 755 /etc/passwd", "mkdir -p /etc/x",
    ):  # fmt: skip
        assert violations(bad), bad
    for good in (
        "uname -a", "ps", "reboot", f"cat {dr.SOC}", f"cat {dr.RCS_LOCAL}",
        q(f"cat > {dr.RCS_TMP}"), f"mv {dr.RCS_TMP} {dr.RCS_LOCAL}",
        q("cat > /data/app/x.new"), "md5sum /data/app/x", "chmod 755 /data/app/x",
        "rm -f /var/volatile/foo", "/data/app/harmony_remote --port 1",
    ):  # fmt: skip
        assert not violations(good), good


def test_every_emitted_command_is_brick_safe(art):
    log: list[list[str]] = []
    payloads: list[bytes] = []

    def go(dev: FakeRemote, fn) -> None:
        d, _, _ = make(dev, runner=Recording(dev, log))
        try:
            fn(d)
        except dh.DeployError:
            pass  # failure paths are exercised for their commands, not their outcome
        payloads.extend(dev.written.values())

    h = FakeRemote()
    go(h, lambda d: d.status())
    go(h, lambda d: d.push(art))
    go(h, lambda d: d.run_app("harmony_remote", ["--x", "a b; rm -rf /"]))
    go(h, lambda d: d.install_hook())
    go(h, lambda d: d.install_hook())
    go(h, lambda d: d.status())
    go(h, lambda d: d.mark_good())
    go(h, lambda d: d.reboot())
    go(h, lambda d: d.uninstall())
    go(h, lambda d: d.uninstall(purge=True))
    go(FakeRemote(rcs="#!/bin/sh\nexit 0\n"), lambda d: d.install_hook())
    go(FakeRemote(corrupt={dr.RCS_TMP}), lambda d: d.install_hook())
    go(FakeRemote(corrupt={f"{dr.APP_DIR}/harmony_remote.new"}), lambda d: d.push(art))
    go(FakeRemote(upload_rc=1), lambda d: d.install_hook())
    go(FakeRemote(up_after=10**6), lambda d: d.reboot(up_timeout=20))
    assert len(log) > 80
    assert any(c[1] == "-tt" for c in log)
    bad = [(c[-1], v) for c in log if (v := violations(c[-1]))]
    assert not bad, bad
    assert all(c[0] == "ssh" and "BatchMode=yes" in c for c in log)
    # Nothing we ever write (start.sh, rcS.local, binaries are opaque) may itself touch a banned node.
    for blob in payloads:
        if blob[:2] == b"#!":
            assert not DENY.search(blob.decode()), blob
    # Stock BusyBox lacks these.
    assert not [
        c[-1] for c in log if re.search(r"\b(dd|tar|head|tail|find|df|uptime|sha256sum)\b", c[-1])
    ]


def test_internal_write_guard_rejects_outside_paths():
    for p in ("/etc/init.d/rcS", "/etc/inittab", "/data/other", "/dev/mtd0", "/data/app/../x",
              "/var/volatile/../etc/x", "/data/app", "/sys/power/state"):  # fmt: skip
        with pytest.raises(dh.DeployError):
            dr._need_write(p)
    for p in (dr.RCS_LOCAL, dr.RCS_TMP, "/data/app/x", "/var/volatile/x"):
        assert dr._need_write(p) == p
