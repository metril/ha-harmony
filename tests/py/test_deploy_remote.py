"""deploy_remote against a fake, stateful remote behind a fake SSH transport (no network)."""

import hashlib
import os
import re
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import deploy_hub as dh
import deploy_remote as dr

_BB = shutil.which("busybox")
SHELLS = ["/bin/sh"] + ([f"{_BB}:sh"] if _BB else [])
_CUR = {"shell": ["/bin/sh"]}


@pytest.fixture(autouse=True, params=SHELLS, ids=lambda x: "busybox" if ":" in x else "sh")
def _shell(request):
    """Every start.sh / selftest run below goes through /bin/sh and, when present, host busybox."""
    sh = request.param
    _CUR["shell"] = sh.split(":") if ":" in sh else [sh]
    yield


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

    def __init__(
        self, rcs=RCS, corrupt=(), upload_rc=0, up_after=3, reboot_fires=True, mangle_mv=None
    ):
        self.mangle_mv = {k: list(v) for k, v in (mangle_mv or {}).items()}  # dest -> [bytes|None]
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
        if a[:2] == ["mkdir", "-p"] and (a[2] == dr.APP_DIR or a[2] == dr.SELFTEST_DIR):
            return 0, b"", b""
        if a[:2] == ["sh", f"{dr.SELFTEST_DIR}/remote_selftest.sh"]:
            return self.selftest(a)
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
        if a[:2] == ["sh", "-n"]:
            data = self.files.get(a[2])
            if data is None:
                return 2, b"", b"No such file"
            p = subprocess.run(["sh", "-n"], input=data, capture_output=True, check=False)
            return p.returncode, b"", p.stderr
        if a[0] == "mv":
            self.files[a[2]] = self.files.pop(a[1])
            self.modes[a[2]] = self.modes.pop(a[1], "644")
            if self.mangle_mv.get(a[2]):
                bad = self.mangle_mv[a[2]].pop(0)
                if bad is not None:
                    self.files[a[2]] = bad
            return 0, b"", b""
        if a[:2] == ["rm", "-f"]:
            self.files.pop(a[2], None)
            return 0, b"", b""
        if a[:2] == ["rm", "-rf"] and a[2] == dr.SELFTEST_DIR:
            pre = a[2] + "/"
            self.files = {p: d for p, d in self.files.items() if not p.startswith(pre)}
            self.selftest_removed = True
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

    selftest_rc = 0
    selftest_removed = False

    def selftest(self, a):
        """Stand-in for the on-device script: assert what was uploaded, report a result."""
        t = a[2]
        assert a[3] == "--slot 1 --powermode 0x30"
        assert f"{t}/start.sh" in self.files and f"{t}/hookline.sh" in self.files
        self.files[f"{t}/app/ran"] = b"x"  # a file the run created: cleanup must remove it
        return self.selftest_rc, b"PASS  fake\nremote selftest done: 0 failure(s)\n", b""

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
    assert dev.log[i + 3] == ["mv", f"{dr.APP_DIR}/rf_probe.new", f"{dr.APP_DIR}/rf_probe"]


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
    d, dev, lines = make()
    assert d.install_hook() == 0
    assert dev.files[dr.RCS_BACKUP] == RCS.encode()
    assert dev.files[dr.START_SH] == dr.start_script().encode()
    assert "--slot 1 --powermode 0x30" in dev.files[dr.START_SH].decode()
    text = hooked(dev)
    assert text == dr.insert_hook(RCS) and text.count(dr.HOOK_MARKER) == 1
    assert dev.modes[dr.START_SH] == "755" and dev.modes[dr.RCS_LOCAL] == "755"
    # rcS.local is never written directly: temp, verify, mv
    uploads = [c[2] for c in dev.log if c[:2] == ["sh", "-c"]]
    assert f"cat > {dr.RCS_TMP}" in uploads and f"cat > {dr.RCS_LOCAL}" not in uploads
    i = dev.log.index(["md5sum", dr.RCS_TMP])
    mv = dev.log.index(["mv", dr.RCS_TMP, dr.RCS_LOCAL])
    # vetted on the device BEFORE the mv, and again after it
    assert ["sh", "-n", dr.RCS_TMP] in dev.log[i:mv] and ["cat", dr.RCS_TMP] in dev.log[i:mv]
    assert dev.log[mv + 1 :][:2] == [["md5sum", dr.RCS_LOCAL], ["sh", "-n", dr.RCS_LOCAL]]
    assert dr.RCS_TMP not in dev.files
    assert any("UNKNOWN" in ln for ln in lines) and lines.index(
        next(ln for ln in lines if "UNKNOWN" in ln)
    ) < lines.index(next(ln for ln in lines if "hook installed in" in ln))


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
    d, dev, _ = make()
    dev.files[dr.RCS_BACKUP] = b"PRECIOUS ORIGINAL\n"
    with pytest.raises(dh.DeployError, match="refresh-backup"):
        d.install_hook()
    assert dev.files[dr.RCS_BACKUP] == b"PRECIOUS ORIGINAL\n"
    assert dr.RCS_BACKUP not in dev.written and dr.RCS_BACKUP + ".new" not in dev.written
    assert dev.files[dr.RCS_LOCAL] == RCS.encode()


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
    mv = dev.log.index(["mv", dr.RCS_TMP, dr.RCS_LOCAL])
    assert ["sh", "-n", dr.RCS_TMP] in dev.log[i:mv]
    assert dev.log[-2:] == [["md5sum", dr.RCS_LOCAL], ["sh", "-n", dr.RCS_LOCAL]]


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
    app.write_text(f'#!/bin/sh\necho started >> {ran}\necho "$@" > {tmp_path}/argv\n')
    app.chmod(0o755)
    script = app_dir / "start.sh"
    script.write_text(dr.start_script())
    script.chmod(0o755)
    env = {
        **os.environ,
        "APP_DIR": str(app_dir),
        "APP_LOG": str(tmp_path / "app.log"),
        "START_TICK": "0.02",
    }
    env.pop("APP_BIN", None)

    class Boot:
        dir, runs, disable, count = app_dir, ran, app_dir / "DISABLE", app_dir / "bootcount"
        argv, log, env_ = tmp_path / "argv", tmp_path / "app.log", env

        @staticmethod
        def start() -> None:
            subprocess.run([*_CUR["shell"], str(script)], env=env, check=True, timeout=10)

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
        subprocess.run([*_CUR["shell"], "-c", f"{line}\nwait"], check=True, timeout=10,
                       env={**os.environ, "APP_DIR": str(boot.dir), "APP_LOG": str(boot.dir / "log"), "START_TICK": "1"})  # fmt: skip

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
    p = subprocess.run([*_CUR["shell"], "-n", "-c", hooked_text], capture_output=True, check=False)
    assert p.returncode == 0, p.stderr


# --- review fixes: argv, backup hygiene, rcS vetting, hardened start.sh, selftest -----------


def test_start_sh_runs_app_with_exact_slot_and_powermode(boot):
    boot.start()
    assert boot.argv.read_text().strip() == "--slot 1 --powermode 0x30"


@pytest.mark.parametrize(("slot", "pm"), [(7, "0xAB"), (15, "0x00")])
def test_start_script_argv_is_configurable(slot, pm):
    text = dr.start_script(slot=slot, powermode=pm)
    assert f'"$APP_BIN" --slot {slot} --powermode 0x{pm[2:].lower()} 2>&1' in text


@pytest.mark.parametrize("slot", ["0", "16", "x", "-1", "1 ; reboot", ""])
def test_slot_validation(slot):
    with pytest.raises(Exception, match="slot"):
        dr.parse_slot(slot)
    with pytest.raises(SystemExit):
        dr.main(["install-hook", "--yes", "--slot", slot])


@pytest.mark.parametrize("pm", ["30", "0x3", "0x300", "0xZZ", "0x30; reboot", ""])
def test_powermode_validation(pm):
    with pytest.raises(Exception, match="powermode"):
        dr.parse_powermode(pm)


def test_install_hook_builds_slot_powermode_into_start_sh_and_status_shows_them():
    d, dev, lines = make()
    d.install_hook(slot=5, powermode="0x2A")
    assert dev.files[dr.START_SH] == dr.start_script(slot=5, powermode="0x2a").encode()
    lines.clear()
    d.status()
    assert "start.sh app argv: --slot 5 --powermode 0x2a" in "\n".join(lines)


def test_cli_install_hook_defaults_and_flags():
    d, dev, _ = make()
    assert dr.main(["install-hook", "--yes"], deployer=d) == 0
    assert b"--slot 1 --powermode 0x30" in dev.files[dr.START_SH]
    d2, dev2, _ = make()
    assert (
        dr.main(["install-hook", "--yes", "--slot", "9", "--powermode", "0x31"], deployer=d2) == 0
    )
    assert b"--slot 9 --powermode 0x31" in dev2.files[dr.START_SH]


def test_start_sh_counter_classes_never_error_and_count_clean(boot):
    for junk in ("", "08", "09", "banana", "1 2", "0003", "99999999999999999999", "-1", "4"):
        boot.count.write_text(junk + "\n")
        boot.disable.unlink(missing_ok=True)
        boot.start()
        assert boot.count.read_text().strip() == "1", junk
        assert not boot.disable.exists(), junk


def test_start_sh_unwritable_counter_does_not_launch(boot):
    boot.count.mkdir()
    before = boot.launches()
    boot.start()  # exits 0 (check=True) without starting the app
    assert boot.launches() == before == 0


def test_start_sh_unwritable_log_still_launches(boot):
    boot.env_["APP_LOG"] = str(boot.dir / "no" / "such" / "log")
    boot.start()
    assert boot.launches() == 1


def test_start_sh_caps_the_log_in_place(boot):
    app = boot.dir / "harmony_remote"
    app.write_text(
        "#!/bin/sh\ni=0\nwhile [ $i -lt 60 ]; do echo line$i; i=$((i+1)); done\nsleep 0.3\n"
        f"echo AFTER >> {boot.log}\nsleep 0.2\n"
    )
    boot.env_["START_LOG_MAX_LINES"] = "10"
    boot.start()
    n = len(boot.log.read_text().splitlines())
    assert 1 <= n < 60


def test_start_sh_has_no_arithmetic_on_file_content_and_no_missing_applets():
    text = dr.start_script()
    assert "$((n + 1))" in text and 'case "$n" in 0 | 1 | 2 | 3)' in text
    assert not re.search(r"\b(head|tail|wc|dd|sync|basename|find|df|tar)\b", text)


FAST = {"ST_TICK": "0.02", "ST_NAP": "0.3"}


def test_selftest_script_passes_on_the_host(tmp_path):
    t = tmp_path / "st"
    t.mkdir()
    (t / "start.sh").write_text(dr.start_script())
    (t / "rcS.local.hooked").write_text(dr.insert_hook(RCS))
    (t / "hookline.sh").write_text(dr.hook_line(str(t / "app")) + "\n")
    p = subprocess.run(
        [*_CUR["shell"], str(dr.SELFTEST_SRC), str(t), "--slot 1 --powermode 0x30"],
        capture_output=True, text=True, timeout=120, check=False,
        env={**os.environ, "REAL_COUNT_FILE": str(tmp_path / "real-bootcount"), **FAST},
    )  # fmt: skip
    assert p.returncode == 0, p.stdout + p.stderr
    assert "FAIL" not in p.stdout and p.stdout.count("PASS") >= 18, p.stdout
    assert not (tmp_path / "real-bootcount").exists()


def test_selftest_script_catches_a_broken_start_sh(tmp_path):
    t = tmp_path / "st"
    t.mkdir()
    (t / "start.sh").write_text(dr.start_script().replace("n=$((n + 1))", "n=$((n + 2))"))
    (t / "rcS.local.hooked").write_text(dr.insert_hook(RCS))
    (t / "hookline.sh").write_text(dr.hook_line(str(t / "app")) + "\n")
    p = subprocess.run([*_CUR["shell"], str(dr.SELFTEST_SRC), str(t), "--slot 1 --powermode 0x30"],
                       capture_output=True, text=True, timeout=120, check=False, env={**os.environ, **FAST})  # fmt: skip
    assert p.returncode != 0 and "FAIL" in p.stdout


def test_selftest_cleans_up_and_touches_only_its_dir():
    d, dev, lines = make()
    assert d.selftest() == 0
    assert dev.selftest_removed and not [p for p in dev.files if p.startswith(dr.SELFTEST_DIR)]
    assert dev.files[dr.RCS_LOCAL] == RCS.encode()  # the real rcS.local is never written
    assert dr.RCS_LOCAL not in dev.written and dr.RCS_TMP not in dev.written
    for c in dev.log:
        if c in (["uname", "-a"], ["cat", dr.RCS_LOCAL]):
            continue
        assert dr.SELFTEST_DIR in " ".join(c), c
    hooked_copy = dev.written[f"{dr.SELFTEST_DIR}/rcS.local.hooked.new"].decode()
    assert hooked_copy == dr.insert_hook(RCS) or dr.HOOK_MARKER in hooked_copy
    assert any("1 failure" not in ln and "done: 0 failure" in ln for ln in lines)
    assert any("removed" in ln for ln in lines)


def test_selftest_failure_still_cleans_up_and_exits_nonzero():
    dev = FakeRemote()
    dev.selftest_rc = 1
    d, dev, _ = make(dev)
    assert d.selftest() == 1 and dev.selftest_removed


def test_selftest_upload_failure_still_cleans_up():
    d, dev, _ = make(FakeRemote(corrupt={f"{dr.SELFTEST_DIR}/start.sh.new"}))
    with pytest.raises(dh.DeployError, match="MISMATCH"):
        d.selftest()
    assert dev.selftest_removed


def test_selftest_real_hook_line_is_rooted_in_the_selftest_dir():
    line = dr.hook_line(dr.SELFTEST_DIR + "/app")
    assert "/data/app" not in line and line.count(dr.SELFTEST_DIR) == 3


# backup hygiene


def test_stale_backup_refuses_install_without_refresh():
    d, dev, _ = make()
    dev.files[dr.RCS_BACKUP] = b"#!/bin/sh\n/data/dropbearmulti dropbear\nexit 0\n"
    with pytest.raises(dh.DeployError, match="differs.*--refresh-backup|--refresh-backup"):
        d.install_hook()
    assert dev.written == {} and dev.files[dr.RCS_LOCAL] == RCS.encode()


def test_refresh_backup_replaces_stale_backup_then_installs():
    d, dev, _ = make()
    dev.files[dr.RCS_BACKUP] = b"stale\n"
    d.install_hook(refresh_backup=True)
    assert dev.files[dr.RCS_BACKUP] == RCS.encode() and dr.HOOK_MARKER in hooked(dev)


def test_matching_backup_is_kept_untouched():
    d, dev, lines = make()
    dev.files[dr.RCS_BACKUP] = RCS.encode()
    d.install_hook()
    assert dr.RCS_BACKUP not in dev.written and dr.RCS_BACKUP + ".new" not in dev.written
    assert any("already matches" in ln for ln in lines)


def test_uninstall_refuses_backup_without_dropbear_or_exit():
    for bad in ("#!/bin/sh\nexit 0\n", "#!/bin/sh\n/data/dropbearmulti dropbear\n", "",
                dr.insert_hook(RCS)):  # fmt: skip
        d, dev, _ = make()
        d.install_hook()
        dev.files[dr.RCS_BACKUP] = bad.encode()
        before = dev.files[dr.RCS_LOCAL]
        with pytest.raises(dh.DeployError, match="refusing to restore"):
            d.uninstall()
        assert dev.files[dr.RCS_LOCAL] == before


# rcS.local vetting on the device


def test_syntax_error_candidate_aborts_before_mv_original_untouched():
    class BadSyntax(FakeRemote):
        def run(self, argv):
            if argv[:2] == ["sh", "-n"] and argv[2] == dr.RCS_TMP:
                self.log.append(argv)
                return 2, b"", b"syntax error: unexpected end of file"
            return super().run(argv)

    d, dev, _ = make(BadSyntax())
    with pytest.raises(dh.DeployError, match=r"sh -n.*rejects"):
        d.install_hook()
    assert dev.files[dr.RCS_LOCAL] == RCS.encode() and dr.RCS_TMP not in dev.files
    assert not [c for c in dev.log if c[0] == "mv" and c[2] == dr.RCS_LOCAL]


def test_candidate_missing_dropbear_is_caught_by_shape_check():
    assert dr.rcs_shape_problem("#!/bin/sh\nexit 0\n", False)
    assert dr.rcs_shape_problem(RCS, True)  # hook expected, absent
    assert dr.rcs_shape_problem(dr.insert_hook(RCS), True) is None
    assert dr.rcs_shape_problem(dr.insert_hook(RCS), False)
    twice = dr.insert_hook(RCS).replace("exit 0\n", dr.hook_line() + "\nexit 0\n")
    assert dr.rcs_shape_problem(twice, True)
    before = RCS.replace("/data/dropbearmulti dropbear -R -p 0.0.0.0:22\n", "")
    assert dr.rcs_shape_problem(before.replace("exit 0", dr.hook_line() + "\nexit 0"), True)


def test_post_mv_corruption_is_restored_from_the_previous_content():
    d, dev, lines = make(FakeRemote(mangle_mv={dr.RCS_LOCAL: [b"#!/bin/sh\nif\n"]}))
    with pytest.raises(dh.DeployError, match="previous content restored and verified"):
        d.install_hook()
    assert dev.files[dr.RCS_LOCAL] == RCS.encode()
    assert any("POST-WRITE CHECK FAILED" in ln for ln in lines)
    assert dev.log.count(["sh", "-n", dr.RCS_LOCAL]) == 1  # the restore (first swap failed on md5)


def test_post_mv_md5_mismatch_is_restored():
    d, dev, _ = make(FakeRemote(mangle_mv={dr.RCS_LOCAL: [RCS.encode() + b"#x\n"]}))
    with pytest.raises(dh.DeployError, match="restored and verified"):
        d.install_hook()
    assert dev.files[dr.RCS_LOCAL] == RCS.encode()


def test_failed_restore_is_loud():
    d, _dev, _ = make(FakeRemote(mangle_mv={dr.RCS_LOCAL: [b"if\n", b"if\n"]}))
    with pytest.raises(dh.DeployError, match="restore failed.*do NOT reboot"):
        d.install_hook()


def test_uninstall_post_mv_failure_restores_the_hooked_file():
    d, dev, _ = make()
    d.install_hook()
    hooked_before = dev.files[dr.RCS_LOCAL]
    dev.mangle_mv = {dr.RCS_LOCAL: [b"if\n"]}
    with pytest.raises(dh.DeployError, match="restored and verified"):
        d.uninstall()
    assert dev.files[dr.RCS_LOCAL] == hooked_before


def test_uninstall_verifies_with_sh_n_and_md5():
    d, dev, _ = make()
    d.install_hook()
    dev.log.clear()
    d.uninstall()
    assert ["sh", "-n", dr.RCS_LOCAL] in dev.log and ["md5sum", dr.RCS_LOCAL] in dev.log


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
ALLOWED_READS = (
    dr.RCS_LOCAL, dr.RCS_TMP, dr.RCS_BACKUP, dr.BOOTCOUNT, dr.START_SH, *READ_ONLY_SYSFS,
)  # fmt: skip


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
    if a in (["mkdir", "-p", dr.APP_DIR], ["rm", "-rf", dr.APP_DIR],
             ["mkdir", "-p", dr.SELFTEST_DIR], ["rm", "-rf", dr.SELFTEST_DIR]):  # fmt: skip
        return []
    if prog == "sh" and len(rest) == 2 and rest[0] == "-n":
        return [] if dr.under_write_root(rest[1]) or rest[1] == dr.RCS_LOCAL else bad
    if prog == "sh" and rest[:1] == [f"{dr.SELFTEST_DIR}/remote_selftest.sh"]:
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
    go(FakeRemote(), lambda d: d.selftest())
    go(FakeRemote(corrupt={f"{dr.SELFTEST_DIR}/start.sh.new"}), lambda d: d.selftest())
    go(FakeRemote(mangle_mv={dr.RCS_LOCAL: [b"if\n"]}), lambda d: d.install_hook())
    go(FakeRemote(mangle_mv={dr.RCS_LOCAL: [b"if\n", b"if\n"]}), lambda d: d.install_hook())
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
