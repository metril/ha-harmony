#!/usr/bin/env python3
"""Build the HACS release package: a zip of custom_components/ha_harmony.

HACS with `zip_release: true` downloads the release asset named in hacs.json and unpacks it
into `custom_components/<domain>/`, so the zip holds the integration's files at its ROOT
(manifest.json at the top level, not under a directory). Caches and bytecode are excluded.

    tools/package_integration.py --version 0.1.0 --out dist/ha_harmony.zip

--version must equal manifest.json's version: a release whose tag disagrees with the version
Home Assistant reports is refused rather than shipped.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "custom_components" / "ha_harmony"
EXCLUDE_DIRS = {"__pycache__"}
EXCLUDE_SUFFIXES = {".pyc", ".pyo"}
# Fixed timestamp so the same source always yields the same bytes (reproducible package).
EPOCH = (2026, 1, 1, 0, 0, 0)


def package_files(src: Path) -> list[Path]:
    """Real files under src, following symlinked directories (harmony_core is one in the repo)."""
    files = []
    for dirpath, dirnames, filenames in os.walk(src, followlinks=True):
        dirnames[:] = [d for d in dirnames if d not in EXCLUDE_DIRS]
        for f in filenames:
            p = Path(dirpath) / f
            if p.suffix not in EXCLUDE_SUFFIXES:
                files.append(p)
    return sorted(files, key=lambda p: p.relative_to(src).as_posix())


def build(src: Path, out: Path, version: str) -> list[str]:
    manifest = json.loads((src / "manifest.json").read_text(encoding="utf-8"))
    if manifest.get("version") != version:
        raise SystemExit(
            f"error: manifest.json version is {manifest.get('version')!r}, release is {version!r}"
        )
    out.parent.mkdir(parents=True, exist_ok=True)
    names = []
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as zf:
        for p in package_files(src):
            name = p.relative_to(src).as_posix()
            info = zipfile.ZipInfo(name, EPOCH)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            zf.writestr(info, p.read_bytes())
            names.append(name)
    return names


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--version", required=True, help="release version, without a leading v")
    ap.add_argument("--out", type=Path, default=ROOT / "dist" / "ha_harmony.zip")
    a = ap.parse_args(argv)
    names = build(SRC, a.out, a.version.removeprefix("v"))
    digest = hashlib.sha256(a.out.read_bytes()).hexdigest()
    print(f"{a.out}: {len(names)} files, sha256 {digest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
