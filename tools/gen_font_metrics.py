"""Generate proto/font_metrics.json (code point -> advance px, per baked font) from remote/app/fonts/ra_font_*.c.

Widths are LVGL's rounded advance, (adv_w + 8) >> 4, and the fonts are built --no-kerning, so a label is
the plain sum. A test fails when the JSON is stale. Usage: uv run python tools/gen_font_metrics.py
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FONT_DIR = ROOT / "remote" / "app" / "fonts"
OUT = ROOT / "proto" / "font_metrics.json"
# copies the runtime reads: harmony_core ships in the add-on and integration, the frontend stage only gets frontend-shared
COPIES = (
    ROOT / "harmony_core" / "font_metrics.json",
    ROOT / "harmony_addon" / "frontend-shared" / "font_metrics.json",
)
FONTS = ("b13", "m16", "b18", "b20", "r12")


def _int(pattern: str, text: str) -> int:
    m = re.search(pattern, text)
    if not m:
        raise SystemExit(f"font source: {pattern} not found")
    return int(m.group(1))


def parse(src: str) -> dict[str, int]:
    body = src.split("glyph_dsc[] = {", 1)[1].split("};", 1)[0]
    adv = [int(m) for m in re.findall(r"\.adv_w = (\d+)", body)]
    lists = {
        m.group(1): [int(x, 16) for x in re.findall(r"0x[0-9a-f]+", m.group(2))]
        for m in re.finditer(r"unicode_list_(\d+)\[\] = \{(.*?)\};", src, re.DOTALL)
    }
    cmaps = src.split("cmaps[] =", 1)[1].split("};", 1)[0]
    out: dict[str, int] = {}
    for m in re.finditer(r"\{\s*\.range_start = (\d+).*?\.type = (\w+)\s*\}", cmaps, re.DOTALL):
        start, gid0 = int(m.group(1)), _int(r"glyph_id_start = (\d+)", m.group(0))
        if m.group(2).endswith("FORMAT0_TINY"):
            length = _int(r"range_length = (\d+)", m.group(0))
            pairs = [(start + k, gid0 + k) for k in range(length)]
        else:
            ul = re.search(r"unicode_list = unicode_list_(\d+)", m.group(0))
            if not ul:
                raise SystemExit("unsupported cmap")
            pairs = [(start + off, gid0 + k) for k, off in enumerate(lists[ul.group(1)])]
        for cp, gid in pairs:
            out[str(cp)] = (adv[gid] + 8) >> 4
    # LV_USE_FONT_PLACEHOLDER: a missing glyph is line_height / 2 + 2 wide (lv_font.c); stored as "lh"
    out["lh"] = _int(r"\.line_height = (\d+)", src)
    return out


def build() -> dict[str, dict[str, int]]:
    return {f: parse((FONT_DIR / f"ra_font_{f}.c").read_text()) for f in FONTS}


def render() -> str:
    return json.dumps(build(), sort_keys=True, separators=(",", ":")) + "\n"


if __name__ == "__main__":
    for path in (OUT, *COPIES):
        path.write_text(render())
        sys.stdout.write(f"wrote {path}\n")
