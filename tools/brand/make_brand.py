#!/usr/bin/env python3
"""Generate Harmony brand assets (SVG + PNG). Text is converted to outlines.

    uv run --with cairosvg --with fonttools python tools/brand/make_brand.py
"""
import io
import math
import urllib.request
import zipfile
from pathlib import Path

import cairosvg
from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen
from fontTools.ttLib import TTFont

HERE = Path(__file__).resolve().parent
OUT = HERE.parents[1] / "custom_components" / "ha_harmony" / "brand"
CACHE = Path.home() / ".cache" / "ha-harmony-brand"
FONT_ZIP = "https://github.com/rsms/inter/releases/download/v4.0/Inter-4.0.zip"
COLOR = "#0EA5C6"
INK = "#1F2328"


def font(name: str) -> TTFont:
    CACHE.mkdir(parents=True, exist_ok=True)
    p = CACHE / name
    if not p.exists():
        data = urllib.request.urlopen(urllib.request.Request(FONT_ZIP, headers={"User-Agent": "x"})).read()
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            member = next(n for n in z.namelist() if n.endswith("/" + name) and "extras/ttf" in n)
            p.write_bytes(z.read(member))
    return TTFont(p)


def text_path(f: TTFont, text: str, size: float, tracking: float, x: float, y: float):
    """Return (svg path d, right edge). No GPOS kerning; tracking only."""
    gs, cmap, hmtx = f.getGlyphSet(), f.getBestCmap(), f["hmtx"]
    s = size / f["head"].unitsPerEm
    pen = SVGPathPen(gs, ntos=lambda v: f"{v:.2f}".rstrip("0").rstrip("."))
    cx = x
    for i, ch in enumerate(text):
        g = cmap[ord(ch)]
        gs[g].draw(TransformPen(pen, (s, 0, 0, -s, cx, y)))
        adv = hmtx[g][0] * s
        cx += adv + (tracking if i < len(text) - 1 else 0)
    return pen.getCommands(), cx


GLYPH = (
    '<g transform="rotate(-28 50 54)" fill="none" stroke="#fff" stroke-width="4.5" '
    'stroke-linecap="round" stroke-linejoin="round">'
    '<rect x="40" y="38" width="20" height="40" rx="8"/>'
    '<path d="M41 28A13 13 0 0 1 59 28"/>'
    '<circle cx="50" cy="50" r="4.5" fill="#fff" stroke="none"/>'
    '<circle cx="50" cy="64" r="2.6" fill="#fff" stroke="none"/></g>'
)
TILE = f'<rect width="100" height="100" rx="22" fill="{COLOR}"/>{GLYPH}'


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    icon = f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100" width="100" height="100">{TILE}</svg>\n'
    (HERE / "icon.svg").write_text(icon)

    d1, r1 = text_path(font("Inter-Bold.ttf"), "INTEGRATION", 15, 3.2, 164, 66)
    d2, r2 = text_path(font("Inter-ExtraBold.ttf"), "Harmony", 60, -1.2, 160, 116)
    width = math.ceil(max(r1, r2) + 20)
    logo = (
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} 160" width="{width}" height="160">'
        f'<g transform="translate(20 20) scale(1.2)">{TILE}</g>'
        f'<path d="{d1}" fill="{COLOR}"/><path d="{d2}" fill="{INK}"/></svg>\n'
    )
    (HERE / "logo.svg").write_text(logo)

    for name, svg, kw in [
        ("icon.png", icon, dict(output_width=256, output_height=256)),
        ("icon@2x.png", icon, dict(output_width=512, output_height=512)),
        ("logo.png", logo, dict(output_height=160)),
        ("logo@2x.png", logo, dict(output_height=320)),
    ]:
        cairosvg.svg2png(bytestring=svg.encode(), write_to=str(OUT / name), **kw)
    print(f"logo width {width} (INTEGRATION right {r1:.1f}, Harmony right {r2:.1f})")


if __name__ == "__main__":
    main()
