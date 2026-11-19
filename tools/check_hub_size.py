#!/usr/bin/env python3
"""Size budget of the hub firmware: `check_hub_size.py build-cross/hub`.

The hub's /data is about 2.4 MB and an OTA needs 2 x image + HP_OTA_SLACK free (hub/ota/hub_ota.c), so the binary that is
pushed and signed must be the stripped one. Fails (exit 1) when the file is unstripped, is over the budget (default
600 KiB), lacks the build-id tag ota_sign reads, or when the OTA need exceeds the free space assumption. Always prints
the numbers."""

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def ota_slack() -> int:
    m = re.search(r"#define\s+HP_OTA_SLACK\s+\((\d+)u\s*\*\s*(\d+)u\)", (ROOT / "hub/ota/hub_ota.h").read_text())
    return int(m.group(1)) * int(m.group(2)) if m else 64 * 1024


def check(path: Path, max_kib: int = 600, data_kib: int = 2400, slack: int | None = None) -> tuple[bool, list[str]]:
    slack = ota_slack() if slack is None else slack
    data = path.read_bytes()
    size = len(data)
    need = 2 * size + slack
    lines = [
        f"{path}: {size} B ({size / 1024:.1f} KiB), budget {max_kib} KiB",
        f"OTA needs 2 x {size} + {slack} = {need} B ({need / 1024:.0f} KiB) free; assumed free /data {data_kib} KiB",
    ]
    ok = True
    if b".symtab" in data:
        lines.append("FAIL: not stripped (has .symtab): push and sign the stripped build output, not hub.debug")
        ok = False
    if size > max_kib * 1024:
        lines.append(f"FAIL: {size / 1024:.1f} KiB is over the {max_kib} KiB budget")
        ok = False
    if need > data_kib * 1024:
        lines.append(f"FAIL: OTA need {need / 1024:.0f} KiB exceeds {data_kib} KiB")
        ok = False
    if b"HPBUILDID=" not in data:
        lines.append("FAIL: build-id tag HPBUILDID= missing (ota_sign could not read the id; was it garbage-collected?)")
        ok = False
    return ok, lines


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("hub", type=Path)
    ap.add_argument("--max-kib", type=int, default=600)
    ap.add_argument("--data-kib", type=int, default=2400)
    a = ap.parse_args()
    ok, lines = check(a.hub, a.max_kib, a.data_kib)
    print("\n".join(lines))
    print("size budget: " + ("ok" if ok else "FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
