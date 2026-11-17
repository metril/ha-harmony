#!/usr/bin/env python3
"""Deploy and run our cross-built hub daemon on a rooted Harmony Hub, non-persistently, over SSH.

Nothing here arms anything at boot: files go only under /data/ha-harmony (and a BusyBox in /tmp),
the daemon runs in the foreground of the SSH session (Ctrl-C or a dropped link kills it), and
`restore` reboots the hub, which comes back as stock. Never writes /etc/*, crontabs or any mtd.

Two measured hazards shape the design: opening /dev/i2s while stock `hal` is alive wedges the
kernel unkillably (so `quiesce` verifies nothing holds the device and `run` refuses while `hal`
lives), and a hand restart of `hal` does not work (so `restore` reboots).

Subcommands are marked [read-only] or [WRITE]; every WRITE one needs --yes.
"""

import argparse
import hashlib
import os
import re
import shlex
import subprocess
import sys
import time
from collections.abc import Callable
from dataclasses import dataclass, field
from datetime import UTC, datetime
from pathlib import Path
from typing import Protocol

from device_backup import DEFAULT_RUN_TIMEOUT, DEFAULT_SSH_TIMEOUT, SshRunner, parse_hash

REMOTE_DIR = "/data/ha-harmony"
REMOTE_HUB = f"{REMOTE_DIR}/hub"
REMOTE_SIM = f"{REMOTE_DIR}/remote_sim"
REMOTE_RF_PROBE = f"{REMOTE_DIR}/rf_probe"
REMOTE_CONFIG = f"{REMOTE_DIR}/config.json"
# tmpfs: gone after reboot. The basename MUST be "busybox": this build dispatches on argv[0] and
# does not fall back to argv[1], so any other name fails with "applet not found".
REMOTE_BUSYBOX = "/tmp/busybox"
RF_SOCKET = "/tmp/ha-harmony-rf.sock"
WRITE_ROOTS = (REMOTE_DIR, "/tmp")  # the only places this tool may write
DATA_MOUNT = "/data"
DEVICES = ("/dev/i2s", "/dev/rfspi")
LUA_NAMES = ("luaworks", "luadraws", "lua")
DEFAULT_MARGIN_KB = 512  # headroom to leave on the 5 MB jffs2 that also holds the stock config

# `ls -l` of a /proc/PID/fd directory only readlink()s the symlinks; it never opens the device
# node, so this is safe against a live `hal` (unlike any open() of /dev/i2s).
#
# The hub's BusyBox `grep` has NO `-E`: it exits 2 with "invalid option -- E" on every
# iteration, which silently reported "not held" while `hal` was in fact holding both devices.
# That false negative would let `quiesce` pass and `run` proceed, and opening /dev/i2s against a
# live `hal` wedges unkillably in `ath_i2s_close`. Basic `grep -e` only, therefore — and likewise
# no `head`, `sort`, `uniq`, `readlink`, `df`, `basename`, `xargs` or `seq` anywhere on-device.
FD_SCAN = (
    "for p in /proc/[0-9]*; do ls -l $p/fd 2>/dev/null"
    ' | grep -e " -> /dev/i2s" -e " -> /dev/rfspi"'
    ' | sed "s|^|$p |"; done'
)
PS_RE = re.compile(r"^\s*(\d+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(.*?)\s*$")
FD_RE = re.compile(r"^/proc/(\d+) .* -> (/dev/(?:i2s|rfspi))\s*$")
DF_RE = re.compile(r"(\d+)\s+(\d+)\s+(\d+)\s+(\d+)%")  # 1K-blocks used available use%


class DeployError(RuntimeError):
    """A step failed or was refused; the message is shown to the user verbatim."""


class HubSsh(SshRunner):
    """device_backup's SshRunner plus stdin streaming and a foreground (inherited-tty) run."""

    def run_input(self, argv: list[str], data: bytes) -> tuple[int, bytes, bytes]:
        try:
            p = subprocess.run(
                self._ssh(argv), input=data, capture_output=True, check=False,
                timeout=self.run_timeout,
            )  # fmt: skip
        except subprocess.TimeoutExpired:
            return 255, b"", f"timed out after {self.run_timeout:g}s".encode()
        return p.returncode, p.stdout, p.stderr

    def run_foreground(self, argv: list[str]) -> int:
        """Stream the remote command's output to our terminal; -tt so Ctrl-C/HUP reach it."""
        cmd = self._ssh(argv)
        cmd.insert(1, "-tt")
        return subprocess.call(cmd)


class Runner(Protocol):
    def run(self, argv: list[str]) -> tuple[int, bytes, bytes]: ...
    def run_input(self, argv: list[str], data: bytes) -> tuple[int, bytes, bytes]: ...
    def run_foreground(self, argv: list[str]) -> int: ...


@dataclass(frozen=True)
class Proc:
    pid: int
    user: str
    stat: str
    cmd: str

    @property
    def name(self) -> str:
        first = self.cmd.split()[0] if self.cmd.split() else ""
        return os.path.basename(first.strip("[]"))

    @property
    def alive(self) -> bool:
        return not self.stat.startswith("Z")  # zombie `[hal]` children hold nothing


def parse_ps(text: str) -> list[Proc]:
    """BusyBox `ps`: `PID USER VSZ STAT COMMAND`; header and junk lines are skipped."""
    procs = []
    for line in text.splitlines():
        if m := PS_RE.match(line):
            procs.append(Proc(int(m[1]), m[2], m[4], m[5]))
    return procs


def parse_fd_scan(text: str) -> list[tuple[int, str]]:
    """(pid, device) pairs from FD_SCAN output."""
    return sorted({(int(m[1]), m[2]) for ln in text.splitlines() if (m := FD_RE.match(ln))})


def parse_df_kb(text: str) -> int | None:
    """Available KiB from `df -k` output (handles a wrapped filesystem-name line)."""
    for line in reversed([ln for ln in text.splitlines() if ln.strip()]):
        if m := DF_RE.search(line):
            return int(m[3])
    return None


def under_write_root(path: str) -> bool:
    norm = os.path.normpath(path)
    return ".." not in path.split("/") and any(
        norm == r or norm.startswith(r + "/") for r in WRITE_ROOTS
    )


def _need_write_path(path: str) -> str:
    if not under_write_root(path):
        raise DeployError(f"internal error: refusing to write outside {WRITE_ROOTS}: {path}")
    return path


@dataclass
class Snapshot:
    uname: str = ""
    procs: list[Proc] = field(default_factory=list)
    holders: list[tuple[int, str]] = field(default_factory=list)
    free_kb: int | None = None
    ours: list[str] = field(default_factory=list)  # `ls -l` lines of our files present

    def named(self, *names: str) -> list[Proc]:
        return [p for p in self.procs if p.alive and p.name in names]

    @property
    def hal_alive(self) -> bool:
        return bool(self.named("hal"))

    @property
    def lua_alive(self) -> bool:
        return bool(self.named(*LUA_NAMES))

    def holder_names(self, device: str) -> list[str]:
        by_pid = {p.pid: p.name for p in self.procs}
        return [f"{by_pid.get(pid, '?')}[{pid}]" for pid, d in self.holders if d == device]


class Deployer:
    def __init__(
        self,
        runner: Runner,
        out: Callable[[str], None] = print,
        sleep: Callable[[float], None] = time.sleep,
        clock: Callable[[], float] = time.monotonic,
        now: Callable[[], datetime] = lambda: datetime.now(UTC),
    ) -> None:
        self.r, self.out, self.sleep, self.clock, self.now = runner, out, sleep, clock, now

    # -- helpers ------------------------------------------------------------------------
    def sh(self, script: str) -> tuple[int, bytes, bytes]:
        return self.r.run(["sh", "-c", script])

    def connect(self) -> str:
        rc, out, err = self.r.run(["uname", "-a"])
        if rc != 0:
            raise DeployError(
                f"ssh/uname failed (rc={rc}): {err.decode('utf-8', 'replace')[-200:]}"
            )
        return out.decode("utf-8", "replace").strip()

    def wait(self, pred: Callable[[], bool], timeout: float, interval: float = 2.0) -> bool:
        deadline = self.clock() + timeout
        while True:
            if pred():
                return True
            if self.clock() >= deadline:
                return False
            self.sleep(interval)

    def uptime(self) -> float | None:
        """Seconds since boot, or None when it cannot be read (ssh down, odd /proc)."""
        rc, out, _ = self.r.run(["cat", "/proc/uptime"])
        if rc != 0:
            return None
        try:
            return float(out.decode("utf-8", "replace").split()[0])
        except ValueError, IndexError:
            return None

    def procs(self) -> list[Proc]:
        rc, out, err = self.r.run(["ps"])
        if rc != 0:
            raise DeployError(f"ps failed (rc={rc}): {err.decode('utf-8', 'replace')[-200:]}")
        return parse_ps(out.decode("utf-8", "replace"))

    def holders(self) -> list[tuple[int, str]]:
        rc, out, err = self.sh(FD_SCAN)
        if rc not in (0, 1):  # the loop's last grep exits 1 when nothing matches
            raise DeployError(f"/proc fd scan failed (rc={rc}): {err.decode('utf-8', 'replace')}")
        return parse_fd_scan(out.decode("utf-8", "replace"))

    def free_kb(self) -> int | None:
        """No `df` in stock BusyBox: use the staged BusyBox's if it is there."""
        for argv in (["df", "-k", DATA_MOUNT], [REMOTE_BUSYBOX, "df", "-k", DATA_MOUNT]):
            rc, out, _ = self.r.run(argv)
            if rc == 0 and (kb := parse_df_kb(out.decode("utf-8", "replace"))) is not None:
                return kb
        return None

    def snapshot(self) -> Snapshot:
        s = Snapshot(uname=self.connect(), procs=self.procs(), holders=self.holders())
        s.free_kb = self.free_kb()
        for path in (REMOTE_DIR, REMOTE_BUSYBOX):
            rc, out, _ = self.r.run(["ls", "-l", path])
            if rc == 0:
                s.ours += [ln for ln in out.decode("utf-8", "replace").splitlines() if ln.strip()]
        return s

    # -- status [read-only] -------------------------------------------------------------
    def status(self) -> int:
        s = self.snapshot()
        p = self.out
        p(f"uname: {s.uname}")
        for name in ("hal", *LUA_NAMES):
            procs = s.named(name)
            p(
                f"  {name}: "
                + (", ".join(f"pid {x.pid}" for x in procs) if procs else "not running")
            )
        zombies = [x for x in s.procs if not x.alive]
        if zombies:
            p("  zombies: " + ", ".join(f"{x.name}[{x.pid}]" for x in zombies))
        for dev in DEVICES:
            held = s.holder_names(dev)
            p(f"  {dev}: " + ("held by " + ", ".join(held) if held else "not held"))
        p(
            f"{DATA_MOUNT} free: "
            + (f"{s.free_kb} KiB" if s.free_kb is not None else "unknown (no df; push --busybox)")
        )
        p("our files: " + ("present" if s.ours else "none"))
        for ln in s.ours:
            p(f"  {ln}")
        stock = s.hal_alive and s.lua_alive
        p("stock stack: " + ("UP" if stock else "NOT fully running (hal/lua)"))
        return 0

    # -- push [WRITE] -------------------------------------------------------------------
    def _put(self, local: Path, remote: str, mode: str | None) -> None:
        _need_write_path(remote)
        data = local.read_bytes()
        want = hashlib.md5(data, usedforsecurity=False).hexdigest()
        rc, _, err = self.r.run_input(["sh", "-c", f"cat > {shlex.quote(remote)}"], data)
        if rc != 0:
            self.r.run(["rm", "-f", remote])
            raise DeployError(
                f"{remote}: upload failed (rc={rc}): {err.decode('utf-8', 'replace')}"
            )
        rc, out, err = self.r.run(["md5sum", remote])
        got = parse_hash(out, "md5") if rc == 0 else None
        if got != want:
            self.r.run(["rm", "-f", remote])  # never leave an unverified file behind
            raise DeployError(
                f"{remote}: md5 MISMATCH (local {want}, device {got or 'unreadable'}); removed"
            )
        if mode:
            rc, _, err = self.r.run(["chmod", mode, remote])
            if rc != 0:
                raise DeployError(f"{remote}: chmod failed: {err.decode('utf-8', 'replace')}")
        self.out(f"  ok  {remote}  {len(data)} B  md5 {want}")

    def push(
        self,
        hub: Path,
        remote_sim: Path | None = None,
        rf_probe: Path | None = None,
        config: Path | None = None,
        busybox: Path | None = None,
        margin_kb: int = DEFAULT_MARGIN_KB,
        check_space: bool = True,
    ) -> int:
        items: list[tuple[Path, str, str | None]] = [(hub, REMOTE_HUB, "755")]
        if remote_sim:
            items.append((remote_sim, REMOTE_SIM, "755"))
        if rf_probe:
            items.append((rf_probe, REMOTE_RF_PROBE, "755"))
        if config:
            items.append((config, REMOTE_CONFIG, None))
        for local, _, _ in items + ([(busybox, "", None)] if busybox else []):
            if not local.is_file():
                raise DeployError(f"local file not found: {local}")
        need = sum(local.stat().st_size for local, _, _ in items)
        self.connect()
        if busybox:  # tmpfs, not counted against /data; also gives us `df`
            self._put(busybox, REMOTE_BUSYBOX, "755")
        if check_space:
            free = self.free_kb()
            if free is None:
                raise DeployError(
                    "cannot determine /data free space (stock BusyBox has no df): pass "
                    "--busybox, or --no-space-check if you accept the risk"
                )
            if free * 1024 - need < margin_kb * 1024:
                raise DeployError(
                    f"insufficient /data space: {free} KiB free, need {need} B + "
                    f"{margin_kb} KiB margin; refusing (5 MB jffs2 also holds the stock config)"
                )
        rc, _, err = self.r.run(["mkdir", "-p", REMOTE_DIR])
        if rc != 0:
            raise DeployError(f"mkdir {REMOTE_DIR} failed: {err.decode('utf-8', 'replace')}")
        for local, remote, mode in items:
            self._put(local, remote, mode)
        self.out(f"push complete: {len(items)} file(s) verified by md5 on both ends")
        return 0

    # -- quiesce [WRITE] ----------------------------------------------------------------
    def _stopped(self, *names: str) -> bool:
        return not [p for p in self.procs() if p.alive and p.name in names]

    def quiesce(self, timeout: float = 20.0) -> int:
        self.connect()
        for group in (LUA_NAMES, ("hal",)):
            for p in [x for x in self.procs() if x.alive and x.name in group]:
                self.out(f"stopping {p.name} (pid {p.pid})")
                self.r.run(["kill", str(p.pid)])  # SIGTERM only; no kill -9 near i2s
            if not self.wait(lambda g=group: self._stopped(*g), timeout):
                raise DeployError(
                    f"{'/'.join(group)} still running after {timeout:g}s; NOT proceeding. "
                    "Stock stack is partly down: run `restore --yes` (reboots) to recover."
                )
        held = self.holders()
        if held:
            names = {p.pid: p.name for p in self.procs()}
            raise DeployError(
                "device still held, NOT safe to open: "
                + ", ".join(f"{names.get(pid, '?')}[{pid}] -> {d}" for pid, d in held)
                + ". Do not open /dev/i2s. Run `restore --yes` to reboot back to stock."
            )
        self.out("=" * 72)
        self.out(f"  QUIESCED at {self.now().isoformat(timespec='seconds')}")
        self.out("  The physical remote and HA's stock Harmony integration are DOWN.")
        self.out("  /dev/i2s and /dev/rfspi are unheld. Finish and run: restore --yes")
        self.out("=" * 72)
        return 0

    # -- run [WRITE] --------------------------------------------------------------------
    def run_hub(self, args: list[str]) -> int:
        self.connect()
        if args != ["--help"]:  # --help never opens a device
            if self.named_alive("hal"):
                raise DeployError(
                    "stock hal is still alive and holds /dev/i2s: run `quiesce --yes` first"
                )
            if held := self.holders():
                raise DeployError(f"devices still held by pids {sorted({p for p, _ in held})}")
        if self.r.run(["test", "-x", REMOTE_HUB])[0] != 0:
            raise DeployError(f"{REMOTE_HUB} missing or not executable: run push first")
        self.out(f"running {REMOTE_HUB} {shlex.join(args)} (foreground; Ctrl-C stops it)")
        return self.r.run_foreground([REMOTE_HUB, *args])

    def named_alive(self, name: str) -> bool:
        return any(p.alive and p.name == name for p in self.procs())

    # -- press [WRITE] ------------------------------------------------------------------
    def press(self, key: int, sock: str = RF_SOCKET) -> int:
        _need_write_path(sock)
        rc, out, err = self.r.run([REMOTE_SIM, "--rf", sock, "--press", str(key)])
        self.out(out.decode("utf-8", "replace").rstrip())
        if rc != 0:
            raise DeployError(f"remote_sim exited {rc}: {err.decode('utf-8', 'replace')[-200:]}")
        return 0

    # -- restore [WRITE] ----------------------------------------------------------------
    def restore(
        self,
        down_timeout: float = 90.0,
        up_timeout: float = 600.0,
        settle_timeout: float = 240.0,
        interval: float = 5.0,
    ) -> int:
        self.connect()
        t0 = self.clock()
        before = self.uptime()
        self.out(f"rebooting hub at {self.now().isoformat(timespec='seconds')} (expect ~4-5 min)")
        rc, _, err = self.r.run(["reboot"])
        if rc not in (0, 255):
            raise DeployError(f"reboot failed (rc={rc}): {err.decode('utf-8', 'replace')[-200:]}")
        # Observing the offline window is unreliable: the hub can stay answerable for a few seconds
        # and be back before the next poll, which falsely looked like "the reboot never fired".
        # A dropped uptime is positive proof it restarted, so that is the real check; going offline
        # is merely the common case.
        if self.wait(lambda: self.r.run(["uname", "-a"])[0] != 0, down_timeout, interval):
            self.out("hub is down; waiting for ssh to return")
        else:
            now = self.uptime()
            if now is not None and before is not None and now < before:
                self.out(f"hub rebooted (uptime {before:g}s -> {now:g}s) without a visible gap")
            else:
                raise DeployError(
                    "cannot confirm the reboot: the hub stayed reachable and its uptime did not "
                    "drop. Power-cycle it; nothing is armed at boot so it comes up stock."
                )
        if not self.wait(lambda: self.r.run(["uname", "-a"])[0] == 0, up_timeout, interval):
            raise DeployError(
                f"ssh did not return within {up_timeout:g}s. Power-cycle the hub; nothing is armed "
                "at boot so it comes up stock. If it will not boot: factory reset + re-root."
            )
        self.out("ssh is back; waiting for the stock stack")

        def stock_up() -> bool:
            try:
                s = Snapshot(procs=self.procs(), holders=self.holders())
            except DeployError:
                return False
            return (
                s.hal_alive and s.lua_alive and all(any(d == dev for _, d in s.holders)
                                                    for dev in DEVICES)
            )  # fmt: skip

        ok = self.wait(stock_up, settle_timeout, interval)
        elapsed = self.clock() - t0
        if not ok:
            raise DeployError(
                f"ssh is back after {elapsed:.0f}s but CANNOT CONFIRM the stock stack (hal + lua "
                "running and holding /dev/i2s, /dev/rfspi). Check the physical remote and HA's "
                "`remote.harmony`; `status` shows detail; reboot again if needed."
            )
        self.out(f"RESTORED: hal + lua up and holding the devices; outage {elapsed:.0f}s")
        self.out("Confirm the physical remote and HA's remote.harmony yourself.")
        return 0

    # -- clean [WRITE] ------------------------------------------------------------------
    def clean(self) -> int:
        self.connect()
        rc, _, err = self.r.run(["rm", "-rf", _need_write_path(REMOTE_DIR)])
        if rc != 0:
            raise DeployError(f"rm {REMOTE_DIR} failed: {err.decode('utf-8', 'replace')}")
        self.r.run(["rm", "-f", _need_write_path(REMOTE_BUSYBOX)])
        self.out(f"removed {REMOTE_DIR} (and {REMOTE_BUSYBOX} if present)")
        return 0


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("--host", default="harmony-hub", help="ssh host/alias (default %(default)s)")
    ap.add_argument("--port", type=int, default=None)
    ap.add_argument("--user", default="root")
    ap.add_argument("--key", default=None, help="ssh key (default: the alias's own)")
    ap.add_argument("--ssh-timeout", type=int, default=DEFAULT_SSH_TIMEOUT, metavar="SEC")
    sub = ap.add_subparsers(dest="cmd", required=True)

    def add(name: str, help_: str, write: bool = True) -> argparse.ArgumentParser:
        p = sub.add_parser(name, help=help_, description=help_)
        if write:
            p.add_argument("--yes", action="store_true", help="confirm this WRITE step")
        return p

    add("status", "[read-only] stock processes, device holders, /data free space, our files", False)
    p = add(
        "push", "[WRITE] copy hub (+remote_sim, rf_probe, config, BusyBox) to /data/ha-harmony, md5-verified"
    )
    p.add_argument("--hub", type=Path, required=True)
    p.add_argument("--remote-sim", type=Path)
    p.add_argument("--rf-probe", type=Path, help="read-only CC2544 probe tool")
    p.add_argument("--config", type=Path)
    p.add_argument("--busybox", type=Path, help="static MIPS BusyBox, staged to /tmp (gives df)")
    p.add_argument("--margin-kb", type=int, default=DEFAULT_MARGIN_KB)
    p.add_argument("--no-space-check", action="store_true")
    p = add("quiesce", "[WRITE] stop lua* then hal; verify devices unheld. BREAKS remote + HA")
    p.add_argument("--timeout", type=float, default=20.0)
    p = add("run", "[WRITE] run the hub in the foreground over ssh (args after --)")
    p.add_argument("hub_args", nargs=argparse.REMAINDER)
    p = add("press", "[WRITE] run remote_sim --rf SOCK --press KEY (numeric key code)")
    p.add_argument("key", type=int)
    p.add_argument("--sock", default=RF_SOCKET)
    p = add("restore", "[WRITE] reboot the hub back to stock; wait; verify hal + lua")
    p.add_argument("--up-timeout", type=float, default=600.0)
    add("clean", "[WRITE] remove /data/ha-harmony")
    return ap


def main(
    argv: list[str] | None = None, runner: Runner | None = None, deployer: Deployer | None = None
) -> int:
    ap = build_parser()
    a = ap.parse_args(argv)
    if a.cmd != "status" and not a.yes:
        print(f"{a.cmd} is a WRITE step on the real hub; re-run with --yes to confirm",
              file=sys.stderr)  # fmt: skip
        return 2
    if a.cmd == "press" and not 0 <= a.key <= 255:
        ap.error("key must be 0..255")
    d = deployer or Deployer(
        runner
        or HubSsh(
            a.host,
            a.user,
            a.key,
            timeout=a.ssh_timeout,
            run_timeout=DEFAULT_RUN_TIMEOUT,
            port=a.port,
        )
    )
    try:
        match a.cmd:
            case "status":
                return d.status()
            case "push":
                return d.push(a.hub, a.remote_sim, a.rf_probe, a.config, a.busybox,
                              a.margin_kb, not a.no_space_check)  # fmt: skip
            case "quiesce":
                return d.quiesce(a.timeout)
            case "run":
                args = a.hub_args[1:] if a.hub_args[:1] == ["--"] else a.hub_args
                return d.run_hub(args)
            case "press":
                return d.press(a.key, a.sock)
            case "restore":
                return d.restore(up_timeout=a.up_timeout)
            case "clean":
                return d.clean()
    except DeployError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("interrupted", file=sys.stderr)
        return 130
    return 1


if __name__ == "__main__":
    sys.exit(main())
