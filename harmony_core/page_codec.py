"""Remote page encoder/decoder, formats v1 and v2 (docs/proto/page-format.md, docs/plans/
2026-11-18-page-format-v2.md). One model, one decoder for both versions. No Home Assistant imports."""

from __future__ import annotations

from dataclasses import dataclass, replace
from typing import Any

from .theme_math import PALETTE_NAMES, from565, to565

VERSION = 1  # the original format; encode_page() still produces it
VERSION_2 = 2
MAX_WIDGETS = 32
MAX_LABEL = 24
MAX_TITLE = 24
# Must match HP_PAGE_MAX_BYTES in remote/page/page.h: one RF object carries at most 254 body
# bytes and page_push uses 3 for its header, so a 252..255-byte page compiles but cannot be sent.
MAX_BYTES = 251
MAX_GRID = 16
HEADER_V1 = 5
HEADER_V2 = 11
WIDGET_BYTES = 5
KINDS = {"button": 0, "tile": 1, "label": 2}
_KIND_NAMES = {v: k for k, v in KINDS.items()}
PAGE_KINDS = ("normal", "home", "favorites")
ROLES = ("default", "primary", "secondary", "danger", "nav")
SHAPES = ("card", "row", "pill", "monogram")
ICON_STOCK_MAX = 159
ICON_CUSTOM_MAX = 254

# widget kind byte
_HAS_STYLE, _HAS_ICON, _HAS_BIND, _HAS_COLOR = 0x10, 0x20, 0x40, 0x80
_KIND_RESERVED = 0x0C  # bits 2, 3
_ICON_ONLY = 0x20
_TRAILING = 0x40  # style bit 6: trailing chevron (nav role only)
_BACK = 0x04
_THEME_SHIFT, _THEME_MASK = (
    3,
    0x38,
)  # pflags bits 3-5: theme preset id 0..7 (7 = custom, delivered separately)
THEME_MAX = 7
_RGB = 15  # colour nibble: 2-byte big-endian RGB565 follows


@dataclass(frozen=True)
class Widget:
    kind: str  # "button" | "tile" | "label"
    id: int  # 0..127
    x: int
    y: int
    w: int
    h: int
    label: str = ""
    role: str = "default"  # v2
    shape: str = "card"  # v2
    icon_only: bool = False  # v2
    icon: int = 0  # v2: 0 none, 1..159 stock, 160..254 custom
    bind: int | None = None  # v2: nav -> target page id; otherwise activity index
    color: str | None = (
        None  # v2: fill; palette name or "#RRGGBB" (snapped to RGB565); None = role default
    )
    on_color: str | None = None  # v2, tiles only: fill when on; same forms
    trailing: bool = False  # v2: draw a trailing chevron (nav role only)


@dataclass(frozen=True)
class Page:
    page_id: int
    cols: int
    rows: int
    widgets: tuple[Widget, ...]
    version: int = 1
    gen: int = 0  # v2
    total: int = 0  # v2 (0 = unknown, v1)
    kind: str = "normal"  # v2: normal | home | favorites
    back: bool = False  # v2
    seq_idx: int = 0  # v2: 0 = not paged; else 1..seq_cnt
    seq_cnt: int = 0  # v2: 0 or >= 2
    act: int = 0  # v2: 0, or activity index + 1
    title: str = ""  # v2: "" -> the remote shows "Harmony"
    theme: int = 0  # v2: preset id 0..7 (0 default, 7 custom)


def _encode_text(s: str, limit: int, what: str) -> bytes:
    if "\x00" in s:
        raise ValueError(f"{what} contains NUL")
    b = s.encode()
    if len(b) > limit:
        raise ValueError(f"{what} exceeds {limit} bytes")
    return b


def _overlap(a: Widget, b: Widget) -> bool:
    return a.x < b.x + b.w and b.x < a.x + a.w and a.y < b.y + b.h and b.y < a.y + a.h


def _validate(page: Page) -> None:
    v2 = page.version == VERSION_2
    if page.version not in (1, 2):
        raise ValueError(f"unsupported page format version {page.version}")
    if not 0 <= page.page_id <= 255:
        raise ValueError(f"page id {page.page_id} out of range 0..255")
    if not (1 <= page.cols <= MAX_GRID and 1 <= page.rows <= MAX_GRID):
        raise ValueError(f"grid {page.cols}x{page.rows} out of range 1..{MAX_GRID}")
    if len(page.widgets) > MAX_WIDGETS:
        raise ValueError(f"too many widgets: {len(page.widgets)} (max {MAX_WIDGETS})")
    if v2:
        _validate_header_v2(page)
    seen: list[Widget] = []
    for w in page.widgets:
        if w.kind not in KINDS:
            raise ValueError(f"unknown widget kind {w.kind!r}")
        if not 0 <= w.id <= 127:
            raise ValueError(f"widget id {w.id} out of range 0..127")
        if not (1 <= w.w <= 15 and 1 <= w.h <= 15):
            raise ValueError(f"widget {w.id} size {w.w}x{w.h} out of range 1..15")
        if not (0 <= w.x and 0 <= w.y and w.x + w.w <= page.cols and w.y + w.h <= page.rows):
            raise ValueError(f"widget {w.id} lies outside the {page.cols}x{page.rows} grid")
        _encode_text(w.label, MAX_LABEL, f"widget {w.id} label")
        if v2:
            _validate_widget_v2(page, w)
        for o in seen:
            if o.id == w.id:
                raise ValueError(f"duplicate widget id {w.id}")
            if _overlap(w, o):
                raise ValueError(f"widgets {o.id} and {w.id} overlap")
        seen.append(w)


def _validate_header_v2(page: Page) -> None:
    if not 1 <= page.total <= 255:
        raise ValueError(f"page total {page.total} out of range 1..255")
    if page.page_id >= page.total:
        raise ValueError(f"page id {page.page_id} is not below total {page.total}")
    if not 0 <= page.gen <= 255:
        raise ValueError(f"gen {page.gen} out of range 0..255")
    if page.kind not in PAGE_KINDS:
        raise ValueError(f"unknown page kind {page.kind!r}")
    if not 0 <= page.theme <= THEME_MAX:
        raise ValueError(f"theme {page.theme} out of range 0..{THEME_MAX}")
    if not 0 <= page.act <= 255:
        raise ValueError(f"act {page.act} out of range 0..255")
    _encode_text(page.title, MAX_TITLE, "title")
    if (page.seq_idx, page.seq_cnt) != (0, 0):
        i, c = page.seq_idx, page.seq_cnt
        if not (2 <= c <= 15 and 1 <= i <= c):
            raise ValueError(f"bad paging {i}/{c}")
        if page.page_id - i + 1 < 0 or page.page_id + c - i >= page.total:
            raise ValueError(
                f"page group of page {page.page_id} ({i}/{c}) leaves 0..{page.total - 1}"
            )


def _validate_widget_v2(page: Page, w: Widget) -> None:
    if w.role not in ROLES:
        raise ValueError(f"widget {w.id} role {w.role!r} unknown")
    if w.shape not in SHAPES:
        raise ValueError(f"widget {w.id} shape {w.shape!r} unknown")
    if not 0 <= w.icon <= ICON_CUSTOM_MAX:
        raise ValueError(f"widget {w.id} icon {w.icon} out of range 0..{ICON_CUSTOM_MAX}")
    if w.role == "nav":
        if w.bind is None:
            raise ValueError(f"nav widget {w.id} needs a target page")
        if w.bind >= page.total:
            raise ValueError(f"nav widget {w.id} targets page {w.bind} (total {page.total})")
    if w.bind is not None and not 0 <= w.bind <= 255:
        raise ValueError(f"widget {w.id} bind {w.bind} out of range 0..255")
    _color_parts(w.color, f"widget {w.id} color")
    if _color_parts(w.on_color, f"widget {w.id} on_color")[0] and w.kind != "tile":
        raise ValueError(f"widget {w.id} on_color is only valid on tiles")


def _color_parts(c: str | None, what: str) -> tuple[int, bytes]:
    """(colour nibble, RGB565 bytes): 0 none, 1..12 palette index, 15 + 2 bytes for "#RRGGBB"."""
    if c is None:
        return 0, b""
    if c in PALETTE_NAMES:
        return PALETTE_NAMES.index(c) + 1, b""
    if (
        isinstance(c, str)
        and len(c) == 7
        and c[0] == "#"
        and all(ch in "0123456789abcdefABCDEF" for ch in c[1:])
    ):
        return _RGB, to565(int(c[1:], 16)).to_bytes(2, "big")
    raise ValueError(f"{what} {c!r} is neither a palette name nor #RRGGBB")


def _color_bytes(w: Widget) -> bytes:
    """The HAS_COLOR payload (colour byte + RGB565 values), or b"" when the widget has no colour."""
    f, fb = _color_parts(w.color, "color")
    o, ob = _color_parts(w.on_color, "on_color")
    return bytes([f | o << 4]) + fb + ob if f | o else b""


def _color_name(nib: int, data: bytes, off: int) -> tuple[str | None, int]:
    if nib == 0:
        return None, off
    if nib == _RGB:
        return f"#{from565(int.from_bytes(data[off : off + 2], 'big')):06X}", off + 2
    return PALETTE_NAMES[nib - 1], off


def widget_cost(w: Widget) -> dict[str, int]:
    """Exact v2 byte cost of one widget by part; the values sum to widget_size(w).

    base = kind, id, x/y, w/h and the label length byte (5); label = label bytes; style/icon/bind
    = 0 or 1; color = 0 (none), 1 (palette), 3 (one rgb) or 5 (two rgb)."""
    style = (
        ROLES.index(w.role)
        | SHAPES.index(w.shape) << 3
        | (_ICON_ONLY if w.icon_only else 0)
        | (_TRAILING if w.trailing else 0)
    )
    return {
        "base": WIDGET_BYTES,
        "label": len(w.label.encode()),
        "style": 1 if style else 0,
        "icon": 1 if w.icon else 0,
        "bind": 1 if w.bind is not None else 0,
        "color": len(_color_bytes(w)),
    }


def _widget_bytes(w: Widget, v2: bool) -> bytes:
    label = w.label.encode()
    kind = KINDS[w.kind]
    extra = b""
    if v2:
        style = (
            ROLES.index(w.role)
        | SHAPES.index(w.shape) << 3
        | (_ICON_ONLY if w.icon_only else 0)
        | (_TRAILING if w.trailing else 0)
        )
        if style:
            kind |= _HAS_STYLE
            extra += bytes([style])
        if w.icon:
            kind |= _HAS_ICON
            extra += bytes([w.icon])
        if w.bind is not None:
            kind |= _HAS_BIND
            extra += bytes([w.bind])
        if cb := _color_bytes(w):
            kind |= _HAS_COLOR
            extra += cb
    head = bytes([kind, w.id, (w.x << 4) | w.y, (w.w << 4) | w.h])
    return head + extra + bytes([len(label)]) + label


def v1_view(page: Page) -> Page:
    """The v1 rendition of a page: no extras, nav widgets dropped, title as a row-0 label."""
    ws = [
        Widget(w.kind, w.id, w.x, w.y, w.w, w.h, w.label) for w in page.widgets if w.role != "nav"
    ]
    rows = page.rows
    if page.title and page.cols <= 15 and rows < MAX_GRID and len(ws) < MAX_WIDGETS:
        free = next(i for i in range(128) if i not in {w.id for w in ws})
        ws = [replace(w, y=w.y + 1) for w in ws]
        ws.insert(0, Widget("label", free, 0, 0, page.cols, 1, page.title))
        rows += 1
    return Page(page.page_id, page.cols, rows, tuple(ws))


def encode(page: Page, version: int = 2) -> bytes:
    """Encode `page` as v1 or v2; raises ValueError for anything the remote would reject."""
    if version == 1:
        page = v1_view(page)
    elif version == 2:
        page = replace(page, version=2)
    else:
        raise ValueError(f"unsupported page format version {version}")
    _validate(page)
    v2 = version == 2
    if v2:
        t = page.title.encode()
        seq = (page.seq_idx << 4) | page.seq_cnt
        pflags = (
            PAGE_KINDS.index(page.kind) | (_BACK if page.back else 0) | page.theme << _THEME_SHIFT
        )
        out = bytearray(
            [2, page.page_id, page.cols, page.rows, len(page.widgets), page.gen, page.total]
            + [pflags, seq, page.act, len(t)]
        )
        out += t
    else:
        out = bytearray([1, page.page_id, page.cols, page.rows, len(page.widgets)])
    for w in page.widgets:
        out += _widget_bytes(w, v2)
    if len(out) > MAX_BYTES:
        raise ValueError(f"encoded page is {len(out)} bytes (max {MAX_BYTES})")
    return bytes(out)


def encode_page(page_id: int, cols: int, rows: int, widgets: list[Widget]) -> bytes:
    """Encode a v1 page (extras on the widgets are ignored)."""
    return encode(Page(page_id, cols, rows, tuple(widgets)), 1)


def widget_size(w: Widget, version: int = 2) -> int:
    """Encoded length of one widget (label included)."""
    return len(_widget_bytes(w, version == 2))


def page_size(page: Page, version: int = 2) -> int:
    """Encoded length without the budget check (to compute an exact overage)."""
    if version == 1:
        page = v1_view(page)
    n = HEADER_V1 if version == 1 else HEADER_V2 + len(page.title.encode())
    for w in page.widgets:
        n += len(_widget_bytes(w, version == 2))
    return n


def _text(data: bytes, what: str) -> str:
    try:
        s = data.decode()
    except UnicodeDecodeError as e:
        raise ValueError(f"{what} is not valid UTF-8") from e
    if "\x00" in s:
        raise ValueError(f"{what} contains NUL")
    return s


def decode_page(data: bytes) -> Page:
    """Decode and validate a v1 or v2 page; raises ValueError on malformed input."""
    if len(data) > MAX_BYTES:
        raise ValueError(f"page data is {len(data)} bytes (max {MAX_BYTES})")
    if len(data) < 1:
        raise ValueError("truncated header")
    ver = data[0]
    if ver not in (1, 2):
        raise ValueError(f"unsupported page format version {ver}")
    v2 = ver == 2
    hdr = HEADER_V2 if v2 else HEADER_V1
    if len(data) < hdr:
        raise ValueError("truncated header")
    count = data[4]
    if count > MAX_WIDGETS:
        raise ValueError(f"too many widgets: {count} (max {MAX_WIDGETS})")
    extra: dict[str, Any] = {}
    off = hdr
    if v2:
        pflags, seq, act, tlen = data[7:11]
        if pflags & 3 == 3 or pflags & ~(3 | _BACK | _THEME_MASK):
            raise ValueError(f"reserved page flag bits set ({pflags:#04x})")
        if tlen > MAX_TITLE:
            raise ValueError(f"title exceeds {MAX_TITLE} bytes")
        if len(data) - off < tlen:
            raise ValueError("truncated title")
        title = _text(data[off : off + tlen], "title")
        off += tlen
        extra = {
            "version": 2,
            "gen": data[5],
            "total": data[6],
            "kind": PAGE_KINDS[pflags & 3],
            "back": bool(pflags & _BACK),
            "seq_idx": seq >> 4,
            "seq_cnt": seq & 15,
            "act": act,
            "title": title,
            "theme": (pflags & _THEME_MASK) >> _THEME_SHIFT,
        }
    widgets = []
    for _ in range(count):
        if len(data) - off < WIDGET_BYTES:
            raise ValueError("truncated widget")
        kind = data[off]
        off += 1
        if not v2 and kind > 2:
            raise ValueError(f"unknown widget kind {kind}")
        if v2 and (kind & _KIND_RESERVED or kind & 3 == 3):
            raise ValueError(f"bad widget kind byte {kind:#04x}")
        role, shape, icon_only, icon, bind = "default", "card", False, 0, None
        trailing = False
        color = on_color = None
        wid, xy, wh = data[off : off + 3]
        off += 3
        if v2:
            need = sum(bool(kind & f) for f in (_HAS_STYLE, _HAS_ICON, _HAS_BIND, _HAS_COLOR)) + 1
            if len(data) - off < need:
                raise ValueError("truncated widget")
            if kind & _HAS_STYLE:
                s = data[off]
                off += 1
                if s & 7 > 4 or s & 0x80 or (s & _TRAILING and s & 7 != 4):
                    raise ValueError(f"bad style byte {s:#04x}")
                role, shape, icon_only = ROLES[s & 7], SHAPES[(s >> 3) & 3], bool(s & _ICON_ONLY)
                trailing = bool(s & _TRAILING)
            if kind & _HAS_ICON:
                icon = data[off]
                off += 1
                if icon == 255:
                    raise ValueError("icon 255 is reserved")
            if kind & _HAS_BIND:
                bind = data[off]
                off += 1
            if kind & _HAS_COLOR:
                cb = data[off]
                off += 1
                fn, on = cb & 15, cb >> 4
                if cb == 0 or {fn, on} & {13, 14}:
                    raise ValueError(f"bad colour byte {cb:#04x}")
                if on and kind & 3 != KINDS["tile"]:
                    raise ValueError("on colour on a non-tile widget")
                if len(data) - off < 2 * ((fn == _RGB) + (on == _RGB)) + 1:
                    raise ValueError("truncated widget")
                color, off = _color_name(fn, data, off)
                on_color, off = _color_name(on, data, off)
        ll = data[off]
        off += 1
        if ll > MAX_LABEL:
            raise ValueError(f"label exceeds {MAX_LABEL} bytes")
        if len(data) - off < ll:
            raise ValueError("truncated label")
        label = _text(data[off : off + ll], "label")
        off += ll
        widgets.append(
            Widget(
                _KIND_NAMES[kind & 3],
                wid,
                xy >> 4,
                xy & 15,
                wh >> 4,
                wh & 15,
                label,
                role,
                shape,
                icon_only,
                icon,
                bind,
                color,
                on_color,
                trailing,
            )
        )
    if off != len(data):
        raise ValueError("trailing bytes after last widget")
    page = Page(data[1], data[2], data[3], tuple(widgets), **extra)
    _validate(page)
    return page


def page_to_dict(page: Page) -> dict[str, Any]:
    """The JSON contract shared with the TypeScript renderer (tests/vectors/pages.json)."""
    return {
        "version": page.version,
        "id": page.page_id,
        "cols": page.cols,
        "rows": page.rows,
        "gen": page.gen,
        "total": page.total,
        "kind": page.kind,
        "back": page.back,
        "seq": {"idx": page.seq_idx, "cnt": page.seq_cnt} if page.seq_cnt else None,
        "act": page.act,
        "title": page.title,
        "theme": page.theme,
        "widgets": [
            {
                "kind": w.kind,
                "id": w.id,
                "x": w.x,
                "y": w.y,
                "w": w.w,
                "h": w.h,
                "label": w.label,
                "role": w.role,
                "shape": w.shape,
                "icon_only": w.icon_only,
                "icon": w.icon,
                "bind": w.bind,
                "color": w.color,
                "on_color": w.on_color,
                **({"trailing": True} if w.trailing else {}),
            }
            for w in page.widgets
        ],
    }
