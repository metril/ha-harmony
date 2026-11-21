"""Model pages -> wire pages: inference, title rule, list splitting, ids, v1 + v2 encodings.

Everything the remote used to guess is decided here, once; explicit config always wins. The result
is one list of v2 `Page` models; `encode(page, 1)` and `encode(page, 2)` derive both wire formats.
No Home Assistant imports.
"""

from __future__ import annotations

import re
import zlib
from dataclasses import dataclass, replace
from types import SimpleNamespace
from typing import Any

from .config_model import Model, infer_app_ids
from .ha_actions import default_tap
from .harmony_proto import SOFT_KEY_BASE
from .icons import BRAND_COLORS, ICON_IDS, ICONS
from .key_names import display_name
from .keys import key_code
from .page_codec import (
    HEADER_V2,
    MAX_BYTES,
    MAX_WIDGETS,
    Page,
    Widget,
    FIXED_MAX,
    encode,
    fixed_height,
    page_size,
    widget_cost,
    widget_size,
)
from .text_fit import fit_label

MAX_SOFT_KEYS = 256 - SOFT_KEY_BASE  # widget wire ids 0..127
MAX_PAGE_GROUP = 15  # seq count is 4 bits
MAX_PAGES = 32  # the remote store holds page ids 0..31 (HP_STORE_MAX)

_TOKEN = re.compile(r"[a-z0-9+\-]+")
_CAMEL = re.compile(r"(?<=[a-z0-9])(?=[A-Z])|(?<=[A-Z])(?=[A-Z][a-z])")
_EXPAND = {"vol": "volume", "ch": "channel", "chan": "channel", "pwr": "power", "prev": "previous"}
_EXACT = 100  # an icon named exactly like the text beats any keyword overlap
_DANGER_LABEL = re.compile(r"^(power ?off|all off)\b")
_DOMAIN_ICON = {
    "light": "bulb",
    "switch": "switch",
    "input_boolean": "switch",
    "climate": "thermo",
    "cover": "blinds",
    "fan": "fan",
    "lock": "lock",
    "scene": "scene",
    "script": "scene",
    "media_player": "tv",
    "sun": "sun",
    "input_select": "settings",
}
_SERVICE_ICON = {"open_cover": "up", "close_cover": "down"}
_NAV_ICON_LEAD = {"prev": "chev_left", "home": "home"}
_KIND_ICONS = ("speaker", "soundbar", "receiver", "streamer", "projector", "kvm", "plug", "laptop", "pc")
_CHEVRONS = frozenset(i for n, i in ICON_IDS.items() if n.startswith("chev"))
_BRAND_NAMES = {"shield": "nvidia", "nvidia": "nvidia", "plex": "plex", "youtube": "youtube", "kodi": "kodi"}


class CompileError(ValueError):
    """The config is valid YAML but does not fit the hub or the remote.

    `path` (when known) is the config path of the culprit, e.g. ["pages", 2]."""

    def __init__(self, msg: str, path: list[Any] | None = None) -> None:
        super().__init__(msg)
        self.path = path or []
        self.count: int | None = None  # wire pages needed, when the error is about the page cap


@dataclass(frozen=True)
class AppBinding:
    """An app button whose ring follows a media player: its app ids and the player (apps_entity)."""

    page: int  # wire page id
    widget: int  # wire id of the button
    app_ids: tuple[str, ...]
    entity_id: str
    activity: str | None = None  # the ring shows only while this activity runs


def _apps_entity(
    model: Model, page: dict[str, Any], action: dict[str, Any]
) -> tuple[str, str] | tuple[None, None]:
    """The apps_entity behind a button: its page's activity, else the activity that uses the button's
    device, else the only one the config names."""
    acts = {a["id"]: a for a in model["activities"]}
    media = lambda a: (a.get("media") or {}).get("apps_entity")  # noqa: E731
    if page.get("activity") in acts:
        a = acts[page["activity"]]
        return (media(a), a["id"]) if media(a) else (None, None)
    if action["type"] == "device":
        for a in model["activities"]:
            if any(d["device"] == action["device"] for d in a["devices"]) and media(a):
                return media(a), a["id"]
    found = [a for a in model["activities"] if media(a)]
    return (media(found[0]), found[0]["id"]) if len(found) == 1 else (None, None)


@dataclass(frozen=True)
class Tile:
    page: int  # wire page id
    widget: int  # wire id
    entity_id: str
    label: str = ""


def _words(text: str) -> list[str]:
    """Lower-case words of a command or label: camelCase and snake_case split ("DirectionUp" -> direction up)."""
    return _TOKEN.findall(re.sub(r"[_\s]+", " ", _CAMEL.sub(" ", text)).lower())


def icon_for_text(text: str) -> int:
    """Stock icon for a command or label; 0 if none.

    An icon named exactly like the text ("vol_up", or "up" for "DirectionUp") beats any keyword
    match; otherwise the icon whose keywords share the most words wins (lowest id on ties).
    """
    toks: set[str] = set()
    names: set[str] = set()
    for src in dict.fromkeys((text, display_name(text))):
        words = _words(src)
        names.add("_".join(words))
        names.add("".join(words))  # "You Tube" -> youtube
        for t in words:
            toks.add(t)
            toks.add(_EXPAND.get(t, t))
            if len(t) > 3 and t.endswith("s"):
                toks.add(t[:-1])
    best, best_score = 0, 0
    for ic in ICONS:
        score = len(toks & set(ic.keywords)) + (_EXACT if ic.name in names else 0)
        if score > best_score:
            best, best_score = ic.id, score
    return best


@dataclass
class _Spec:
    """One widget before ids and positions are final."""

    cfg: dict[str, Any]
    kind: str
    label: str
    role: str
    shape: str
    icon_only: bool
    icon: int
    bind: int | None
    nav: str | None
    color: str | None = None
    on_color: str | None = None
    align: str = "center"
    compact: bool = False
    x: int = 0
    y: int = 0
    w: int = 1
    h: int = 1
    order: int = 0  # index in the config page, to emit widgets in config order after a re-grid

    @property
    def trailing(self) -> bool:
        """Forward nav (next / a page id) draws a trailing chevron; prev and home keep a leading icon."""
        return self.nav is not None and self.nav not in _NAV_ICON_LEAD


@dataclass
class _Unit:
    """One wire page: a configured page, or one page of a split list."""

    cfg_index: int
    cfg_id: str
    specs: list[_Spec]
    cols: int
    rows: int
    title: str
    kind: str
    back: bool
    act: int
    seq: tuple[int, int] = (0, 0)
    fixed: bool = False


def nav_icon(label: str, dev_icons: dict[str, str] | None, dev_by_name: dict[str, str] | None) -> int:
    """Icon for a nav row with none set: the device it names (its config icon), a brand logo matched by
    name, then name keywords (the device kind or domain words are among them). Mirrors studio-model.ts."""
    dev = (dev_by_name or {}).get(label.strip().casefold())
    if dev and (dev_icons or {}).get(dev):
        return ICON_IDS[dev_icons[dev]]  # type: ignore[index]
    for word in _words(label):
        if word in _BRAND_NAMES:
            return ICON_IDS[_BRAND_NAMES[word]]
    for word in _words(label):
        kind = word[:-1] if len(word) > 3 and word.endswith("s") else word
        if kind in _KIND_ICONS:
            return ICON_IDS[kind]
    found = icon_for_text(label)
    return 0 if found in _CHEVRONS else found  # a nav row draws its own trailing chevron


def _spec(
    w: dict[str, Any],
    act_ids: list[str],
    act_names: dict[str, str],
    in_list: bool,
    dev_icons: dict[str, str] | None = None,
    dev_by_name: dict[str, str] | None = None,
    act_devs: dict[str, list[str]] | None = None,
) -> _Spec:
    kind, label = w["type"], w["label"]
    role = w.get("role", "default")
    icon: int | None = None
    if w.get("icon") is not None:
        icon = 0 if w["icon"] == "none" else ICON_IDS[w["icon"]]
    bind: int | None = None
    nav = w.get("nav")
    if nav is not None:
        role = "nav"
        if icon is None and nav in _NAV_ICON_LEAD:
            icon = ICON_IDS[_NAV_ICON_LEAD[nav]]
        if icon is None:
            icon = nav_icon(label, dev_icons, dev_by_name)
    elif kind == "button":
        action = w["action"]
        target = action["activity"] if action["type"] == "activity" else None
        if target == "off" or _DANGER_LABEL.match(label.lower()):
            role = w.get("role", "danger")
            if icon is None:
                icon = ICON_IDS["power"]
        elif target is not None:
            bind = (
                act_ids.index(target) if target in act_ids else None
            )  # unknown: validated elsewhere
            if icon is None:
                icon = icon_for_text(f"{act_names.get(target, target)} {target}")
                for dn in (act_devs or {}).get(target, []) if not icon else []:
                    found = nav_icon(dn, dev_icons, dev_by_name)  # the activity's own devices
                    icon = icon or found
        if icon is None and action["type"] == "device" and dev_icons:
            named = dev_icons.get(action["device"])  # the device's own icon is the default
            if named:
                icon = ICON_IDS[named]
        if icon is None and action["type"] == "ha_call":
            svc = action["service"]
            dom, _, name = svc.partition(".")
            icon = ICON_IDS.get(_SERVICE_ICON.get(name) or _DOMAIN_ICON.get(dom, ""), None)
        if icon is None:
            icon = icon_for_text(label)
    elif kind == "tile" and icon is None:
        icon = ICON_IDS.get(_DOMAIN_ICON.get(w["entity"].split(".")[0], ""), 0)
    shape = w.get("shape", "row" if in_list or nav is not None else "card")
    return _Spec(
        w,
        kind,
        label,
        role,
        shape,
        bool(w.get("icon_only", False)),
        icon or 0,
        bind,
        nav,
        w.get("color"),
        w.get("on_color"),
        w.get("align", "center"),
        bool(w.get("compact", False)),
        w.get("x", 0),
        w.get("y", 0),
        w["w"],
        w["h"],
    )


def _probe(s: _Spec) -> Widget:
    return Widget(
        s.kind,
        0,
        s.x,
        s.y,
        s.w,
        s.h,
        s.label,
        s.role,
        s.shape,
        s.icon_only,
        s.icon,
        s.bind,
        s.color,
        s.on_color,
        s.trailing,
        s.align,
        s.compact,
    )


def _lead_title(specs: list[_Spec], cols: int) -> _Spec | None:
    """The full-width row-0 label that the old remote promoted to the status-bar title."""
    return next(
        (
            s
            for s in specs
            if s.kind == "label" and (s.x, s.y, s.w, s.h) == (0, 0, cols, 1) and s.label
        ),
        None,
    )


def _default_icon_only(s: _Spec, cols: int, rows: int, fixed: bool = False) -> None:
    """A button that has an icon and no `icon_only` in the config shows the icon alone when its label
    would be cut, or when the cell is 1 wide in a 4-column grid. An explicit value always wins."""
    if "icon_only" in s.cfg or s.kind != "button" or not s.icon or s.nav is not None:
        return
    if s.shape == "monogram":
        return
    if s.icon in BRAND_COLORS:  # a brand logo replaces the text where there is no room for both
        if not fixed or (s.w <= 4 and s.shape == "card"):
            s.icon_only = True
            return
    if (cols == 4 and s.w == 1) or fit_label(SimpleNamespace(cols=cols, rows=rows, fixed=fixed), _probe(s)):
        s.icon_only = True


_EXIT = key_code("exit")


def _maps_exit(model: Model, activity: str | None) -> bool:
    """Would the hub resolve Exit to a binding? With an activity: its keys or the global map. Without
    one (any activity may be running): any activity's keys or the global map."""
    maps = [model.get("keys") or {}]
    maps += [a.get("keys") or {} for a in model["activities"] if activity is None or a["id"] == activity]
    return any(key_code(n) == _EXIT for m in maps for n in m)


def _default_back(model: Model, p: dict[str, Any]) -> bool:
    if "back" in p:
        return bool(p["back"])
    if p.get("kind") in ("home", "favorites"):
        return False
    return not _maps_exit(model, p.get("activity"))


def _split_fixed(specs: list[_Spec], p: dict[str, Any], pi: int, base: int) -> list[list[_Spec]]:
    """Split a FIXED page by grid rows: a new wire page when the stacked height, the byte budget, the
    widget count or the 16 grid rows run out. Rows are renumbered from 0 on every page."""
    rows: dict[int, list[_Spec]] = {}
    for s in specs:
        rows.setdefault(s.y, []).append(s)
    groups: list[list[list[_Spec]]] = [[]]
    size = base

    def height(g: list[list[_Spec]]) -> int:
        ws = tuple(
            replace(_probe(s), y=y) for y, row in enumerate(g) for s in row
        )
        return fixed_height(Page(0, 12, max(1, len(g)), ws, 2, 0, 1, fixed=True))

    for y in sorted(rows):
        row = rows[y]
        n = sum(widget_size(_probe(s)) for s in row)
        try:
            if height([row]) > FIXED_MAX or base + n > MAX_BYTES or len(row) > MAX_WIDGETS:
                raise CompileError(
                    f"page {p['id']!r}: grid row {y} does not fit one page; shorten its labels or use fewer widgets",
                    ["pages", pi],
                )
            cur = groups[-1]
            fits = (
                cur
                and len(cur) < 16
                and size + n <= MAX_BYTES
                and sum(len(r) for r in cur) + len(row) <= MAX_WIDGETS
                and height([*cur, row]) <= FIXED_MAX
            )
        except ValueError as exc:
            raise CompileError(f"page {p['id']!r}: {exc}", ["pages", pi]) from exc
        if not fits and groups[-1]:
            groups.append([])
            size = base
        groups[-1].append(row)
        size += n
    while len(groups) > 1 and len(groups[-1]) == 1 and len(groups[-2]) > 1:
        moved = groups[-2][-1]  # never leave one grid row alone on the last page
        if height([moved, *groups[-1]]) > FIXED_MAX:
            break
        groups[-1].insert(0, moved)
        groups[-2].pop()
    moved_any = True  # balance: neighbouring pages differ by at most one grid row
    while moved_any and len(groups) > 1:
        moved_any = False
        for i in range(len(groups) - 1, 0, -1):
            if len(groups[i - 1]) - len(groups[i]) >= 2:
                row = groups[i - 1][-1]
                nb = sum(widget_size(_probe(x)) for x in row) + sum(widget_size(_probe(x)) for r in groups[i] for x in r)
                if height([row, *groups[i]]) <= FIXED_MAX and base + nb <= MAX_BYTES and len(groups[i]) < 16:
                    groups[i].insert(0, row)
                    groups[i - 1].pop()
                    moved_any = True
    out = []
    for g in groups:
        flat: list[_Spec] = []
        for y, row in enumerate(g):
            for s in row:
                s.y = y
                flat.append(s)
        out.append(flat)
    return out


_FIT_PAGE = SimpleNamespace(cols=12, rows=1, fixed=True)
_SPANS = (3, 4, 6, 12)


def _span_of(w: int, cols: int) -> int:
    """The FIXED span (12/6/4/3) nearest a grid cell width: a third is 4, a quarter is 3."""
    f = w / cols
    return 12 if f >= 0.66 else 6 if f > 0.4 else 4 if f > 0.28 else 3


def _over(s: _Spec) -> bool:
    return fit_label(_FIT_PAGE, _probe(replace(s, x=0, y=0, h=1))) is not None


_ABBREV = {"volume": "Vol", "channel": "Ch", "previous": "Prev", "settings": "Setup", "navigation": "Nav", "subtitles": "Subs"}


def _abbrev(label: str) -> str:
    return " ".join(_ABBREV.get(w.lower(), w) for w in label.split())


def _shorten_key(s: _Spec) -> bool:
    """A glyph key whose caption is cut gets a shorter one (Volume Up -> Vol Up) before it loses the caption."""
    if s.kind != "button" or s.w > 4 or s.shape != "card" or not s.icon or s.icon_only or s.icon in BRAND_COLORS:
        return False
    if not _over(s):
        return False
    short = _abbrev(s.label)
    if short == s.label:
        return False
    full, s.label = s.label, short
    if _over(s):
        s.label = full
        return False
    return True


def _caption_rows(specs: list[_Spec]) -> None:
    """In one row of glyph keys either every key has a caption or none does (logo keys excepted)."""
    rows: dict[int, list[_Spec]] = {}
    for s in specs:
        if s.kind == "button" and s.w <= 4 and s.shape == "card" and s.icon and s.icon not in BRAND_COLORS and s.nav is None:
            rows.setdefault(s.y, []).append(s)
    for row in rows.values():
        if any(s.label != _abbrev(s.label) for s in row) and any(s.label != s.cfg["label"] for s in row):
            for s in row:  # one wording across the row (Vol Up beside Vol Down)
                s.label = _abbrev(s.label)
        if any(s.icon_only for s in row) and not all(s.icon_only for s in row):
            for s in row:
                if s.cfg.get("icon_only") is not False:
                    s.icon_only = True


def _fit_class(s: _Spec) -> None:
    """Widen a button or tile until its label fits its class: key -> half card -> compact (2 lines) -> full
    row. A button with a glyph stops at the half card and falls back to icon-only; one without goes on."""
    if s.kind == "label" or s.shape in ("row", "pill", "monogram") or s.icon_only or not s.label:
        return
    if s.icon in BRAND_COLORS and s.w <= 4 and s.shape == "card" and "icon_only" not in s.cfg:
        return  # a brand logo key
    if not _over(s):
        return
    if _shorten_key(s):
        return
    if s.kind == "tile":
        if s.w < 12:
            s.w = 12 if _over(replace(s, w=6)) else 6
        return
    start = s.w
    for sp in [t for t in _SPANS if t >= start]:
        s.w = sp
        if not _over(s):
            return
        if sp == 6 and not s.cfg.get("compact"):
            s.compact = True
            if not _over(s):
                return
            s.compact = False
        if sp == 6 and s.icon:
            break
    if s.icon:
        s.w, s.compact = start, False  # icon-only (set by _default_icon_only) keeps the small class
        return
    s.w, s.compact = 12, False
    s.shape = "row"


def _pack(specs: list[_Spec]) -> None:
    """Row-major, left to right, each span on its own boundary; a label or full span has a row to itself."""
    x = y = 0
    for s in specs:
        s.h = 1
        if x and (s.w == 12 or x % s.w or x + s.w > 12):
            y, x = y + 1, 0
        s.x, s.y = x, y
        x += s.w
        if x >= 12:
            y, x = y + 1, 0


def _regrid(specs: list[_Spec], cols: int, rows: int) -> list[_Spec]:
    """Convert a hand-placed grid page to the FIXED layout. Returns the specs in placement order.

    Cell width picks the class: full width a row (a hero card when the cell is tall), a half a card (compact
    when there are many) or a tile, a third a key, a quarter a glyph key; pills stay pad keys. Labels that
    do not fit widen the widget (see `_fit_class`)."""
    cell = 288 / max(rows, 1)
    for s in specs:
        sp = _span_of(s.w, cols)
        explicit = s.cfg.get("shape")
        if s.kind == "label" or s.nav is not None or s.shape == "row":
            sp, s.shape = 12, ("row" if s.kind != "label" else s.shape)
        elif s.kind == "button" and sp == 12 and not explicit:
            hero = cell * s.h >= 140
            s.shape = "card" if hero else "row"
        s.w = sp
        _fit_class(s)
    ordered = sorted(specs, key=lambda s: (s.y, s.x, s.order))
    halves = [s for s in ordered if s.kind == "button" and s.shape == "card" and s.w == 6]
    if len(halves) > 6:
        for s in halves:
            s.compact = True
    _pack(ordered)
    return ordered


def _is_off(s: _Spec) -> bool:
    a = s.cfg.get("action")
    return s.kind == "button" and bool(a) and a.get("type") == "activity" and a.get("activity") == "off"


def _units(model: Model, act_ids: list[str]) -> list[_Unit]:
    names = {a["id"]: a.get("name", a["id"]) for a in model["activities"]}
    dev_icons = {d["id"]: d["icon"] for d in model["devices"] if d.get("icon")}
    dev_name = {d["id"]: d.get("name") or d["id"] for d in model["devices"]}
    act_devs = {a["id"]: [dev_name[x["device"]] for x in a["devices"] if x["device"] in dev_name] for a in model["activities"]}
    dev_by_name = {}
    for d in model["devices"]:
        dev_by_name[d["id"].casefold()] = d["id"]
        dev_by_name[d.get("name", d["id"]).casefold()] = d["id"]
    units: list[_Unit] = []
    for pi, p in enumerate(model["pages"]):
        in_list = "list" in p
        specs = [_spec(w, act_ids, names, in_list, dev_icons, dev_by_name, act_devs) for w in p["widgets"]]
        for k, s in enumerate(specs):
            s.order = k
        cols, rows = p["cols"], p["rows"]
        convert = p.get("layout") not in ("fixed", "grid")  # every page is FIXED unless it opts out
        fixed = convert or p.get("layout") == "fixed"
        if fixed and in_list:  # one full-width row each, one grid row per widget
            half = convert and len(specs) >= 7 and all(
                s.kind == "button" and s.nav is None and "shape" not in s.cfg for s in specs
            )
            if half:  # many short items: compact half cards, 8 a page
                for i, s in enumerate(specs):
                    s.x, s.y, s.w, s.h, s.shape, s.compact = (i % 2) * 6, i // 2, 6, 1, "card", True
                half = not any(_over(replace(s, x=0)) for s in specs)
            if not half:
                for s in specs:
                    if s.compact and s.shape == "card" and "compact" not in s.cfg:
                        s.shape, s.compact = "row", False  # the compact trial did not fit
                for i, s in enumerate(specs):
                    s.x, s.y, s.w, s.h = 0, i, 12, 1
            rows = max(1, min(16, len(specs) if not half else (len(specs) + 1) // 2))
        title = p.get("title", "")
        if not title and not in_list and rows >= 2:
            lead = _lead_title(specs, cols)
            if lead is not None:  # becomes the header title; the rows below shift up by one
                specs.remove(lead)
                title, rows = lead.label, rows - 1
                for s in specs:
                    s.y -= 1
        placed: list[_Spec] | None = None
        if convert and not in_list:
            placed = _regrid(specs, cols, rows)
            cols, rows = 12, max(1, max(s.y for s in specs) + 1) if specs else 1
        if not in_list:
            for s in specs:
                if fixed and not convert:
                    _shorten_key(s)
                _default_icon_only(s, cols, rows, fixed)
            if fixed:
                _caption_rows(specs)
        common: dict[str, Any] = {
            "cfg_index": pi,
            "cfg_id": p["id"],
            "cols": 12 if fixed else cols,
            "rows": rows,
            "title": title,
            "kind": p.get("kind", "normal"),
            "back": _default_back(model, p),
            "fixed": fixed,
            "act": act_ids.index(p["activity"]) + 1 if p.get("activity") in act_ids else 0,
        }
        if fixed:
            base = HEADER_V2 + len(title.encode())
            groups = _split_fixed(specs, p, pi, base)
            if placed is not None:
                halves = [s for s in placed if s.kind == "button" and s.shape == "card" and s.w == 6 and not s.compact]
                if len(groups) > 1 and halves:  # compact (56 px) half cards may save the split
                    for s in halves:
                        s.compact = True
                    _pack(placed)
                    tight = _split_fixed(placed, p, pi, base)
                    if len(tight) < len(groups):
                        groups = tight
                    else:
                        for s in halves:
                            s.compact = False
                        _pack(placed)
                        groups = _split_fixed(placed, p, pi, base)
                off = next((s for s in placed if _is_off(s)), None)
                while off is not None and not any(x is off for x in groups[0]) and placed.index(off) > 0:
                    placed.remove(off)  # All Off belongs on page 1: nudge it up the reading order
                    placed.insert(max(0, len(groups[0]) - 1), off)
                    _pack(placed)
                    groups = _split_fixed(placed, p, pi, base)
                for g in groups:
                    g.sort(key=lambda s: s.order)
            if len(groups) > MAX_PAGE_GROUP:
                raise CompileError(
                    f"page {p['id']!r}: needs {len(groups)} pages (at most {MAX_PAGE_GROUP});"
                    " shorten it or split it across pages",
                    ["pages", pi],
                )
            for gi, g in enumerate(groups, 1):
                u = _Unit(specs=g, **{**common, "rows": max([s.y for s in g], default=0) + 1})
                if len(groups) > 1:
                    u.seq = (gi, len(groups))
                units.append(u)
            continue
        if not in_list:
            units.append(_Unit(specs=specs, **common))
            continue
        base = HEADER_V2 + len(title.encode())
        groups: list[list[_Spec]] = [[]]
        size = base
        for s in specs:  # one full-width row each, split by rows and by bytes
            s.x, s.w, s.h = 0, cols, 1
            n = widget_size(_probe(s))
            if base + n > MAX_BYTES:
                raise CompileError(
                    f"page {p['id']!r}: one list row is {n} bytes; shorten its label", ["pages", pi]
                )
            if len(groups[-1]) == rows or size + n > MAX_BYTES:
                groups.append([])
                size = base
            s.y = len(groups[-1])
            groups[-1].append(s)
            size += n
        if len(groups) > MAX_PAGE_GROUP:
            raise CompileError(
                f"page {p['id']!r}: the list needs {len(groups)} pages (at most {MAX_PAGE_GROUP});"
                " shorten it or split it across pages",
                ["pages", pi],
            )
        for gi, g in enumerate(groups, 1):
            u = _Unit(specs=g, **common)
            if len(groups) > 1:
                u.seq = (gi, len(groups))
            units.append(u)
    return units


def build_pages(
    model: Model,
    act_ids: list[str],
    names: dict[int, str],
    keys: dict[str, Any],
    theme: int = 0,
    apps: list[AppBinding] | None = None,
) -> tuple[list[Page], list[str], list[Tile]]:
    """v2 page models (gen 0), the config id behind each wire page, and the tiles.

    Fills `names` and `keys` for buttons and tiles. Only those take global soft-key ids; labels
    and nav widgets get page-local ids and never reach the hub.
    """
    units = _units(model, act_ids)
    if len(units) > MAX_PAGES:
        err = CompileError(f"too many pages: {len(units)} (the remote holds at most {MAX_PAGES}); delete pages or shorten lists that split into several")
        err.count = len(units)
        raise err
    first: dict[int, int] = {}
    for i, u in enumerate(units):
        first.setdefault(u.cfg_index, i)
    cfg = model["pages"]
    by_id = {p["id"]: i for i, p in enumerate(cfg)}
    home = next((i for i, p in enumerate(cfg) if p.get("kind") == "home"), None)

    def target(u: _Unit, nav: str) -> int:
        rel = {"next": u.cfg_index + 1, "prev": u.cfg_index - 1, "home": home}
        want = rel[nav] if nav in rel else by_id.get(nav)
        if want not in first:
            raise CompileError(
                f"page {u.cfg_id!r}: nav target {nav!r} does not exist", ["pages", u.cfg_index]
            )
        return first[want]

    pages: list[Page] = []
    tiles: list[Tile] = []
    wire = 0
    for pid, u in enumerate(units):
        if len(u.specs) > MAX_WIDGETS:
            raise CompileError(
                f"page {u.cfg_id!r}: too many widgets: {len(u.specs)} (max {MAX_WIDGETS})",
                ["pages", u.cfg_index],
            )
        gids: dict[int, int] = {}
        for k, s in enumerate(u.specs):
            if s.kind == "label" or s.nav is not None:
                continue
            if wire >= MAX_SOFT_KEYS:
                raise CompileError(
                    f"too many widgets across all pages: more than {MAX_SOFT_KEYS} soft keys",
                    ["pages", u.cfg_index],
                )
            gids[k] = wire
            code = SOFT_KEY_BASE + wire
            names[code] = f"tap_{s.cfg['id']}"
            if s.kind == "button":
                keys[str(code)] = s.cfg["action"]
                if apps is not None and s.cfg["action"]["type"] != "activity":
                    ids = s.cfg.get("app_id") or infer_app_ids(s.label, s.cfg.get("icon"))
                    ent, act = _apps_entity(model, cfg[u.cfg_index], s.cfg["action"]) if ids else (None, None)
                    if ent:
                        apps.append(AppBinding(pid, wire, tuple(ids), ent, act))
            else:
                tiles.append(Tile(pid, wire, s.cfg["entity"], s.label))
                tap = s.cfg.get("tap")
                if tap is None or tap == "toggle":
                    tap = default_tap(s.cfg["entity"])
                if tap not in (None, "none"):
                    keys[str(code)] = (
                        tap  # an HA tap never reaches the hub (compiler splits it off)
                    )
            wire += 1
        free = (i for i in range(128) if i not in set(gids.values()))
        ws = []
        for k, s in enumerate(u.specs):
            wid = gids[k] if k in gids else next(free)
            bind = target(u, s.nav) if s.nav is not None else s.bind
            ws.append(
                Widget(
                    s.kind,
                    wid,
                    s.x,
                    s.y,
                    s.w,
                    s.h,
                    s.label,
                    s.role,
                    s.shape,
                    s.icon_only,
                    s.icon,
                    bind,
                    s.color,
                    s.on_color,
                    s.trailing,
                    s.align,
                    s.compact,
                )
            )
        pages.append(
            Page(
                pid,
                u.cols,
                u.rows,
                tuple(ws),
                2,
                0,
                len(units),
                u.kind,
                u.back,
                u.seq[0],
                u.seq[1],
                u.act,
                u.title,
                theme,
                u.fixed,
            )
        )
    return pages, [u.cfg_id for u in units], tiles


def _over_msg(cid: str, p: Page, over: int) -> str:
    """The over-budget message; with colours on the page it says what they cost."""
    costs = [widget_cost(w)["color"] for w in p.widgets]
    free = sum(c.startswith("#") for w in p.widgets for c in (w.color, w.on_color) if c)
    if not sum(costs):
        return f"page {cid!r}: {over} byte{'s' * (over != 1)} over; shorten labels, use icon_only, or make it a list"
    bytes_ = sum(1 for c in costs if c)
    return (
        f"page {cid!r}: {over} byte{'s' * (over != 1)} over ({free} custom colour{'s' * (free != 1)} = {2 * free} B,"
        f" {bytes_} colour byte{'s' * (bytes_ != 1)} = {bytes_} B); use palette colours,"
        " shorten labels, use icon_only, or make it a list"
    )


def encode_pages(
    pages: list[Page],
    cfg_ids: list[str],
    seed: bytes,
    problems: list[tuple[list[Any], str]] | None = None,
) -> tuple[list[Page], list[bytes], list[bytes]]:
    """Fill gen (low byte of a CRC over `seed` - the hub config plus a user theme's wire bytes - and every page layout) and encode both formats.

    Returns (models with gen, v1 bytes, v2 bytes). A page over the byte budget is a CompileError
    with the exact overage; nothing is ever truncated. With `problems` (previews) nothing is
    raised: the failing pages are reported there, keep their model, and have empty bytes.
    """
    if not pages:
        return [], [], []
    order = {cid: i for i, cid in enumerate(dict.fromkeys(cfg_ids))}
    bad: set[int] = set()

    def reject(k: int, msg: str) -> None:
        path = ["pages", order[cfg_ids[k]]]
        if problems is None:
            raise CompileError(msg, path)
        problems.append((path, msg))
        bad.add(k)

    layout = b""
    for k, (p, cid) in enumerate(zip(pages, cfg_ids, strict=True)):
        over = page_size(p, 2) - MAX_BYTES
        if over > 0:
            reject(k, _over_msg(cid, p, over))
            continue
        try:
            layout += encode(p, 2)
        except ValueError as exc:
            reject(k, f"page {cid!r}: {exc}")
    gen = zlib.crc32(seed + layout) & 0xFF
    pages = [replace(p, gen=gen) for p in pages]
    v1: list[bytes] = []
    v2: list[bytes] = []
    for k, (p, cid) in enumerate(zip(pages, cfg_ids, strict=True)):
        if k in bad:
            v1.append(b"")
            v2.append(b"")
            continue
        try:
            b2, b1 = encode(p, 2), encode(p, 1)
        except ValueError as exc:
            reject(k, f"page {cid!r}: {exc}")
            b2 = b1 = b""
        v2.append(b2)
        v1.append(b1)
    return pages, v1, v2


def overflow_warnings(pages: list[Page], cfg_ids: list[str], model: Model) -> list[dict[str, Any]]:
    """Non-fatal: one warning per button or tile whose label the remote will cut with "..."."""
    order = {p["id"]: i for i, p in enumerate(model["pages"])}
    out: list[dict[str, Any]] = []
    for pg, cid in zip(pages, cfg_ids, strict=True):
        for w in pg.widgets:
            f = fit_label(pg, w)
            if f:
                out.append(
                    {
                        "path": ["pages", order[cid]],
                        "msg": f"page {cid!r}: {w.label!r} is {f[2]} px too wide ({f[1]} px in {f[0]});"
                        " shorten it or use an icon",
                        "severity": "warning",
                    }
                )
    return out
