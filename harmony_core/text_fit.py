"""Label pixel budgets on the Elite's 240x320 screen: what fits, what the remote clips.

A port of `ra_ui_widget_rect` and the font / label-width rules of `build_v2_content`
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
    """Width of `s` in `font` (b13 m16 b18 b20 r12); a code point the font lacks counts as '?'."""
    m = _metrics()[font]
    fallback = m["63"]
    return sum(m.get(str(ord(c)), fallback) for c in s)


def clip(s: str, font: str, px: int) -> tuple[str, int]:
    """What LVGL's LONG_DOT mode shows in `px`: (text, pixels over); the text ends in "..." when cut."""
    w = text_px(s, font)
    if w <= px:
        return s, 0
    dots = text_px(DOTS, font)
    chars = list(s)
    while chars and text_px("".join(chars), font) + dots > px:
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
    """(width, height) in px of a widget's drawn rectangle (v2 pages: no title-row shift)."""
    x1, x2 = _edge(w.x, page.cols, SCREEN_W), _edge(w.x + w.w, page.cols, SCREEN_W) - 1
    y1 = BAR_H + _edge(w.y, page.rows, SCREEN_H - BAR_H)
    y2 = BAR_H + _edge(w.y + w.h, page.rows, SCREEN_H - BAR_H) - 1
    x1, x2 = _inset(x1, x2, MARGIN if x1 == 0 else GAP // 2, MARGIN if x2 == SCREEN_W - 1 else GAP // 2)
    y1, y2 = _inset(y1, y2, MARGIN if y1 == BAR_H else GAP // 2, MARGIN if y2 == SCREEN_H - 1 else GAP // 2)
    return x2 - x1 + 1, y2 - y1 + 1


def label_budget(page: Any, w: Any) -> tuple[str, int] | None:
    """(font, max label px) the remote gives this widget's label; None when it draws no label.

    `page` has cols and rows; `w` has kind, x, y, w, h, label, shape, icon, icon_only, trailing.
    """
    bw, bh = widget_size(page, w)
    has_icon = bool(w.icon)
    if w.kind == "tile":
        if not (bh >= 44 and w.label):
            return None
        return "r12", bw - (38 if has_icon and bh >= 56 and bw >= 56 else 8)
    if w.kind != "button" or not w.label or w.shape == "monogram" or (w.icon_only and has_icon):
        return None
    row = w.shape in ("row", "pill") or (bh < 44 and bw >= 100)
    big = w.shape == "card" and not row and bw >= 100 and bh >= 80
    px = 24 if row else 32 if big else 24 if bh >= 64 else 20
    if big:
        font, lw = "b18", bw - 24
    else:
        font = "m16" if row or bw >= 100 else "b13"
        lw = bw - 8
    if row:
        # a keypad pill with a 1-2 character label (a digit) is set in b20
        if w.shape == "pill" and len(w.label) <= 2:
            font = "b20"
        lw = bw - 12 - px - 16 if has_icon else bw - 24
        if getattr(w, "trailing", False):
            lw -= 28
    return font, max(0, lw)


def fit_label(page: Any, w: Any) -> tuple[str, int, int] | None:
    """(font, budget px, px over) for a widget whose label the remote would cut, else None."""
    b = label_budget(page, w)
    if b is None:
        return None
    over = text_px(w.label, b[0]) - b[1]
    return (b[0], b[1], over) if over > 0 else None
