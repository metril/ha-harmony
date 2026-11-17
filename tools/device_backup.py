#!/usr/bin/env python3
"""Verified full-partition backup of a rooted Harmony Hub / Elite remote over SSH.

Read-only on the device: only `cat`, `dd if=...`, `sha256sum`/`md5sum`, `test`, `uname`.
Every image is hashed on the device and locally over the received bytes; mismatch => `*.CORRUPT`.
`dd` is preferred; a BusyBox without it is read with `cat` (read-only `/dev/mtdNro` or `/dev/mtdrN` if present).
`--verify <dir>` re-checks a finished backup offline (no SSH).
"""

import argparse
import hashlib
import json
import os
import re
import select
import shlex
import subprocess
import sys
import tempfile
from collections.abc import Callable
from dataclasses import dataclass
from datetime import UTC, datetime
from pathlib import Path
from typing import Any, Protocol

DEFAULT_BS = 4096
DEFAULT_SSH_TIMEOUT = 15  # seconds; ConnectTimeout and ServerAliveInterval
SERVER_ALIVE_COUNT = 3
DEFAULT_RUN_TIMEOUT = 3600.0  # hard cap for one non-streaming remote command (e.g. a device hash)
UTILITIES = ("dd", "cat", "sha256sum", "md5sum", "find", "tar")
MTD_RE = re.compile(r'^mtd(\d+):\s+([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+"(.*)"\s*$')
IGNORED_PREFIXES = ("ram", "loop", "zram", "nbd", "sr", "fd")
DETECT_SCRIPT = (
    "for u in " + " ".join(UTILITIES) + "; do command -v $u >/dev/null 2>&1 && echo $u; done"
)
# Used only when `command -v` found nothing (BusyBox without ASH_CMDCMD): run each tool harmlessly.
PROBES = {
    "dd": "dd if=/dev/null bs=1 >/dev/null 2>&1 || which dd >/dev/null 2>&1",
    "cat": "cat /dev/null >/dev/null 2>&1 || which cat >/dev/null 2>&1",
    "sha256sum": "sha256sum </dev/null >/dev/null 2>&1 || which sha256sum >/dev/null 2>&1",
    "md5sum": "md5sum </dev/null >/dev/null 2>&1 || which md5sum >/dev/null 2>&1",
    "find": "which find >/dev/null 2>&1",
    "tar": "which tar >/dev/null 2>&1",
}
RC_RE = re.compile(rb"(?m)^rc=(\d+)\s*$")
RECORDS_RE = re.compile(rb"(?m)^(\d+)\+(\d+) records (in|out)\s*$")


class SshRunner:
    """Runs remote commands via the ssh binary. `argv` is the remote command.

    A dead link is caught by ssh itself (ConnectTimeout, ServerAlive*); a live link whose remote
    command stops producing data is caught by `idle_timeout` (streams) / `run_timeout` (others).
    """

    def __init__(
        self,
        host: str,
        user: str = "root",
        key: str | None = None,
        timeout: int = DEFAULT_SSH_TIMEOUT,
        idle_timeout: float | None = None,
        run_timeout: float = DEFAULT_RUN_TIMEOUT,
        port: int | None = None,
    ) -> None:
        self.host, self.user, self.key, self.timeout = host, user, key, timeout
        self.port = port
        self.idle_timeout = idle_timeout if idle_timeout is not None else timeout * 4
        self.run_timeout = run_timeout

    def _ssh(self, argv: list[str]) -> list[str]:
        cmd = [
            "ssh", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=accept-new",
            "-o", f"ConnectTimeout={self.timeout}",
            "-o", f"ServerAliveInterval={self.timeout}",
            "-o", f"ServerAliveCountMax={SERVER_ALIVE_COUNT}",
        ]  # fmt: skip
        if self.key:
            cmd += ["-i", os.path.expanduser(self.key)]
        if self.port:
            cmd += ["-p", str(self.port)]
        return [*cmd, f"{self.user}@{self.host}", shlex.join(argv)]

    def run(self, argv: list[str]) -> tuple[int, bytes, bytes]:
        try:
            p = subprocess.run(
                self._ssh(argv), capture_output=True, check=False, timeout=self.run_timeout
            )
        except subprocess.TimeoutExpired:
            return 255, b"", f"timed out after {self.run_timeout:g}s".encode()
        return p.returncode, p.stdout, p.stderr

    def run_stream(self, argv: list[str], sink: Callable[[bytes], None]) -> tuple[int, bytes]:
        with (
            tempfile.TemporaryFile() as errf,
            subprocess.Popen(self._ssh(argv), stdout=subprocess.PIPE, stderr=errf) as p,
        ):
            assert p.stdout is not None
            try:
                while True:
                    if not select.select([p.stdout], [], [], self.idle_timeout)[0]:
                        p.kill()
                        return 255, f"stalled: no data for {self.idle_timeout:g}s".encode()
                    if not (chunk := os.read(p.stdout.fileno(), 1 << 20)):
                        break
                    sink(chunk)
                try:
                    rc = p.wait(timeout=self.idle_timeout)
                except subprocess.TimeoutExpired:
                    p.kill()
                    return 255, b"stalled: no exit after EOF"
                errf.seek(0)
                return rc, errf.read()
            finally:
                if p.poll() is None:
                    p.kill()


class Runner(Protocol):
    def run(self, argv: list[str]) -> tuple[int, bytes, bytes]: ...
    def run_stream(self, argv: list[str], sink: Callable[[bytes], None]) -> tuple[int, bytes]: ...


@dataclass
class Partition:
    name: str
    index: int
    size: int
    erasesize: int
    device: str  # block device path
    char_device: str | None = None  # raw mtd char device candidate

    @property
    def bs(self) -> int:
        return self.erasesize or DEFAULT_BS


def parse_proc_mtd(text: str) -> list[Partition]:
    parts = []
    for line in text.splitlines():
        m = MTD_RE.match(line.strip())
        if m:
            idx = int(m[1])
            parts.append(
                Partition(
                    name=m[4],
                    index=idx,
                    size=int(m[2], 16),
                    erasesize=int(m[3], 16),
                    device=f"/dev/mtdblock{idx}",
                    char_device=f"/dev/mtd{idx}",
                )
            )
    return parts


def parse_proc_partitions(text: str) -> list[Partition]:
    """`major minor #blocks name` (1 KiB blocks). Skips ram/loop-style and nested names."""
    parts = []
    for line in text.splitlines():
        f = line.split()
        if len(f) != 4 or not f[0].isdigit() or not f[2].isdigit():
            continue
        name = f[3]
        if "/" in name or name.startswith(IGNORED_PREFIXES):
            continue
        parts.append(
            Partition(name=name, index=len(parts), size=int(f[2]) * 1024, erasesize=0,
                      device=f"/dev/{name}")
        )  # fmt: skip
    return parts


def safe_name(name: str) -> str:
    return re.sub(r"[^A-Za-z0-9._-]+", "_", name).strip("_") or "unnamed"


def parse_hash(out: bytes, algo: str) -> str | None:
    toks = out.decode("ascii", "replace").split()
    want = 64 if algo == "sha256" else 32
    if toks and len(toks[0]) == want and re.fullmatch(r"[0-9a-fA-F]+", toks[0]):
        return toks[0].lower()
    return None


def file_hash(path: Path, algo: str) -> str:
    h = hashlib.new(algo)
    with path.open("rb") as f:
        while chunk := f.read(1 << 20):
            h.update(chunk)
    return h.hexdigest()


class Backup:
    def __init__(self, runner: Runner, target: str, host: str, out_dir: Path) -> None:
        self.r, self.target, self.host, self.out = runner, target, host, out_dir
        self.errors: list[str] = []
        self.warnings: list[str] = []
        self.manifest: dict[str, Any] = {}
        self.read_method = "dd"

    def sh(self, script: str) -> tuple[int, bytes, bytes]:
        return self.r.run(["sh", "-c", script])

    def discover(self) -> tuple[str, list[Partition]]:
        rc, out, _ = self.r.run(["cat", "/proc/mtd"])
        text = out.decode("utf-8", "replace")
        self.manifest["proc_mtd"] = text if rc == 0 else ""
        parts = parse_proc_mtd(text) if rc == 0 else []
        if parts:
            return "/proc/mtd", parts
        if self.target == "hub":
            raise RuntimeError("hub: /proc/mtd missing or has no mtd entries")
        rc, out, _ = self.r.run(["cat", "/proc/partitions"])
        parts = parse_proc_partitions(out.decode("utf-8", "replace")) if rc == 0 else []
        if not parts:
            raise RuntimeError("remote: neither /proc/mtd nor /proc/partitions yielded partitions")
        self.manifest["proc_partitions"] = out.decode("utf-8", "replace")
        return "/proc/partitions", parts

    def detect_utils(self) -> dict[str, bool]:
        _, out, _ = self.sh(DETECT_SCRIPT)
        have = set(out.decode().split())
        if not have:  # no `command -v` builtin? probe the tools directly before giving up
            have = {u for u, probe in PROBES.items() if self.sh(probe)[0] == 0}
        return {u: u in have for u in UTILITIES}

    def has_char(self, p: Partition) -> bool:
        if not p.char_device:
            return False
        return self.sh(f"test -c {shlex.quote(p.char_device)}")[0] == 0

    def read_argv(self, dev: str, bs: int) -> list[str]:
        return ["dd", f"if={dev}", f"bs={bs}"] if self.read_method == "dd" else ["cat", dev]

    def nodes(self, p: Partition) -> tuple[str, str | None]:
        """(primary device, extra raw char device). `cat` mode prefers a read-only raw node (mtdNro, or mtdrN as on the Harmony Hub)."""
        if self.read_method == "cat" and p.char_device:
            for ro in (f"{p.char_device}ro", f"/dev/mtdr{p.index}"):
                if self.sh(f"test -c {shlex.quote(ro)}")[0] == 0:
                    return ro, None
        return p.device, (p.char_device if self.has_char(p) else None)

    def dest(self, p: Partition, raw: bool = False) -> Path:
        stem = f"{p.index:02d}_{safe_name(p.name)}" if p.char_device else safe_name(p.name)
        return self.out / f"{stem}{'.raw' if raw else ''}.img"

    def device_hash(self, dev: str, size: int, bs: int, tool: str) -> tuple[str | None, str]:
        """(hash, "") on a verified-complete device read, else (None, reason).

        The reader's own exit status is echoed to stderr (a plain pipeline reports only the hash
        tool's status) and, for dd, its record counts must cover exactly `size` bytes.
        """
        src = shlex.join(self.read_argv(dev, bs))
        rc, out, err = self.sh(f'{{ {src}; echo "rc=$?" >&2; }} | {tool}')
        tail = err.decode("utf-8", "replace")[-200:]
        if rc != 0:
            return None, f"hash command exited {rc}: {tail}"
        m = RC_RE.search(err)
        if not m or int(m[1]) != 0:
            return None, f"device read did not exit 0 during hash pass: {tail}"
        if self.read_method == "dd":
            want = (size // bs, 1 if size % bs else 0)
            counts = {k: (int(a), int(b)) for a, b, k in RECORDS_RE.findall(err)}
            if counts.get(b"in") != want or counts.get(b"out") != want:
                return None, f"dd records {counts} != expected {want} in hash pass: {tail}"
        h = parse_hash(out, "sha256" if tool == "sha256sum" else "md5")
        return (h, "") if h else (None, "device hash output unparseable")

    def image(self, dev: str, size: int, bs: int, tool: str, final: Path) -> dict[str, Any]:
        algo = "sha256" if tool == "sha256sum" else "md5"
        rec: dict[str, Any] = {
            "device_path": dev, "file": str(final), "size_bytes": size, "bytes_received": 0,
            "hash_algo": algo, "device_hash": None, "local_hash": None,
            "utility": tool, "status": "error",
            "read_method": self.read_method, "read_node": dev,
        }  # fmt: skip
        part = final.with_name(final.name + ".part")
        if size <= 0:
            rec["error"] = f"declared size {size} is not positive; refusing to record an image"
            rec["file"] = None
            return rec
        try:
            if final.exists():
                return self._existing(rec, final, dev, size, bs, tool, algo)
            h, count = hashlib.new(algo), 0
            with part.open("wb") as fh:

                def sink(b: bytes) -> None:
                    nonlocal count
                    fh.write(b)
                    h.update(b)
                    count += len(b)

                rc, err = self.r.run_stream(self.read_argv(dev, bs), sink)
            rec["bytes_received"], rec["local_hash"] = count, h.hexdigest()
            if rc == 0 and err:
                rec["device_stderr"] = err.decode("utf-8", "replace")[-500:]
            if rc != 0:
                rec["status"] = "error"
                rec["error"] = (
                    f"{self.read_method} exited {rc}: {err.decode('utf-8', 'replace')[-200:]}"
                )
            elif count != size:
                rec["status"] = "short_read"
                rec["error"] = f"received {count} bytes, expected {size}"
            else:
                rec["device_hash"], why = self.device_hash(dev, size, bs, tool)
                if rec["device_hash"] is None:
                    rec["status"] = "hash_failed"
                    rec["error"] = f"device hash pass failed (image may be intact): {why}"
                elif rec["device_hash"] != rec["local_hash"]:
                    rec["status"] = "corrupt"
                    rec["error"] = "device and local hashes differ"
                    corrupt = final.with_name(final.name + ".CORRUPT")
                    os.replace(part, corrupt)
                    rec["file"] = str(corrupt)
                else:
                    os.replace(part, final)
                    rec["status"] = "ok"
        except OSError as e:
            rec["error"] = f"{type(e).__name__}: {e}"
        if rec["status"] not in ("ok", "corrupt"):
            rec["file"] = str(part) if part.exists() else None
        return rec

    def _existing(
        self, rec: dict[str, Any], final: Path, dev: str, size: int, bs: int, tool: str, algo: str
    ) -> dict[str, Any]:
        rec["bytes_received"] = final.stat().st_size
        if rec["bytes_received"] == size:
            rec["local_hash"] = file_hash(final, algo)
            rec["device_hash"], why = self.device_hash(dev, size, bs, tool)
            if rec["device_hash"] is None:
                rec["status"] = "hash_failed"
                rec["error"] = f"device hash pass failed (stored image may be intact): {why}"
                return rec
            if rec["device_hash"] == rec["local_hash"]:
                rec["status"] = "skipped"
                return rec
        rec["status"] = "conflict"
        rec["error"] = f"{final} exists and is not a matching complete image; move it aside"
        return rec

    def run_partition(self, p: Partition, tool: str) -> dict[str, Any]:
        entry: dict[str, Any] = {"name": p.name, "index": p.index, "erasesize": p.erasesize}
        dev, raw_dev = self.nodes(p)
        entry.update(self.image(dev, p.size, p.bs, tool, self.dest(p)))
        statuses = [entry["status"]]
        if raw_dev:
            entry["raw"] = self.image(raw_dev, p.size, p.bs, tool, self.dest(p, raw=True))
            statuses.append(entry["raw"]["status"])
        bad = [s for s in statuses if s not in ("ok", "skipped")]
        entry["status"] = bad[0] if bad else ("skipped" if set(statuses) == {"skipped"} else "ok")
        return entry

    def plan(self, parts: list[Partition], utils: dict[str, bool]) -> list[dict[str, Any]]:
        plan = []
        for p in parts:
            dev, raw_dev = self.nodes(p)
            plan.append({
                "name": p.name, "index": p.index, "device_path": dev, "size_bytes": p.size,
                "bs": p.bs, "raw_device": raw_dev, "read_method": self.read_method,
                "file": str(self.dest(p)),
            })  # fmt: skip
        return plan

    def execute(self, only: list[str], dry_run: bool) -> int:
        m = self.manifest
        m.update(
            target=self.target, host=self.host, timestamp=datetime.now(UTC).isoformat(),
            status="failed", partitions=[], warnings=self.warnings, errors=self.errors,
            dry_run=dry_run,
        )  # fmt: skip
        try:
            rc, out, err = self.r.run(["uname", "-a"])
            if rc == 255:  # ssh itself failed (refused, timeout, auth, host key): say so
                raise RuntimeError(
                    f"ssh connection failed: {err.decode('utf-8', 'replace').strip()}"
                )
            m["uname"] = out.decode("utf-8", "replace").strip()
            source, parts = self.discover()
            m["discovery_source"] = source
            m["utilities"] = utils = self.detect_utils()
            if only:
                keep = lambda p: {p.name, str(p.index), f"mtd{p.index}"} & set(only)
                missing = set(only) - {
                    k for p in parts for k in (p.name, str(p.index), f"mtd{p.index}")
                }
                if missing:
                    raise RuntimeError(f"--only matched no partition: {sorted(missing)}")
                parts = [p for p in parts if keep(p)]
            self.read_method = m["read_method"] = self._pick_reader(utils)
            tool = self._pick_tool(utils)
            m["hash_utility"] = tool
            m["plan"] = self.plan(parts, utils)
            if dry_run:
                m["status"] = "dry-run"
                return 0
            self.out.mkdir(parents=True, exist_ok=True)
            for p in parts:
                m["partitions"].append(self.run_partition(p, tool))
                self.write_manifest()  # keep device hashes if the run dies mid-way
            bad = [x for x in m["partitions"] if x["status"] not in ("ok", "skipped")]
            for x in bad:
                self.errors.append(f"{x['name']}: {x['status']}")
            m["status"] = "failed" if bad else "ok"
        except RuntimeError as e:
            self.errors.append(str(e))
        except OSError as e:
            self.errors.append(f"{type(e).__name__}: {e}")
        except BaseException as e:  # incl. KeyboardInterrupt: never lose finished partitions
            self.errors.append(f"interrupted by {type(e).__name__}: {e}")
            self._flush(dry_run)
            if not isinstance(e, Exception):
                raise
        return 0 if m["status"] in ("ok", "dry-run") else 1

    def _flush(self, dry_run: bool) -> None:
        if dry_run:
            return
        try:
            self.write_manifest()
        except OSError as e:
            self.errors.append(f"could not write manifest: {e}")

    def _pick_reader(self, utils: dict[str, bool]) -> str:
        if utils["dd"]:
            return "dd"
        if utils["cat"]:
            self.warnings.append("dd missing on device; reading partitions with cat")
            return "cat"
        raise RuntimeError("required utility missing on device: dd (and cat)")

    def _pick_tool(self, utils: dict[str, bool]) -> str:
        if utils["sha256sum"]:
            return "sha256sum"
        if utils["md5sum"]:
            self.warnings.append(
                "WARNING: sha256sum missing on device; falling back to WEAKER md5sum"
            )
            return "md5sum"
        raise RuntimeError("required utility missing on device: sha256sum (and md5sum)")

    def write_manifest(self) -> Path:
        self.out.mkdir(parents=True, exist_ok=True)
        path = self.out / "manifest.json"
        tmp = path.with_name(path.name + ".tmp")
        tmp.write_text(json.dumps(self.manifest, indent=2, sort_keys=True) + "\n")
        os.replace(tmp, path)
        return path


def print_plan(m: dict[str, Any]) -> None:
    print(f"target={m['target']} host={m['host']} source={m.get('discovery_source')}")
    print(f"uname: {m.get('uname')}")
    print("utilities: " + ", ".join(f"{k}={'yes' if v else 'NO'}" for k, v in
                                   m.get("utilities", {}).items()))  # fmt: skip
    for w in m["warnings"]:
        print(w, file=sys.stderr)
    for p in m.get("plan", []):
        raw = f" raw={p['raw_device']}" if p["raw_device"] else ""
        print(f"  [{p['index']}] {p['name']!r}: {p['size_bytes']} B {p['device_path']}{raw}"
              f" -> {p['file']}")  # fmt: skip
    for e in m["errors"]:
        print(f"ERROR: {e}", file=sys.stderr)


MAGICS = (
    (b"hsqs", "squashfs"), (b"sqsh", "squashfs"), (b"\x85\x19", "jffs2"),
    (b"\x19\x85", "jffs2"), (b"UBI#", "ubi"), (b"\x27\x05\x19\x56", "uboot"),
    (b"\x1f\x8b", "kernel"),
)  # fmt: skip
ZIMAGE_MAGIC = b"\x18\x28\x6f\x01"  # ARM zImage, little-endian, at offset 0x24


def detect_magic(head: bytes) -> str:
    """Best-effort filesystem/image type from the first bytes; never an error."""
    for magic, name in MAGICS:
        if head.startswith(magic):
            return name
    return "kernel" if head[0x24:0x28] == ZIMAGE_MAGIC else "unknown"


def _check_image(
    d: Path, rec: dict[str, Any], label: str, res: dict[str, Any], seen: set[str]
) -> None:
    """Verify one manifest image record (a partition or its `raw` twin) into `res`."""
    out: dict[str, Any] = {"partition": label, "file": None, "status": "ok", "magic": None}
    res["images"].append(out)

    def fail(msg: str, status: str = "error") -> None:
        out["status"] = status
        res["errors"].append(f"{label}: {msg}")

    if rec.get("status") not in ("ok", "skipped") or not rec.get("file"):
        return fail(f"manifest records status {rec.get('status')!r}, no verified image")
    name = Path(rec["file"]).name
    out["file"] = name
    seen.add(name)
    path = d / name
    if not path.is_file():
        return fail(f"image {name} is missing", "missing")
    size, want = path.stat().st_size, rec.get("size_bytes")
    out.update(size=size, expected_size=want)
    if not isinstance(want, int) or want <= 0 or size == 0:
        return fail(f"{name} has zero/invalid size (manifest {want}, file {size})", "size_mismatch")
    with path.open("rb") as f:
        out["magic"] = detect_magic(f.read(64))
    if size != want:
        fail(f"{name} size {size} != manifest {want}", "size_mismatch")
    got = file_hash(path, rec["hash_algo"])
    out["hash"] = got
    for key in ("local_hash", "device_hash"):
        if got != rec.get(key):
            fail(f"{name} {rec['hash_algo']} differs from manifest {key}", "hash_mismatch")


def verify_dir(d: Path) -> dict[str, Any]:
    """Offline re-check of a finished backup directory; `ok` is False on any error."""
    res: dict[str, Any] = {"dir": str(d), "ok": False, "images": [], "errors": [], "warnings": []}
    try:
        manifest = json.loads((d / "manifest.json").read_text())
        parts = manifest["partitions"]
    except (OSError, ValueError, KeyError) as e:
        res["errors"].append(f"cannot read manifest.json: {type(e).__name__}: {e}")
        return res
    if not parts:
        res["errors"].append("manifest lists no partitions (dry-run or failed discovery?)")
    seen: set[str] = set()
    for p in parts:
        _check_image(d, p, p.get("name", "?"), res, seen)
        if "raw" in p:
            _check_image(d, p["raw"], f"{p.get('name', '?')} (raw)", res, seen)
    for leftover in sorted(d.glob("*.part")) + sorted(d.glob("*.CORRUPT")):
        res["errors"].append(f"leftover {leftover.name}")
    for extra in sorted(d.glob("*.img")):
        if extra.name not in seen:
            res["warnings"].append(f"extra image not in manifest: {extra.name}")
    res["ok"] = not res["errors"]
    return res


def print_verify(res: dict[str, Any]) -> None:
    print(f"verify {res['dir']}")
    for i in res["images"]:
        print(f"  {i['status'].upper():<13} {i['partition']}: {i['file']} magic={i['magic']}")
    for w in res["warnings"]:
        print(f"WARNING: {w}", file=sys.stderr)
    for e in res["errors"]:
        print(f"ERROR: {e}", file=sys.stderr)
    print("RESULT: " + ("OK" if res["ok"] else "FAILED"))


def main(argv: list[str] | None = None, runner: Runner | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host")
    ap.add_argument("--port", type=int, default=None, help="ssh port (default: ssh's own)")
    ap.add_argument("--user", default="root")
    ap.add_argument("--key", default="~/.ssh/harmony_owner_ed25519")
    ap.add_argument("--target", choices=["hub", "remote"])
    ap.add_argument("--out-dir", type=Path)
    ap.add_argument("--ssh-timeout", type=int, default=DEFAULT_SSH_TIMEOUT, metavar="SEC",
                    help="ssh ConnectTimeout/ServerAliveInterval (default %(default)s)")  # fmt: skip
    ap.add_argument("--idle-timeout", type=float, default=None, metavar="SEC",
                    help="kill a stream with no data for SEC (default 4 x --ssh-timeout)")  # fmt: skip
    ap.add_argument("--verify", type=Path, metavar="DIR", help="offline re-check of a backup dir")
    ap.add_argument("--json", action="store_true", help="with --verify: machine-readable output")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--only", action="append", default=[], help="partition name/index; repeatable")
    a = ap.parse_args(argv)
    if a.verify:
        res = verify_dir(a.verify)
        if a.json:
            print(json.dumps(res, indent=2))
        else:
            print_verify(res)
        return 0 if res["ok"] else 1
    if not (a.host and a.target and a.out_dir):
        ap.error("--host, --target and --out-dir are required unless --verify is given")
    ssh = runner or SshRunner(
        a.host,
        a.user,
        a.key,
        timeout=a.ssh_timeout,
        idle_timeout=a.idle_timeout,
        port=a.port,
    )
    b = Backup(ssh, a.target, a.host, a.out_dir)
    try:
        rc = b.execute(a.only, a.dry_run)
    except KeyboardInterrupt:  # execute() already flushed the manifest
        print("interrupted: partial *.part files left; re-run or inspect with --verify",
              file=sys.stderr)  # fmt: skip
        return 130
    if a.dry_run:
        print_plan(b.manifest)
        return rc
    path = b.write_manifest()
    for w in b.warnings:
        print(w, file=sys.stderr)
    for p in b.manifest["partitions"]:
        print(f"{p['name']}: {p['status']}")
    for e in b.errors:
        print(f"ERROR: {e}", file=sys.stderr)
    print(f"manifest: {path} status={b.manifest['status']}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
