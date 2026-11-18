"""Model pages -> wire pages: inference, title rule, list splitting, ids, v1 + v2 encodings.

Everything the remote used to guess is decided here, once; explicit config always wins. The result
is one list of v2 `Page` models; `encode(page, 1)` and `encode(page, 2)` derive both wire formats.
No Home Assistant imports.
"""

from __future__ import annotations

import re
import zlib
from dataclasses import dataclass, replace
from typing import Any

from .config_model import Model
from .harmony_proto import SOFT_KEY_BASE
from .icons import ICON_IDS, ICONS
from .page_codec import (
    HEADER_V2,
    MAX_BYTES,
    MAX_WIDGETS,
    Page,
    Widget,
    encode,
    page_size,
    widget_size,
)

MAX_SOFT_KEYS = 256 - SOFT_KEY_BASE  # widget wire ids 0..127
MAX_PAGE_GROUP = 15  # seq count is 4 bits
MAX_PAGES = 255

_TOKEN = re.compile(r"[a-z0-9+\-]+")
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
}
_NAV_ICON = {"next": "chev_right", "prev": "chev_left", "home": "home"}


class CompileError(ValueError):
    """The config is valid YAML but does not fit the hub or the remote.

    `path` (when known) is the config path of the culprit, e.g. ["pages", 2]."""

    def __init__(self, msg: str, path: list[Any] | None = None) -> None:
        super().__init__(msg)
        self.path = path or []


@dataclass(frozen=True)
class Tile:
    page: int  # wire page id
    widget: int  # wire id
    entity_id: str
    label: str = ""


def icon_for_text(text: str) -> int:
    """Stock icon whose keywords share the most words with `text` (lowest id on ties); 0 if none."""
    toks: set[str] = set()
    for t in _TOKEN.findall(text.lower()):
        toks.add(t)
        if len(t) > 3 and t.endswith("s"):
            toks.add(t[:-1])
    best, best_score = 0, 0
    for ic in ICONS:
        score = len(toks & set(ic.keywords))
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
    x: int = 0
    y: int = 0
    w: int = 1
    h: int = 1


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


def _spec(w: dict[str, Any], act_ids: list[str], act_names: dict[str, str], in_list: bool) -> _Spec:
    kind, label = w["type"], w["label"]
    role = w.get("role", "default")
    icon: int | None = None
    if w.get("icon") is not None:
        icon = 0 if w["icon"] == "none" else ICON_IDS[w["icon"]]
    bind: int | None = None
    nav = w.get("nav")
    if nav is not None:
        role = "nav"
        if icon is None:
            icon = ICON_IDS[_NAV_ICON.get(nav, "chev_right")]
    elif kind == "button":
        action = w["action"]
        target = action["activity"] if action["type"] == "activity" else None
        if target == "off" or _DANGER_LABEL.match(label.lower()):
            role = w.get("role", "danger")
            if icon is None:
                icon = ICON_IDS["power"]
        elif target is not None:
            bind = act_ids.index(target) if target in act_ids else None  # unknown: validated elsewhere
            if icon is None:
                icon = icon_for_text(f"{act_names.get(target, target)} {target}")
        if icon is None:
            icon = icon_for_text(label)
    elif kind == "tile" and icon is None:
        icon = ICON_IDS.get(_DOMAIN_ICON.get(w["entity"].split(".")[0], ""), 0)
    return _Spec(
        w,
        kind,
        label,
        role,
        w.get("shape", "row" if in_list else "card"),
        bool(w.get("icon_only", False)),
        icon or 0,
        bind,
        nav,
        w.get("x", 0),
        w.get("y", 0),
        w["w"],
        w["h"],
    )


def _probe(s: _Spec) -> Widget:
    return Widget(
        s.kind, 0, s.x, s.y, s.w, s.h, s.label, s.role, s.shape, s.icon_only, s.icon, s.bind
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


def _units(model: Model, act_ids: list[str]) -> list[_Unit]:
    names = {a["id"]: a.get("name", a["id"]) for a in model["activities"]}
    units: list[_Unit] = []
    for pi, p in enumerate(model["pages"]):
        in_list = "list" in p
        specs = [_spec(w, act_ids, names, in_list) for w in p["widgets"]]
        cols, rows = p["cols"], p["rows"]
        title = p.get("title", "")
        if not title and not in_list and rows >= 2:
            lead = _lead_title(specs, cols)
            if lead is not None:  # becomes the header title; the rows below shift up by one
                specs.remove(lead)
                title, rows = lead.label, rows - 1
                for s in specs:
                    s.y -= 1
        common: dict[str, Any] = {
            "cfg_index": pi,
            "cfg_id": p["id"],
            "cols": cols,
            "rows": rows,
            "title": title,
            "kind": p.get("kind", "normal"),
            "back": p.get("back", False),
            "act": act_ids.index(p["activity"]) + 1 if p.get("activity") in act_ids else 0,
        }
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
    model: Model, act_ids: list[str], names: dict[int, str], keys: dict[str, Any]
) -> tuple[list[Page], list[str], list[Tile]]:
    """v2 page models (gen 0), the config id behind each wire page, and the tiles.

    Fills `names` and `keys` for buttons and tiles. Only those take global soft-key ids; labels
    and nav widgets get page-local ids and never reach the hub.
    """
    units = _units(model, act_ids)
    if len(units) > MAX_PAGES:
        raise CompileError(f"too many pages: {len(units)} (at most {MAX_PAGES})")
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
            raise CompileError(f"page {u.cfg_id!r}: nav target {nav!r} does not exist", ["pages", u.cfg_index])
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
            else:
                tiles.append(Tile(pid, wire, s.cfg["entity"], s.label))
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
            )
        )
    return pages, [u.cfg_id for u in units], tiles


def encode_pages(
    pages: list[Page],
    cfg_ids: list[str],
    seed: bytes,
    problems: list[tuple[list[Any], str]] | None = None,
) -> tuple[list[Page], list[bytes], list[bytes]]:
    """Fill gen (low byte of a CRC over `seed` and every page layout) and encode both formats.

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
            reject(k, f"page {cid!r}: {over} bytes over; shorten labels, use icon_only, or make it a list")
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
