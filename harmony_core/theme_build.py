"""The config's active theme, colour warnings, and degrading pages to what a remote can show.

Pure, no Home Assistant imports. `remote_info.caps` bit 2 = THEME_COLOR (palette + presets),
bit 3 = THEME_CUSTOM (free colours + a user theme).
"""

from __future__ import annotations

from dataclasses import dataclass, replace
from typing import Any

from .icons import BRAND_COLORS, ICON_IDS
from .page_codec import Page, Widget
from .theme_math import (
    INK_MIN,
    MAX_DEPTH,
    PALETTE_NAMES,
    contrast_x100,
    ink,
    meets,
    nearest_palette,
    resolve_theme,
    serialise_theme,
    snap565,
    suggest_fill,
    validate_theme,
)
from .themes import CUSTOM_ID, PRESETS, THEME_IDS

CAP_THEME_COLOR = 4  # remote_info.caps bit 2
CAP_THEME_CUSTOM = 8  # remote_info.caps bit 3
CAPS_ALL = CAP_THEME_COLOR | CAP_THEME_CUSTOM


@dataclass(frozen=True)
class ActiveTheme:
    name: str = "default"
    id: int = 0  # preset id 0..6, or CUSTOM_ID for a user theme
    base: str = "default"  # the preset itself, or the first preset along `extends`
    wire: bytes | None = None  # the 99 wire bytes of a user theme
    tokens: tuple[tuple[str, int], ...] = ()
    fills: tuple[int, ...] = ()


def base_preset(name: str, themes: dict[str, Any]) -> str:
    """The preset a theme name stands on (itself for a preset); 'default' on a broken chain."""
    cur = name
    for _ in range(MAX_DEPTH + 2):
        if cur in PRESETS:
            return cur
        t = themes.get(cur)
        cur = t.get("extends", "default") if isinstance(t, dict) else "default"
    return "default"


def active_theme(model: dict[str, Any]) -> ActiveTheme:
    """The theme the config selects. Raises ThemeError for a broken theme (parse_model rejects those)."""
    name = model.get("theme", "default")
    themes = model.get("themes", {})
    if name in PRESETS:
        p = PRESETS[name]
        return ActiveTheme(
            name, p.id, name, None, tuple(p.tokens.items()), tuple(s.fill for s in p.palette)
        )
    tokens, fills = resolve_theme(name, themes, PRESETS)
    return ActiveTheme(
        name,
        CUSTOM_ID,
        base_preset(name, themes),
        serialise_theme(tokens, fills),
        tuple(tokens.items()),
        tuple(fills),
    )


def _warn(path: list[Any], msg: str) -> dict[str, Any]:
    return {"path": path, "msg": msg, "severity": "warning"}


def _x(v: int) -> str:
    return f"{v / 100:.2f}"


def theme_warnings(model: dict[str, Any]) -> list[dict[str, Any]]:
    """Non-blocking problems: contrast of every user theme, and free colours with weak ink."""
    out: list[dict[str, Any]] = []
    themes = model.get("themes", {})
    for name in themes:
        if name in PRESETS:
            continue
        try:
            tokens, fills = resolve_theme(name, themes, PRESETS)
        except ValueError:
            continue
        for p in validate_theme(tokens, fills):
            what = f"{p['text']} on {p['surface']}"
            out.append(
                _warn(
                    ["themes", name],
                    f"theme {name!r}: {what} contrast is {_x(p['ratio_x100'])}:1, below {_x(p['min_x100'])}:1;"
                    f" nearest passing colour #{p['fix']:06X}",
                )
            )
    try:
        at = active_theme(model)
    except ValueError:
        return out
    toks = dict(at.tokens)
    for i, pg in enumerate(model.get("pages", [])):
        for j, w in enumerate(pg.get("widgets", [])):
            if _logo_only(
                w
            ):  # a brand logo key draws no text on its fill: the brand colour is the point
                continue
            for key in ("color", "on_color"):
                v = w.get(key)
                if not isinstance(v, str) or not v.startswith("#"):
                    continue
                c = snap565(int(v[1:], 16))
                best = ink(c, toks["tx"], toks["bg"])
                if not meets(best, c, *INK_MIN):
                    out.append(
                        _warn(
                            ["pages", i, "widgets", j, key],
                            f"page {pg.get('id', i)!r}: colour {v} has only {_x(contrast_x100(best, c))}:1 with its"
                            f" best text colour (4.50:1 wanted); nearest passing colour"
                            f" #{suggest_fill(c, toks['tx'], toks['bg']):06X}",
                        )
                    )
    return out


def _logo_only(w: dict[str, Any]) -> bool:
    """A button that shows a brand logo and no caption (explicit icon_only)."""
    icon = w.get("icon")
    return (
        w.get("type") == "button"
        and isinstance(icon, str)
        and ICON_IDS.get(icon) in BRAND_COLORS
        and w.get("icon_only") is True
    )


def _is_free(c: str | None) -> bool:
    return c is not None and c.startswith("#")


def has_theming(pages: list[Page]) -> bool:
    return any(p.theme or any(w.color or w.on_color for w in p.widgets) for p in pages)


def degrade_pages(
    pages: list[Page], cfg_ids: list[str], at: ActiveTheme, caps: int
) -> tuple[list[Page], list[dict[str, Any]]]:
    """`pages` as a remote with `caps` can show them, plus notices saying what was lost and why.

    No bit 2: colours and theme id dropped (role colours). Bit 2 only: free colours snap to the
    nearest palette entry of the (base) preset, a user theme is replaced by its base preset."""
    notices: list[dict[str, Any]] = []
    if caps & CAP_THEME_COLOR:
        free = any(_is_free(c) for p in pages for w in p.widgets for c in (w.color, w.on_color))
        if caps & CAP_THEME_CUSTOM or not (at.wire is not None or free):
            return pages, notices
        fills = [s.fill for s in PRESETS[at.base].palette]
        tid = THEME_IDS[at.base]
        reason = "the remote firmware has no custom colour support"

        def snap(c: str | None) -> str | None:
            return PALETTE_NAMES[nearest_palette(int(c[1:], 16), fills) - 1] if _is_free(c) else c

        if at.wire is not None:
            notices.append(
                {
                    "scope": "theme",
                    "page": None,
                    "widgets": [],
                    "msg": f"theme {at.name!r} replaced by its base preset {at.base!r}: {reason}",
                }
            )
    else:
        tid = 0
        reason = "the remote firmware has no colour support"

        def snap(c: str | None) -> str | None:
            return None

        if at.id:
            notices.append(
                {
                    "scope": "theme",
                    "page": None,
                    "widgets": [],
                    "msg": f"theme {at.name!r} not shown: {reason}",
                }
            )
    out: list[Page] = []
    for p, cid in zip(pages, cfg_ids, strict=True):
        hit: list[str] = []
        ws: list[Widget] = []
        for w in p.widgets:
            nc, no = snap(w.color), snap(w.on_color)
            if (nc, no) != (w.color, w.on_color):
                hit.append(w.label or f"#{w.id}")
                w = replace(w, color=nc, on_color=no)
            ws.append(w)
        if hit:
            what = (
                "colours dropped"
                if not caps & CAP_THEME_COLOR
                else "custom colours snapped to the palette"
            )
            notices.append(
                {
                    "scope": "widgets",
                    "page": cid,
                    "widgets": hit,
                    "msg": f"page {cid!r}: {what} on {', '.join(map(repr, hit))}: {reason}",
                }
            )
        out.append(replace(p, widgets=tuple(ws), theme=tid if p.theme else 0))
    return out, notices
