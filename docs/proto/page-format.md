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
- Pixels: a cell spans (240/cols) x (320/rows) px; pixel (px,py) is in cell
  (px*cols/240, py*rows/320), integer division. Hit-testing is by cell.
- Tiles show live text set by `tile_update` (remote, page, tile id, text);
  text starts empty and is truncated to 31 bytes at a UTF-8 boundary.
- Tapping a button or tile emits `key_event` with code `128 + widget id`
  (press, then release): the reserved soft-key range 128..255.
