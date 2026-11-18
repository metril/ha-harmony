"""assets/icons/*.svg (24x24 stroke icons) -> TTF with each icon as a filled glyph at U+E000+id.

Strokes become fills via picosvg (skia-pathops). Run through tools/gen_remote_icon_font.sh, which
supplies the dependencies (`uv run --no-project --with picosvg --with fonttools`).
Usage: icons_to_ttf.py out.ttf
"""

import sys
from pathlib import Path

import yaml
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.cu2quPen import Cu2QuPen
from fontTools.pens.transformPen import TransformPen
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.svgLib.path import parse_path
from picosvg.svg import SVG

ROOT = Path(__file__).resolve().parents[1]
BASE = 0xE000
SCALE = 100  # 24-unit grid -> 2400 upm, so size N px in lv_font_conv draws the 24-grid at N/24


def main(out: str) -> None:
    icons = yaml.safe_load((ROOT / "proto/icons.yaml").read_text())["icons"]
    glyphs, cmap, metrics = {".notdef": TTGlyphPen(None).glyph()}, {}, {".notdef": (2400, 0)}
    for ic in icons:
        name = f"i_{ic['name']}"
        svg = SVG.fromstring((ROOT / "assets/icons" / f"{ic['name']}.svg").read_text()).topicosvg()
        pen = TTGlyphPen(None)
        # y flip: SVG y grows down; glyph y grows up from the baseline (icon sits on 0..2400)
        tp = TransformPen(Cu2QuPen(pen, 1.0, reverse_direction=True), (SCALE, 0, 0, -SCALE, 0, 2400))
        for el in svg.svg_root.iter():
            if el.tag.endswith("path") and el.get("d"):
                parse_path(el.get("d"), tp)
        glyphs[name] = pen.glyph()
        cmap[BASE + ic["id"]] = name
        metrics[name] = (2400, 0)
    order = [".notdef", *(f"i_{i['name']}" for i in icons)]
    fb = FontBuilder(2400, isTTF=True)
    fb.setupGlyphOrder(order)
    fb.setupCharacterMap(cmap)
    fb.setupGlyf(glyphs)
    fb.setupHorizontalMetrics(metrics)
    fb.setupHorizontalHeader(ascent=2400, descent=0)
    fb.setupNameTable({"familyName": "ra_icons", "styleName": "Regular"})
    fb.setupOS2(sTypoAscender=2400, sTypoDescender=0, usWinAscent=2400, usWinDescent=0)
    fb.setupPost()
    fb.save(out)


if __name__ == "__main__":
    main(sys.argv[1])
