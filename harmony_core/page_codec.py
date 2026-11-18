"""Remote page encoder/decoder (format: docs/proto/page-format.md). No Home Assistant imports."""

from __future__ import annotations

from dataclasses import dataclass

VERSION = 1
MAX_WIDGETS = 32
MAX_LABEL = 24
# Must match HP_PAGE_MAX_BYTES in remote/page/page.h: one RF object carries at most 254 body
# bytes and page_push uses 3 for its header, so a 252..255-byte page compiles but cannot be sent.
MAX_BYTES = 251
MAX_GRID = 16
KINDS = {"button": 0, "tile": 1, "label": 2}
_KIND_NAMES = {v: k for k, v in KINDS.items()}


@dataclass(frozen=True)
class Widget:
    kind: str  # "button" | "tile" | "label"
    id: int  # 0..127
    x: int
    y: int
    w: int
    h: int
    label: str = ""


@dataclass(frozen=True)
class Page:
    page_id: int
    cols: int
    rows: int
    widgets: tuple[Widget, ...]


def _validate(page: Page) -> None:
    if not 0 <= page.page_id <= 255:
        raise ValueError(f"page id {page.page_id} out of range 0..255")
    if not (1 <= page.cols <= MAX_GRID and 1 <= page.rows <= MAX_GRID):
        raise ValueError(f"grid {page.cols}x{page.rows} out of range 1..{MAX_GRID}")
    if len(page.widgets) > MAX_WIDGETS:
        raise ValueError(f"too many widgets: {len(page.widgets)} (max {MAX_WIDGETS})")
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
        if len(w.label.encode()) > MAX_LABEL:
            raise ValueError(f"widget {w.id} label exceeds {MAX_LABEL} bytes")
        if "\x00" in w.label:
            raise ValueError(f"widget {w.id} label contains NUL")
        for o in seen:
            if o.id == w.id:
                raise ValueError(f"duplicate widget id {w.id}")
            if w.x < o.x + o.w and o.x < w.x + w.w and w.y < o.y + o.h and o.y < w.y + w.h:
                raise ValueError(f"widgets {o.id} and {w.id} overlap")
        seen.append(w)


def encoded_size(widgets: list[Widget]) -> int:
    """Bytes the page would occupy (header + widgets), whether or not it fits."""
    return 5 + sum(5 + len(w.label.encode()) for w in widgets)


def encode_page(page_id: int, cols: int, rows: int, widgets: list[Widget]) -> bytes:
    """Encode a page; raises ValueError for anything the remote would reject."""
    _validate(Page(page_id, cols, rows, tuple(widgets)))
    out = bytearray([VERSION, page_id, cols, rows, len(widgets)])
    for w in widgets:
        label = w.label.encode()
        out += bytes([KINDS[w.kind], w.id, (w.x << 4) | w.y, (w.w << 4) | w.h, len(label)]) + label
    if len(out) > MAX_BYTES:
        raise ValueError(f"encoded page is {len(out)} bytes (max {MAX_BYTES})")
    return bytes(out)


def decode_page(data: bytes) -> Page:
    """Decode and validate; raises ValueError on malformed input."""
    if len(data) > MAX_BYTES:
        raise ValueError(f"page data is {len(data)} bytes (max {MAX_BYTES})")
    if len(data) < 5:
        raise ValueError("truncated header")
    if data[0] != VERSION:
        raise ValueError(f"unsupported page format version {data[0]}")
    count = data[4]
    if count > MAX_WIDGETS:
        raise ValueError(f"too many widgets: {count} (max {MAX_WIDGETS})")
    off, widgets = 5, []
    for _ in range(count):
        if len(data) - off < 5:
            raise ValueError("truncated widget")
        kind, wid, xy, wh, ll = data[off : off + 5]
        off += 5
        if kind not in _KIND_NAMES:
            raise ValueError(f"unknown widget kind {kind}")
        if ll > MAX_LABEL:
            raise ValueError(f"label exceeds {MAX_LABEL} bytes")
        if len(data) - off < ll:
            raise ValueError("truncated label")
        try:
            label = data[off : off + ll].decode()
        except UnicodeDecodeError as e:
            raise ValueError("label is not valid UTF-8") from e
        off += ll
        widgets.append(Widget(_KIND_NAMES[kind], wid, xy >> 4, xy & 15, wh >> 4, wh & 15, label))
    if off != len(data):
        raise ValueError("trailing bytes after last widget")
    page = Page(data[1], data[2], data[3], tuple(widgets))
    _validate(page)
    return page
