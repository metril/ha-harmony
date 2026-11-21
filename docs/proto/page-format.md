# Remote page format (version 1)

Carried as the `data` bytes of `page_push` (<= 251 bytes; one page per push,
page id also in the RF message). Screen 240x320 px; all integers are bytes.
Decoders: `remote/page/page.c`, `custom_components/ha_harmony/page_codec.py`;
golden vectors: `tests/vectors/pages.json`.

## Header (5 bytes)
| off | field   | rule                                  |
|-----|---------|---------------------------------------|
| 0   | version | must be 1                             |
| 1   | page id | 0..255                                |
| 2   | cols    | 1..16                                 |
| 3   | rows    | 1..16                                 |
| 4   | count   | widget count, 0..32                   |

## Widget (5 + label_len bytes), repeated `count` times
| off | field | rule                                                      |
|-----|-------|-----------------------------------------------------------|
| 0   | kind  | 0 button, 1 tile, 2 label                                 |
| 1   | id    | 0..127, unique within the page                            |
| 2   | x,y   | high nibble x, low nibble y (grid cells, 0-based)         |
| 3   | w,h   | high nibble w, low nibble h (cells, each 1..15, 4-bit)    |
| 4   | len   | label byte length, 0..24                                  |
| 5.. | label | UTF-8, no NUL                                             |

## Rules
- Widgets must lie inside the grid (x+w <= cols, y+h <= rows) and must not
  overlap each other (all kinds, labels included).
- Total length must equal header + widgets exactly. Trailing bytes, truncation,
  bad version/kind/UTF-8, size outside 1..15 (zero or >15), out-of-grid, overlap, duplicate id:
  rejected (`HP_E_INVAL`). More than 32 widgets, a label over 24 bytes, or
  data over 251 bytes: `HP_E_TOOBIG` (one RF object body is 254 bytes and
  `page_push` spends 3 on its header). The Python encoder raises `ValueError`
  for the same cases. Nothing is ever silently truncated.
- Pixels: the top 32 px of the 240x320 screen are the remote's status bar and
  are never part of a page. The grid fills the 240x288 content area below it:
  a cell spans (240/cols) x (288/rows) px; pixel (px,py) with py >= 32 is in
  cell (px*cols/240, (py-32)*rows/288), integer division. Hit-testing is by
  cell (the whole cell, including the margin drawn around the widget); the
  status bar is not hit-testable. Wire format unchanged.
- Presentation (remote-side only, nothing on the wire): a label widget at
  x=0, y=0 spanning all `cols` with h=1 is shown as the status-bar title
  instead of in the grid, and the remaining rows are laid out over the whole
  240x288 area as if the page had rows-1 rows (widget row y draws and hits as
  row y-1; hit: (py-32)*(rows-1)/288 + 1). Without such a label, or with
  rows=1, the plain rule above applies and the page is titled "Harmony". A
  button labelled exactly "Power Off" (case-insensitive) is drawn in the
  danger colour. Widgets are drawn inset 8 px from the screen edge and 8 px apart.
  Labels that do not fit end in "..." instead of wrapping.
- Tiles show live text set by `tile_update` (remote, page, tile id, text);
  text starts empty and is truncated to 31 bytes at a UTF-8 boundary.
- Tapping a button or tile emits `key_event` with code `128 + widget id`
  (press, then release): the reserved soft-key range 128..255.

---

# Version 2

Full design and rationale: [docs/plans/2026-11-18-page-format-v2.md](../plans/2026-11-18-page-format-v2.md).
v1 above stays valid and unchanged; one decoder (`harmony_core/page_codec.py`, `decode_page`) reads both.
Golden vectors: `tests/vectors/pages.json` keys `valid` / `invalid` (v1), `valid_v2` / `valid_v2c` (colours, theme) / `invalid_v2`
(each valid entry carries its `page_to_dict` JSON, the contract the add-on renderer mirrors).

## Header (11 + tlen bytes)
`version`=2 · `page id` (< total) · `cols` · `rows` · `count` 0..32 · `gen` (low byte of a CRC over the hub
config and every page layout; a new gen clears the remote's page store) · `total` 1..255 · `pflags`
(bits 0-1 kind: 0 normal, 1 home, 2 favorites, 3 rejected; bit 2 BACK = "Exit pops locally" (clear: Exit goes to the hub; the header chevron ignores it); bits 3-5 theme preset id 0..7, 7 = custom; bit 6 FIXED; bit 7 must be 0) · `seq`
(`idx<<4|cnt`; 0 = not paged, else 1 <= idx <= cnt, cnt >= 2; the group is the consecutive ids
`id-idx+1 .. id+cnt-idx`, all < total) · `act` (0, or activity index + 1) · `tlen` 0..24 · `title`
(UTF-8, no NUL; empty = "Harmony").

## Widget: `kind id xy wh [style] [icon] [bind] [color] len label`
`kind` bits 0-1: 0 button, 1 tile, 2 label (3 rejected); bit 4 HAS_STYLE, 5 HAS_ICON, 6 HAS_BIND, 7 HAS_COLOR;
bits 2-3 ALIGN (0 centre, the default; 1 left; 2 right; 3 rejected: the icon + label group's alignment, no extra
byte; it applies to row/pill and wide, short buttons and to big cards, in the space left of a trailing chevron, with a 12 px inset
for left/right; dense and icon-only widgets are always centred; any role). `color` (after `bind`): low nibble fill, high nibble tile on-fill; 0 default,
1..12 palette, 15 = RGB565 big-endian follows (fill, then on), 13/14 and a 0x00 byte rejected, on only
on tiles; see docs/plans/2026-11-18-page-format-v2.md section 7. `style`: bits 0-2 role (0 default, 1 primary, 2 secondary, 3 danger, 4 nav;
5-7 rejected), bits 3-4 shape (0 card, 1 row, 2 pill, 3 monogram), bit 5 icon_only, bit 6 TRAILING (draw a chevron at the
right end of the widget; only valid with role nav; the compiler sets it on `nav: next|<page id>`, which also default to shape row
and no leading icon; `nav: prev|home` keep their leading chev_left/home icon), bit 7 COMPACT (FIXED pages only; see below).
`icon`: 0 none, 1..159 stock (`proto/icons.yaml`, glyph U+E000+id), 160..254 reserved custom, 255
rejected. `bind`: role nav -> target page id (< total, a nav widget must have one); otherwise the
activity index highlighted while `state_update.activity == bind` and power is on. Encoders drop
default style/icon bytes; decoders accept them. Everything else (grid, overlap, unique ids, exact
length, 251 bytes) as v1. v2 never promotes a title label and never matches "Power Off".

Errors: more than 251 bytes, a title or label over 24 bytes, more than 32 widgets: too big
(`HP_E_TOOBIG`); every other violation: invalid (`HP_E_INVAL`).

## FIXED layout (pflags bit 6)
Widgets keep a class size whatever the page holds; nothing stretches. Requires `cols` = 12, every widget `h` = 1,
span `w` in 12/6/4/3 with `x` a multiple of `w`; anything else is `HP_E_INVAL`. COMPACT (style bit 7) is valid only
on a FIXED page and only on a button with shape card and span 6; otherwise `HP_E_INVAL` (so is bit 7 on a non-FIXED page).

Width = (224 - (n-1)*8) / n with n = 12/w (integer division): span 12 = 224, 6 = 108, 4 = 69, 3 = 50 px.
x1 = 8 + (x/w)*(width+8); x2 = x1 + width - 1.

Height by class: label or shape row = 44; kind tile = 64; shape pill = 52; shape card or monogram by span: 12 = 80,
6 = 80 (COMPACT 56), 4 or 3 = 52. A grid row is as tall as its tallest widget; a grid row with no widget takes no
space. Rows stack from content y 8 (screen y = 32 + 8 + offset), with a 2 px gap between two consecutive non-empty grid rows
that hold only row-class widgets (labels, shape row) and 8 px otherwise. The stack (heights + gaps) may not exceed 276
px (288 - 8 top - 4 bottom): `HP_E_INVAL`. y2 = y1 + height - 1. The drawn rectangle IS the box (no inset); hit-testing
uses the box, the gaps are dead.

One function computes it: `hp_page_box` (remote/page/page.c; `ra_ui_widget_box` calls it), `page_box`
(harmony_core/page_codec.py) and `widgetBox` (addon/frontend/src/lib/page-renderer.ts). For a non-FIXED page it is
the grid cell (ceil edges, v1 title row shift). Vectors: `valid_fixed` (each with `boxes`, one `[x1,y1,x2,y2]` per widget)
and `invalid_fixed`.

Compiler: config page `layout: fixed` (cols 12), widget `compact: true`. A FIXED page is split by grid rows
when the stacked height, the 251 byte budget, 32 widgets or 16 grid rows run out; the pieces reuse the list `seq` groups
(header "1/2") and renumber their rows from 0. BACK is set unless the page sets `back: false` or its activity (or the
global key map) maps the Exit key.

## Compiler contract
`compile_config` keeps one list of v2 page models (`Compiled.models`) and derives both encodings:
`Compiled.pages_v2` (v2) and `Compiled.pages` (v1: the title is a row-0 label again, widget extras and
`nav` buttons are dropped, list groups become plain pages). Ids: buttons and tiles take global soft-key
ids; labels and nav widgets take page-local ids, so v1 bytes differ from pre-v2 output only in ids when
a label precedes a button.

## New messages
`remote_info` (18, both): `{remote, page_fmt, caps, icons, gen, pages}` (`gen`/`pages`: the v2 set held in the page store, 0/0 when none); `tile_state` (19, rf): `{remote, page, tile,
state, text}` with state 0 value-only, 1 off, 2 on, 3 unavailable. Code that predates them drops both.
