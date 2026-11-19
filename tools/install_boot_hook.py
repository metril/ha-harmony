#!/usr/bin/env python3
"""Install, inspect or remove the ha-harmony boot hook (Phase 4, T2/T3).

Separate from deploy_hub.py on purpose. deploy_hub has a test asserting it NEVER writes
/etc/init.d, /etc/inittab, /etc/rc* or any /dev/mtd*, and that invariant is worth keeping
intact for the bench tooling. This tool is the one place allowed to touch /etc/init.d, and
it is correspondingly narrow: two files there, one marker, and nothing else, ever.

WHAT IT DOES. Stock /etc/init.d/rcS sources /etc/init.d/rcS.local at line 43. We append a
guarded block to rcS.local that sources our /etc/init.d/rcS.harmony when the marker
/etc/ha-harmony-enable exists. See docs/plans/2026-11-17-phase4-persistence.md.

ORDERING IS THE SAFETY PROPERTY. The marker is created LAST and removed FIRST, and the
appended block is inert without it. So every intermediate state -- including every way this
tool can fail halfway -- is a hub that boots pure stock:

    upload rcS.harmony   -> inert (nothing sources it)
    write hub.conf       -> inert
    append to rcS.local  -> inert (guard fails: no marker)
    create marker        -> ARMED
    remove marker        -> disarmed, instantly

After editing rcS.local we run `sh -n` on it ON THE DEVICE before going any further,
because a syntax error there aborts rcS at line 43 -- before dropbear starts at line 47 --
which costs SSH, the stock stack, and leaves only UART or a factory reset. If that check
fails we restore the backup and abort without ever creating the marker.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from deploy_hub import DEFAULT_SSH_TIMEOUT, HubSsh, Runner

REMOTE_DIR = "/data/ha-harmony"
CONF = f"{REMOTE_DIR}/hub.conf"
DAEMON = f"{REMOTE_DIR}/hub"
BACKUP_DIR = f"{REMOTE_DIR}/backup"
RCS = "/etc/init.d/rcS"  # never written; read only, and only to report its hook line
RCS_LOCAL = "/etc/init.d/rcS.local"
RCS_HARMONY = "/etc/init.d/rcS.harmony"
MARKER = "/etc/ha-harmony-enable"
NOWATCHDOG = "/etc/nowatchdog"  # read only (test -e); stock rcS:59 skips the watchdog while it exists
COUNTER = "/cache/ha-harmony-boot.n"
PIDFILE = "/cache/ha-harmony.pid"
LOG = "/cache/ha-harmony.log"
DAEMON_LOG = "/var/volatile/ha-harmony.log"  # tmpfs; read-only here, the hook writes it

# The only paths this tool may write. Asserted by tests over the recorded command log.
WRITE_PATHS = (RCS_LOCAL, RCS_HARMONY, MARKER, REMOTE_DIR, COUNTER, PIDFILE, LOG)

BEGIN = "# >>> ha-harmony boot hook >>>"
END = "# <<< ha-harmony boot hook <<<"

# Guarded so a missing/removed rcS.harmony is a no-op rather than a boot error, and so
# removing one file (the marker) is a complete disable.
BLOCK = f"""{BEGIN}
# Installed by tools/install_boot_hook.py. Disable with: rm {MARKER}
if [ -r {MARKER} ] && [ -r {RCS_HARMONY} ]; then
\t. {RCS_HARMONY}
fi
{END}"""

HOOK_SRC = Path(__file__).resolve().parent / "boot" / "rcS.harmony"
SELFTEST_SRC = Path(__file__).resolve().parent / "boot" / "hook_selftest.sh"
SELFTEST_DIR = "/tmp/hah-selftest"  # tmpfs; the ONLY place selftest writes


class InstallError(RuntimeError):
    """Refused or failed; the message is shown to the user verbatim."""


TOKEN_RE = re.compile(r"[A-Za-z0-9._~+/=-]{1,256}")  # keep in step with rcS.harmony
HUB_ID_RE = re.compile(r"[A-Za-z0-9._-]{1,63}")


def md5_bytes(b: bytes) -> str:
    return hashlib.md5(b, usedforsecurity=False).hexdigest()


class HookInstaller:
    def __init__(self, runner: Runner, hook_src: Path = HOOK_SRC) -> None:
        self.r = runner
        self.hook_src = hook_src

    # --- primitives ---------------------------------------------------------------

    def _run(self, argv: list[str], what: str) -> bytes:
        rc, out, err = self.r.run(argv)
        if rc != 0:
            raise InstallError(f"{what} failed (rc={rc}): {err.decode(errors='replace').strip()}")
        return out

    def _read(self, path: str) -> bytes | None:
        rc, out, _ = self.r.run(["cat", path])
        return out if rc == 0 else None

    def _exists(self, path: str) -> bool:
        rc, _, _ = self.r.run(["test", "-e", path])
        return rc == 0

    def _inode(self, path: str) -> int | None:
        rc, out, _ = self.r.run(["ls", "-i", path])
        m = re.match(r"\s*(\d+)\s", out.decode(errors="replace")) if rc == 0 else None
        return int(m.group(1)) if m else None

    def _write(self, path: str, data: bytes, mode: str, atomic: bool = False) -> None:
        """Write via a temp file then `mv`, so a half-written boot file never exists.

        `mv` within the same filesystem is atomic; /etc is the unionfs rw branch (mtd4),
        the same branch /tmp is not -- hence the temp file goes next to the target.
        """
        tmp = f"{path}.hah-tmp"
        rc, _, err = self.r.run_input(["sh", "-c", f"cat > {tmp}"], data)
        if rc != 0:
            raise InstallError(f"writing {tmp} failed: {err.decode(errors='replace').strip()}")
        self._run(["chmod", mode, tmp], f"chmod {tmp}")
        before = self._inode(tmp) if atomic else None
        self._run(["mv", tmp, path], f"mv into place {path}")
        if atomic:
            # /etc is a unionfs; when the target lives only in the read-only lower layer a
            # rename may be a copy-up (not atomic), but unionfs may also hand out its own inode
            # numbers, so a mismatch can be a false positive. rcS sources rcS.local only under
            # `[ -x ... ]`, so a briefly missing file is a stock boot with SSH up: warn, do not refuse.
            after = self._inode(path)
            if before is None or after is None or before != after:
                print(
                    f"WARNING: {path}: mv not provably an atomic rename (inode {before} -> {after}); "
                    "content is verified by read-back, and a missing rcS.local still boots stock"
                )
        got = self._read(path)
        if got is None or md5_bytes(got) != md5_bytes(data):
            raise InstallError(f"verify failed for {path}: content differs after write")

    def _syntax_ok(self, path: str) -> tuple[bool, str]:
        rc, _, err = self.r.run(["sh", "-n", path])
        return rc == 0, err.decode(errors="replace").strip()

    # --- commands -----------------------------------------------------------------

    def _md5_on_device(self, path: str) -> str | None:
        rc, out, _ = self.r.run(["md5sum", path])
        m = re.match(r"([0-9a-f]{32})\b", out.decode(errors="replace")) if rc == 0 else None
        return m.group(1) if m else None

    @staticmethod
    def redact(conf: str) -> str:
        return re.sub(r"(?m)^(token=).*$", r"\1<redacted>", conf)

    def _tail(self, title: str, path: str, n: int = 8) -> None:
        log = self._read(path)
        print(f"--- {title} ({path}) ---")
        if not log:
            print("  (absent or empty)")
            return
        for line in log.decode(errors="replace").strip().splitlines()[-n:]:
            print(f"  {line}")

    def status(self) -> int:
        rcs = self._read(RCS_LOCAL)
        armed = self._exists(MARKER)
        print(f"marker {MARKER}: {'PRESENT (armed)' if armed else 'absent (disarmed)'}")
        print(f"hook {RCS_HARMONY}: {'present' if self._exists(RCS_HARMONY) else 'absent'}")
        hooked = rcs is not None and BEGIN.encode() in rcs
        print(f"{RCS_LOCAL}: {'contains our block' if hooked else 'clean (no block)'}")
        conf = self._read(CONF)
        if conf is None:
            print(f"config {CONF}: absent")
        else:
            print(f"config {CONF} (token redacted):")
            for line in self.redact(conf.decode(errors="replace")).strip().splitlines():
                print(f"  {line}")
        if self._exists(DAEMON):
            print(f"daemon {DAEMON}: present, md5 {self._md5_on_device(DAEMON) or 'unreadable'}")
        else:
            print(f"daemon {DAEMON}: absent")
        n = self._read(COUNTER)
        print(f"strike counter: {n.decode().strip() if n else 'clear (no unclean boots pending)'}")
        if rcs is not None:
            ok, err = self._syntax_ok(RCS_LOCAL)
            print(f"{RCS_LOCAL} syntax: {'ok' if ok else 'BROKEN -- ' + err}")
        self._tail("hook log, last boot", LOG)
        self._tail("daemon log", DAEMON_LOG)
        print(
            "\narmed: our daemon replaces stock hal at boot"
            if armed
            else "\ndisarmed: the hub boots pure stock"
        )
        return 0

    def selftest(self, hook_src: Path | None = None, script_src: Path | None = None) -> int:
        """Dry-run the hook on the hub's own BusyBox sh, writing only under /tmp/hah-selftest."""
        hook = (hook_src or self.hook_src).read_bytes()
        script = (script_src or SELFTEST_SRC).read_bytes()
        self._run(["mkdir", "-p", SELFTEST_DIR], "create selftest dir")
        try:
            for name, data in (("rcS.harmony", hook), ("selftest.sh", script)):
                rc, _, err = self.r.run_input(["sh", "-c", f"cat > {SELFTEST_DIR}/{name}"], data)
                if rc != 0:
                    raise InstallError(f"upload {name} failed: {err.decode(errors='replace').strip()}")
            rc, out, err = self.r.run(["sh", f"{SELFTEST_DIR}/selftest.sh", f"{SELFTEST_DIR}/rcS.harmony"])
            print(out.decode(errors="replace").rstrip())
            if rc != 0:
                print(err.decode(errors="replace").rstrip(), file=sys.stderr)
        finally:
            self.r.run(["rm", "-rf", SELFTEST_DIR])
        return 0 if rc == 0 else 1

    def disarm(self) -> int:
        """Remove the marker and nothing else: next boot is pure stock."""
        if self._exists(MARKER):
            self._run(["rm", "-f", MARKER], "remove marker")
            print(f"disarmed: removed {MARKER} (next boot is pure stock)")
        else:
            print(f"{MARKER} already absent (already disarmed)")
        return 0

    def install(
        self,
        token: str,
        port: int,
        hub_id: str | None,
        *,
        rf_slot: int | None = None,
        rf: bool = False,
        expect_md5: str | None = None,
        stop_lua: bool = True,
        reboot_on_early_death: bool = True,
    ) -> int:
        if not TOKEN_RE.fullmatch(token or ""):
            raise InstallError("token must match [A-Za-z0-9._~+/=-]{1,256} (no whitespace or CR/LF)")
        if hub_id is not None and not HUB_ID_RE.fullmatch(hub_id):
            raise InstallError("hub id must match [A-Za-z0-9._-]{1,63}")
        if not 1 <= port <= 65535:
            raise InstallError("port must be 1..65535")
        if expect_md5 is None or not re.fullmatch(r"[0-9a-f]{32}", expect_md5):
            raise InstallError("--expect-md5 <md5 of the local hub build> is required")
        if rf_slot is not None and not 1 <= rf_slot <= 15:
            raise InstallError("--rf-slot must be 1..15 (the remote's HID++ device index)")
        if not self.hook_src.is_file():
            raise InstallError(f"hook source missing: {self.hook_src}")
        hook = self.hook_src.read_bytes()

        # --- preflight: refuse rather than half-install -------------------------
        # No escape hatch for a missing daemon. One existed "for the guard rehearsal" and
        # was useless for it: the hook deliberately leaves stock alone, spending no strike,
        # when the daemon is missing -- so such a rehearsal boots stock three times and
        # proves nothing. To rehearse the guard, install with a daemon that is PRESENT and
        # executable but exits non-zero (that is what T2 did on hardware, 2026-11-17).
        rc, _, _ = self.r.run(["test", "-x", DAEMON])
        if rc != 0:
            raise InstallError(
                f"{DAEMON} is missing or not executable -- run deploy_hub.py push first"
            )
        if not self._exists(NOWATCHDOG):
            raise InstallError(
                f"{NOWATCHDOG} is absent (stock watchdog enabled): with no hal/lua the watchdog would "
                "reboot the hub after every health mark, an unbounded loop. Refusing to arm."
            )
        got = self._md5_on_device(DAEMON)
        if got != expect_md5:
            raise InstallError(
                f"{DAEMON} md5 is {got or 'unreadable'}, expected {expect_md5}: "
                "refusing to arm a binary that is not the verified build (run deploy_hub.py push)"
            )
        local = self._read(RCS_LOCAL)
        if local is None:
            raise InstallError(f"{RCS_LOCAL} not found -- this is not the expected stock firmware")
        ok, err = self._syntax_ok(RCS_LOCAL)
        if not ok:
            raise InstallError(f"{RCS_LOCAL} does not parse before we touch it: {err}")

        # --- 1. back up rcS.local, on the device and to our hands ---------------
        stamp = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
        backup = f"{BACKUP_DIR}/rcS.local.{stamp}"
        self._run(["mkdir", "-p", BACKUP_DIR], "create backup dir")
        self._write(backup, local, "644")
        print(f"backed up {RCS_LOCAL} -> {backup} ({len(local)} B)")

        # --- 2. the hook itself: inert, nothing sources it yet ------------------
        self._write(RCS_HARMONY, hook, "755")
        ok, err = self._syntax_ok(RCS_HARMONY)
        if not ok:
            self._run(["rm", "-f", RCS_HARMONY], "remove bad hook")
            raise InstallError(f"{RCS_HARMONY} does not parse on the device: {err}")
        print(f"installed {RCS_HARMONY} ({len(hook)} B, md5 {md5_bytes(hook)[:8]})")

        # --- 3. config: parsed by the hook, never sourced -----------------------
        conf = f"token={token}\nport={port}\n" + (f"hub_id={hub_id}\n" if hub_id else "")
        if rf or rf_slot is not None:  # the slot is learned from the radio's link-up report unless given
            conf += "rf=spi\n" + (f"rf_slot={rf_slot}\n" if rf_slot is not None else "")
        if not stop_lua:
            conf += "stop_lua=0\n"
        if not reboot_on_early_death:
            conf += "reboot_on_early_death=0\n"
        self._write(CONF, conf.encode(), "600")
        print(f"wrote {CONF}")

        # --- 4. hook rcS.local, then prove it still parses ----------------------
        if BEGIN.encode() in local:
            print(f"{RCS_LOCAL} already contains our block; leaving it as is (idempotent)")
        else:
            body = local.decode(errors="replace")
            if not body.endswith("\n"):
                body += "\n"
            self._write(RCS_LOCAL, (body + BLOCK + "\n").encode(), "755", atomic=True)
            ok, err = self._syntax_ok(RCS_LOCAL)
            if not ok:
                # The dangerous case, and the whole reason for the backup.
                self._write(RCS_LOCAL, local, "755", atomic=True)
                raise InstallError(
                    f"{RCS_LOCAL} stopped parsing after our edit ({err}); "
                    f"restored the original and did NOT arm. Backup: {backup}"
                )
            print(f"appended our block to {RCS_LOCAL}; syntax ok")

        # --- 5. arm, last ------------------------------------------------------
        # A stale strike count from an earlier run must not make the first boot of a fresh
        # install count as already struck.
        self._run(["rm", "-f", COUNTER], "clear strike counter")
        self._write(MARKER, b"", "644")
        print(f"\nARMED: created {MARKER}")
        print(f"  next boot runs {DAEMON} instead of stock hal (DEMO_MODE=1)")
        print(f"  three unclean boots self-disable by removing {MARKER}")
        print(f"  to disable now: tools/install_boot_hook.py uninstall --yes"
              f"  (or on the hub: rm {MARKER})")  # fmt: skip
        return 0

    def uninstall(self, purge: bool = False) -> int:
        """Disarm first, then unpick. Safe to run against a partial install."""
        if self._exists(MARKER):
            self._run(["rm", "-f", MARKER], "remove marker")
            print(f"disarmed: removed {MARKER} (next boot is pure stock)")
        else:
            print(f"{MARKER} already absent (already disarmed)")

        local = self._read(RCS_LOCAL)
        if local is not None and BEGIN.encode() in local:
            # sed is present on the hub; `head`/`find`/`awk` are not.
            kept = []
            skipping = False
            for line in local.decode(errors="replace").splitlines():
                if line.strip() == BEGIN:
                    skipping = True
                    continue
                if line.strip() == END:
                    skipping = False
                    continue
                if not skipping:
                    kept.append(line)
            new = ("\n".join(kept).rstrip("\n") + "\n").encode()
            self._write(RCS_LOCAL, new, "755", atomic=True)
            ok, err = self._syntax_ok(RCS_LOCAL)
            if not ok:
                self._write(RCS_LOCAL, local, "755", atomic=True)
                raise InstallError(f"removing the block broke {RCS_LOCAL} ({err}); restored it")
            print(f"removed our block from {RCS_LOCAL}; syntax ok")
        else:
            print(f"{RCS_LOCAL} has no block of ours")

        for p in (RCS_HARMONY, COUNTER, PIDFILE):
            if self._exists(p):
                self._run(["rm", "-f", p], f"remove {p}")
                print(f"removed {p}")
        if purge:
            for p in (CONF, LOG):
                if self._exists(p):
                    self._run(["rm", "-f", p], f"remove {p}")
                    print(f"removed {p}")
        print("\nthe hub now boots pure stock")
        return 0


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter, allow_abbrev=False
    )
    ap.add_argument("--host", default="harmony-hub")
    ap.add_argument("--port", type=int, default=None, help="ssh port")
    ap.add_argument("--user", default="root")
    ap.add_argument("--key", default=None)
    ap.add_argument("--ssh-timeout", type=int, default=DEFAULT_SSH_TIMEOUT, metavar="SEC")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("status", help="[read-only] is the hook installed and armed?")
    p = sub.add_parser("install", help="[WRITE] install and arm the boot hook", allow_abbrev=False)
    p.add_argument("--yes", action="store_true")
    p.add_argument("--token-file", type=Path, help="file holding the WS token (or set HAH_TOKEN)")
    p.add_argument("--rf", action="store_true", help="enable the real radio (the remote's slot is learned from the link-up report)")
    p.add_argument("--rf-slot", type=int, default=None, help="enable the real radio and override the learned slot (1..15)")
    p.add_argument("--expect-md5", required=True, help="md5 of the local hub build; arm only if the device matches")
    p.add_argument("--no-reboot-on-early-death", action="store_true",
                   help="do not reboot when the daemon dies inside the 120 s health window")
    p.add_argument("--keep-lua", action="store_true", help="do not stop luaworks/luadraws at boot")
    p.add_argument("--hub-port", type=int, default=8099, help="daemon WS port (default %(default)s)")
    p.add_argument("--hub-id", default=None)
    p = sub.add_parser("disarm", help="[WRITE] remove only the marker; next boot is pure stock")
    p.add_argument("--yes", action="store_true")
    p = sub.add_parser("selftest", help="[WRITE /tmp only] dry-run the hook under the hub's BusyBox sh")
    p.add_argument("--yes", action="store_true")
    p = sub.add_parser("uninstall", help="[WRITE] disarm and remove the hook")
    p.add_argument("--yes", action="store_true")
    p.add_argument("--purge", action="store_true", help="also remove hub.conf and the boot log")
    return ap


def read_token(path: Path | None) -> str:
    """Token from a file or HAH_TOKEN, never from argv (visible in `ps` and shell history)."""
    try:
        tok = path.read_text().strip() if path else os.environ.get("HAH_TOKEN", "").strip()
    except OSError as e:
        raise InstallError(f"cannot read token file: {e.strerror}") from e
    if not tok:
        raise InstallError("no token: pass --token-file F or set HAH_TOKEN")
    return tok


def main(argv: list[str] | None = None, runner: Runner | None = None) -> int:
    ap = build_parser()
    a = ap.parse_args(argv)
    if a.cmd != "status" and not a.yes:
        print(
            f"{a.cmd} writes to /etc/init.d on the real hub; re-run with --yes to confirm",
            file=sys.stderr,
        )
        return 2
    ssh = runner or HubSsh(a.host, a.user, a.key, timeout=a.ssh_timeout, port=a.port)
    inst = HookInstaller(ssh)
    try:
        match a.cmd:
            case "status":
                return inst.status()
            case "install":
                return inst.install(
                    read_token(a.token_file),
                    a.hub_port,
                    a.hub_id,
                    rf_slot=a.rf_slot,
                    rf=a.rf,
                    expect_md5=a.expect_md5,
                    stop_lua=not a.keep_lua,
                    reboot_on_early_death=not a.no_reboot_on_early_death,
                )
            case "disarm":
                return inst.disarm()
            case "selftest":
                return inst.selftest()
            case "uninstall":
                return inst.uninstall(a.purge)
    except InstallError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
