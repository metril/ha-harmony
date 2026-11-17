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
COUNTER = "/cache/ha-harmony-boot.n"
PIDFILE = "/cache/ha-harmony.pid"
LOG = "/cache/ha-harmony.log"

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


class InstallError(RuntimeError):
    """Refused or failed; the message is shown to the user verbatim."""


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

    def _write(self, path: str, data: bytes, mode: str) -> None:
        """Write via a temp file then `mv`, so a half-written boot file never exists.

        `mv` within the same filesystem is atomic; /etc is the unionfs rw branch (mtd4),
        the same branch /tmp is not -- hence the temp file goes next to the target.
        """
        tmp = f"{path}.hah-tmp"
        rc, _, err = self.r.run_input(["sh", "-c", f"cat > {tmp}"], data)
        if rc != 0:
            raise InstallError(f"writing {tmp} failed: {err.decode(errors='replace').strip()}")
        self._run(["chmod", mode, tmp], f"chmod {tmp}")
        self._run(["mv", tmp, path], f"mv into place {path}")
        got = self._read(path)
        if got is None or md5_bytes(got) != md5_bytes(data):
            raise InstallError(f"verify failed for {path}: content differs after write")

    def _syntax_ok(self, path: str) -> tuple[bool, str]:
        rc, _, err = self.r.run(["sh", "-n", path])
        return rc == 0, err.decode(errors="replace").strip()

    # --- commands -----------------------------------------------------------------

    def status(self) -> int:
        rcs = self._read(RCS_LOCAL)
        armed = self._exists(MARKER)
        print(f"marker {MARKER}: {'PRESENT (armed)' if armed else 'absent (disarmed)'}")
        print(f"hook {RCS_HARMONY}: {'present' if self._exists(RCS_HARMONY) else 'absent'}")
        hooked = rcs is not None and BEGIN.encode() in rcs
        print(f"{RCS_LOCAL}: {'contains our block' if hooked else 'clean (no block)'}")
        print(f"config {CONF}: {'present' if self._exists(CONF) else 'absent'}")
        print(f"daemon {DAEMON}: {'present' if self._exists(DAEMON) else 'absent'}")
        n = self._read(COUNTER)
        print(f"strike counter: {n.decode().strip() if n else 'clear (no unclean boots pending)'}")
        if rcs is not None:
            ok, err = self._syntax_ok(RCS_LOCAL)
            print(f"{RCS_LOCAL} syntax: {'ok' if ok else 'BROKEN -- ' + err}")
        log = self._read(LOG)
        if log:
            tail = log.decode(errors="replace").strip().splitlines()[-8:]
            print("--- last boot log ---")
            for line in tail:
                print(f"  {line}")
        print(
            "\narmed: our daemon replaces stock hal at boot"
            if armed
            else "\ndisarmed: the hub boots pure stock"
        )
        return 0

    def install(self, token: str, port: int, hub_id: str | None, require_daemon: bool = True) -> int:
        if not self.hook_src.is_file():
            raise InstallError(f"hook source missing: {self.hook_src}")
        hook = self.hook_src.read_bytes()

        # --- preflight: refuse rather than half-install -------------------------
        if require_daemon:
            rc, _, _ = self.r.run(["test", "-x", DAEMON])
            if rc != 0:
                raise InstallError(
                    f"{DAEMON} is missing or not executable -- run deploy_hub.py push first "
                    "(or pass --allow-missing-daemon for the T2 guard rehearsal)"
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
        self._write(CONF, conf.encode(), "600")
        print(f"wrote {CONF}")

        # --- 4. hook rcS.local, then prove it still parses ----------------------
        if BEGIN.encode() in local:
            print(f"{RCS_LOCAL} already contains our block; leaving it as is (idempotent)")
        else:
            body = local.decode(errors="replace")
            if not body.endswith("\n"):
                body += "\n"
            self._write(RCS_LOCAL, (body + BLOCK + "\n").encode(), "755")
            ok, err = self._syntax_ok(RCS_LOCAL)
            if not ok:
                # The dangerous case, and the whole reason for the backup.
                self._write(RCS_LOCAL, local, "755")
                raise InstallError(
                    f"{RCS_LOCAL} stopped parsing after our edit ({err}); "
                    f"restored the original and did NOT arm. Backup: {backup}"
                )
            print(f"appended our block to {RCS_LOCAL}; syntax ok")

        # --- 5. arm, last ------------------------------------------------------
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
            self._write(RCS_LOCAL, new, "755")
            ok, err = self._syntax_ok(RCS_LOCAL)
            if not ok:
                self._write(RCS_LOCAL, local, "755")
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
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--host", default="harmony-hub")
    ap.add_argument("--port", type=int, default=None, help="ssh port")
    ap.add_argument("--user", default="root")
    ap.add_argument("--key", default=None)
    ap.add_argument("--ssh-timeout", type=int, default=DEFAULT_SSH_TIMEOUT, metavar="SEC")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("status", help="[read-only] is the hook installed and armed?")
    p = sub.add_parser("install", help="[WRITE] install and arm the boot hook")
    p.add_argument("--yes", action="store_true")
    p.add_argument("--token", required=True, help="WS auth token the daemon will require")
    p.add_argument("--hub-port", type=int, default=8099, help="daemon WS port (default %(default)s)")
    p.add_argument("--hub-id", default=None)
    p.add_argument(
        "--allow-missing-daemon",
        action="store_true",
        help="install without a working daemon -- for the T2 guard rehearsal only",
    )
    p = sub.add_parser("uninstall", help="[WRITE] disarm and remove the hook")
    p.add_argument("--yes", action="store_true")
    p.add_argument("--purge", action="store_true", help="also remove hub.conf and the boot log")
    return ap


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
                    a.token, a.hub_port, a.hub_id, require_daemon=not a.allow_missing_daemon
                )
            case "uninstall":
                return inst.uninstall(a.purge)
    except InstallError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
