#!/usr/bin/env python3
"""Verified full-partition backup of a rooted Harmony Hub / Elite remote over SSH.

Read-only on the device: only `cat`, `dd if=...`, `sha256sum`/`md5sum`, `test`, `uname`.
Every image is hashed on the device and locally over the received bytes; mismatch => `*.CORRUPT`.
"""

import argparse
import hashlib
import json
import os
import re
import shlex
import subprocess
import sys
from collections.abc import Callable
from dataclasses import dataclass
from datetime import UTC, datetime
from pathlib import Path
from typing import Any, Protocol

DEFAULT_BS = 4096
UTILITIES = ("dd", "sha256sum", "md5sum", "find", "tar")
MTD_RE = re.compile(r'^mtd(\d+):\s+([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+"(.*)"\s*$')
IGNORED_PREFIXES = ("ram", "loop", "zram", "nbd", "sr", "fd")
DETECT_SCRIPT = (
    "for u in " + " ".join(UTILITIES) + "; do command -v $u >/dev/null 2>&1 && echo $u; done"
)


class SshRunner:
    """Runs remote commands via the ssh binary. `argv` is the remote command."""

    def __init__(self, host: str, user: str = "root", key: str | None = None) -> None:
        self.host, self.user, self.key = host, user, key

    def _ssh(self, argv: list[str]) -> list[str]:
        cmd = ["ssh", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=accept-new"]
        if self.key:
            cmd += ["-i", os.path.expanduser(self.key)]
        return [*cmd, f"{self.user}@{self.host}", shlex.join(argv)]

    def run(self, argv: list[str]) -> tuple[int, bytes, bytes]:
        p = subprocess.run(self._ssh(argv), capture_output=True, check=False)
        return p.returncode, p.stdout, p.stderr

    def run_stream(self, argv: list[str], sink: Callable[[bytes], None]) -> tuple[int, bytes]:
        with subprocess.Popen(self._ssh(argv), stdout=subprocess.PIPE, stderr=subprocess.PIPE) as p:
            assert p.stdout is not None and p.stderr is not None
            while chunk := p.stdout.read(1 << 20):
                sink(chunk)
            err = p.stderr.read()
            return p.wait(), err


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
        return {u: u in have for u in UTILITIES}

    def has_char(self, p: Partition) -> bool:
        if not p.char_device:
            return False
        return self.sh(f"test -c {shlex.quote(p.char_device)}")[0] == 0

    def dest(self, p: Partition, raw: bool = False) -> Path:
        stem = f"{p.index:02d}_{safe_name(p.name)}" if p.char_device else safe_name(p.name)
        return self.out / f"{stem}{'.raw' if raw else ''}.img"

    def device_hash(self, dev: str, bs: int, tool: str) -> str | None:
        rc, out, _ = self.sh(f"dd if={shlex.quote(dev)} bs={bs} 2>/dev/null | {tool}")
        return parse_hash(out, "sha256" if tool == "sha256sum" else "md5") if rc == 0 else None

    def image(self, dev: str, size: int, bs: int, tool: str, final: Path) -> dict[str, Any]:
        algo = "sha256" if tool == "sha256sum" else "md5"
        rec: dict[str, Any] = {
            "device_path": dev, "file": str(final), "size_bytes": size, "bytes_received": 0,
            "hash_algo": algo, "device_hash": None, "local_hash": None,
            "utility": tool, "status": "error",
        }  # fmt: skip
        part = final.with_name(final.name + ".part")
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

                rc, err = self.r.run_stream(["dd", f"if={dev}", f"bs={bs}"], sink)
            rec["bytes_received"], rec["local_hash"] = count, h.hexdigest()
            if rc != 0:
                rec["status"] = "error"
                rec["error"] = f"dd exited {rc}: {err.decode('utf-8', 'replace')[-200:]}"
            elif count != size:
                rec["status"] = "short_read"
                rec["error"] = f"received {count} bytes, expected {size}"
            else:
                rec["device_hash"] = self.device_hash(dev, bs, tool)
                if rec["device_hash"] is None:
                    rec["error"] = "could not obtain device hash"
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
        if rec["status"] != "ok" and rec["status"] != "corrupt":
            rec["file"] = str(part) if part.exists() else None
        return rec

    def _existing(
        self, rec: dict[str, Any], final: Path, dev: str, size: int, bs: int, tool: str, algo: str
    ) -> dict[str, Any]:
        rec["bytes_received"] = final.stat().st_size
        if rec["bytes_received"] == size:
            rec["local_hash"] = file_hash(final, algo)
            rec["device_hash"] = self.device_hash(dev, bs, tool)
            if rec["device_hash"] == rec["local_hash"]:
                rec["status"] = "skipped"
                return rec
        rec["status"] = "conflict"
        rec["error"] = f"{final} exists and is not a matching complete image; move it aside"
        return rec

    def run_partition(self, p: Partition, tool: str) -> dict[str, Any]:
        entry: dict[str, Any] = {"name": p.name, "index": p.index, "erasesize": p.erasesize}
        entry.update(self.image(p.device, p.size, p.bs, tool, self.dest(p)))
        statuses = [entry["status"]]
        if self.has_char(p):
            assert p.char_device
            entry["raw"] = self.image(p.char_device, p.size, p.bs, tool, self.dest(p, raw=True))
            statuses.append(entry["raw"]["status"])
        bad = [s for s in statuses if s not in ("ok", "skipped")]
        entry["status"] = bad[0] if bad else ("skipped" if set(statuses) == {"skipped"} else "ok")
        return entry

    def plan(self, parts: list[Partition], utils: dict[str, bool]) -> list[dict[str, Any]]:
        return [
            {
                "name": p.name, "index": p.index, "device_path": p.device, "size_bytes": p.size,
                "bs": p.bs, "raw_device": p.char_device if self.has_char(p) else None,
                "file": str(self.dest(p)),
            }
            for p in parts
        ]  # fmt: skip

    def execute(self, only: list[str], dry_run: bool) -> int:
        m = self.manifest
        m.update(
            target=self.target, host=self.host, timestamp=datetime.now(UTC).isoformat(),
            status="failed", partitions=[], warnings=self.warnings, errors=self.errors,
            dry_run=dry_run,
        )  # fmt: skip
        try:
            _, out, _ = self.r.run(["uname", "-a"])
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
            tool = self._pick_tool(utils)
            m["hash_utility"] = tool
            m["plan"] = self.plan(parts, utils)
            if dry_run:
                m["status"] = "dry-run"
                return 0
            self.out.mkdir(parents=True, exist_ok=True)
            for p in parts:
                m["partitions"].append(self.run_partition(p, tool))
            bad = [x for x in m["partitions"] if x["status"] not in ("ok", "skipped")]
            for x in bad:
                self.errors.append(f"{x['name']}: {x['status']}")
            m["status"] = "failed" if bad else "ok"
        except RuntimeError as e:
            self.errors.append(str(e))
        except OSError as e:
            self.errors.append(f"{type(e).__name__}: {e}")
        return 0 if m["status"] in ("ok", "dry-run") else 1

    def _pick_tool(self, utils: dict[str, bool]) -> str:
        if not utils["dd"]:
            raise RuntimeError("required utility missing on device: dd")
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


def main(argv: list[str] | None = None, runner: Runner | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", required=True)
    ap.add_argument("--user", default="root")
    ap.add_argument("--key", default="~/.ssh/harmony_owner_ed25519")
    ap.add_argument("--target", required=True, choices=["hub", "remote"])
    ap.add_argument("--out-dir", required=True, type=Path)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--only", action="append", default=[], help="partition name/index; repeatable")
    a = ap.parse_args(argv)
    b = Backup(runner or SshRunner(a.host, a.user, a.key), a.target, a.host, a.out_dir)
    rc = b.execute(a.only, a.dry_run)
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
