#!/usr/bin/env -S uv run
"""Regenerate harmony_irdb/manufacturers.json from the archive's top-level index.json.

    tools/gen_irdb_manufacturers.py                 # download (jsDelivr, GitHub raw fallback)
    tools/gen_irdb_manufacturers.py --from index.json

Output rows are `[name, device_count]`, or `[name, device_count, slug]` when the folder name differs.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from harmony_irdb.archive import MANUFACTURERS_FILE, ArchiveError, safe_segment


async def _download() -> object:
    import aiohttp

    from harmony_irdb.archive import IrdbArchive

    with tempfile.TemporaryDirectory() as tmp:
        async with aiohttp.ClientSession() as session:
            return await IrdbArchive(session, tmp).fetch_json("index.json")


def build(index: object) -> list[list]:
    if not isinstance(index, list):
        raise ArchiveError("index.json is not a list")
    rows = []
    for e in index:
        if not isinstance(e, dict) or not isinstance(e.get("n"), str):
            continue
        slug = safe_segment(e.get("s", e["n"]))
        row = [e["n"], e["c"] if isinstance(e.get("c"), int) else 0]
        if slug != e["n"]:
            row.append(slug)
        rows.append(row)
    rows.sort(key=lambda r: r[0].casefold())
    return rows


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--from", dest="src", type=Path, help="read a local index.json instead")
    ap.add_argument("--out", type=Path, default=MANUFACTURERS_FILE)
    a = ap.parse_args()
    index = json.loads(a.src.read_text(encoding="utf-8")) if a.src else asyncio.run(_download())
    rows = build(index)
    a.out.write_text(
        json.dumps(rows, separators=(",", ":"), ensure_ascii=False) + "\n", encoding="utf-8"
    )
    print(f"{len(rows)} manufacturers -> {a.out} ({a.out.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
