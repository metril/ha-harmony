"""Label pixel budgets on the Elite's 240x320 screen: what fits, what the remote clips.

A port of `ra_ui_widget_rect` and the class font / label-width rules of `build_v2_content` / `build_tile`
(remote/app/ui.c), measured with the baked fonts' advance widths (proto/font_metrics.json, made by
tools/gen_font_metrics.py). The TypeScript twin is addon/frontend/src/lib/text-fit.ts; both run
tests/vectors/text_fit.json. No Home Assistant imports.
"""

from __future__ import annotations

import json
from functools import cache
from pathlib import Path
from typing import Any

SCREEN_W, SCREEN_H, BAR_H = 240, 320, 32
MARGIN, GAP = 8, 8
DOTS = "..."


@cache
def _metrics() -> dict[str, dict[str, int]]:
    return json.loads((Path(__file__).with_name("font_metrics.json")).read_text())


def text_px(s: str, font: str) -> int:
    """Width of `s` in `font` (b13 m16 b18 b20 r12); a code point the font lacks is LVGL's placeholder, line_height / 2 + 2."""
    m = _metrics()[font]
    fallback = m["lh"] // 2 + 2  # LV_USE_FONT_PLACEHOLDER (lv_font.c)
    return sum(m.get(str(ord(c)), fallback) for c in s)


def clip(s: str, font: str, px: int) -> tuple[str, int]:
    """What LVGL's LONG_DOT mode shows in `px`: (text, pixels over); the text ends in "..." when cut."""
    w = text_px(s, font)
    if w <= px:
        return s, 0
    if len(s) <= len(DOTS):  # lv_label: never turn all the letters into dots
        return s, w - px
    dots = text_px(DOTS, font)
    chars = list(s)
    while chars and text_px("".join(chars), font) + dots > px:
        chars.pop()
    # lv_label.c: "be sure there is space for the dots" - the dots overwrite 3 bytes, so step back until they fit the text
    while chars and len("".join(chars).encode()) + len(DOTS) > len(s.encode()):
        chars.pop()
    return "".join(chars) + DOTS, w - px


def _edge(i: int, n: int, total: int) -> int:
    return -(-i * total // n)


def _inset(lo: int, hi: int, a: int, b: int) -> tuple[int, int]:
    while hi - lo + 1 - a - b < 12 and (a or b):
        a -= a > 0
        b -= b > 0
    return lo + a, hi - b


def widget_size(page: Any, w: Any) -> tuple[int, int]:
    """(width, height) in px of a widget's drawn rectangle (v2 pages: no title-row shift).

    A FIXED page (`page.fixed`) gives every widget its class size (docs/proto/page-format.md); other pages the grid cell."""
    if getattr(page, "fixed", False):
        sp, compact = w.w, getattr(w, "compact", False)
        if w.h == 1 and sp in (12, 6, 4, 3) and w.x % sp == 0:
            n = 12 // sp
            if w.kind == "label" or w.shape == "row":
                h = 44
            elif w.kind == "tile":
                h = 64
            elif w.shape == "pill":
                h = 52
            else:
                h = (56 if compact else 80) if sp >= 6 else 52
            return (224 - (n - 1) * 8) // n, h
        # an unplaceable widget: the page is rejected elsewhere; measure it as a cell
    x1, x2 = _edge(w.x, page.cols, SCREEN_W), _edge(w.x + w.w, page.cols, SCREEN_W) - 1
    y1 = BAR_H + _edge(w.y, page.rows, SCREEN_H - BAR_H)
    y2 = BAR_H + _edge(w.y + w.h, page.rows, SCREEN_H - BAR_H) - 1
    x1, x2 = _inset(
        x1, x2, MARGIN if x1 == 0 else GAP // 2, MARGIN if x2 == SCREEN_W - 1 else GAP // 2
    )
    y1, y2 = _inset(
        y1, y2, MARGIN if y1 == BAR_H else GAP // 2, MARGIN if y2 == SCREEN_H - 1 else GAP // 2
    )
    return x2 - x1 + 1, y2 - y1 + 1


def label_budget(page: Any, w: Any) -> tuple[str, int] | None:
    """(font, max label px) the remote gives this widget's label; None when it draws no label.

    `page` has cols and rows (and `fixed` on a FIXED page); `w` has kind, x, y, w, h, label, shape, icon, icon_only,
    trailing and compact. The class rules of `build_v2_content` / `build_tile` (remote/app/ui.c): row m16, card b13
    (compact: two lines), key r12 caption under a glyph, pill b20 digit, tile name m16.
    """
    bw, bh = widget_size(page, w)
    has_icon = bool(w.icon)
    if w.kind == "tile":
        if not w.label:
            return None
        return "m16", max(0, bw - 20 if bh >= 56 and bw >= 80 else bw - 64)
    if w.kind != "button" or not w.label or w.shape == "monogram" or (w.icon_only and has_icon):
        return None
    if w.shape == "row" or (bh < 44 and bw >= 100):
        px = 20 if bh < 36 else 24
        lw = bw - 10 - (px + 10 if has_icon else 0) - 8 - (30 if getattr(w, "trailing", False) else 0)
        return "m16", max(0, lw)
    if w.shape == "pill":
        return ("b20" if len(w.label) <= 2 else "m16"), max(0, bw - 8)  # a digit reads at b20
    if bw >= 100:
        if getattr(w, "compact", False) or bh < 72:  # two lines beside the glyph, wrapped at spaces only
            line = bw - (44 if has_icon else 10) - 8
            if any(text_px(word, "b13") > line for word in w.label.split()):
                return "b13", max(0, line - 1)  # a word wider than the line would break mid-word: never fits
            return "b13", max(0, 2 * line - 4)
        return ("b13" if has_icon else "m16"), max(0, bw - 24)
    return ("r12" if has_icon else "b13"), max(0, bw - 6)


def fit_label(page: Any, w: Any) -> tuple[str, int, int] | None:
    """(font, budget px, px over) for a widget whose label the remote would cut, else None."""
    b = label_budget(page, w)
    if b is None:
        return None
    over = text_px(w.label, b[0]) - b[1]
    return (b[0], b[1], over) if over > 0 else None
