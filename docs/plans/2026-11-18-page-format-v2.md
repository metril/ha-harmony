# Page format v2

Why: UI direction B needs widget roles and icons, the active-activity highlight, tile state, paged
lists, Home/Favorites pages and a page count on the wire. The add-on editor previews pages from the
same decoded model. v1: `docs/proto/page-format.md`.

Decisions taken (user asleep 2026-11-18; recommendations adopted, revisit on request): Home and
Favorites navigate on the remote AND still forward `key_event`; icon glyphs are our own line icons
(mockup symbols + ~16 more in the same style); a page over the byte budget fails the compile with
the exact overage, never truncates.

## What in today's code blocks a purely additive change

1. Existing messages cannot gain fields: the Python ws decoder rejects unknown fields
   (`harmony_core/harmony_proto.py` `_validate`) and the RF codec rejects trailing bytes
   (`hub/rf/rf_payload.h`). So: NEW message types only. Old hub, old remote and old HA all drop
   unknown types.
2. The remote holds ONE page: every `page_push` is shown as it arrives, so a multi-page config shows
   the last page pushed. Paging/Home/Favorites need a page store on the remote.
3. `HUB_PEND_MAX` is 8: with the remote away, held pages beyond 8 push out the oldest.
4. Widget ids are global 0..127 across pages (soft key = `128+id` in a u8 `key_event`; labels use ids
   too; hub `MAX_KEYS` 128 counts page buttons).
5. Title promotion and "Power Off" styling are guessed on the remote. v2 puts both on the wire; the
   remote keeps the guesses for v1 pages only.

## 1. Wire layout (`page_push.data`, at most 251 bytes, unchanged)

Header, 11 + tlen bytes:

| off | field | notes |
|---|---|---|
| 0 | version | 2 |
| 1 | page id | < total |
| 2 | cols | |
| 3 | rows | |
| 4 | count | 0..32 |
| 5 | gen | low byte of the config hash; a new gen clears the remote's page store |
| 6 | total | pages in the set, 1..255 |
| 7 | pflags | bits 0-1 kind (0 normal, 1 home, 2 favorites); bit 2 BACK (back chevron, Exit goes Home); bits 3-5 theme id (section 7); bits 6-7 0 |
| 8 | seq | `idx<<4 \| cnt`; 0 = not paged; else 1≤idx≤cnt, cnt≥2; group = consecutive ids `id-idx+1 .. id+cnt-idx`, all < total |
| 9 | act | 0, or activity index+1: page auto-shows when that activity starts |
| 10 | tlen | 0..24 |
| 11.. | title | UTF-8; tlen 0 → "Harmony" |

Widget, 5 + optional bytes + len: `kind` · `id` · `xy` · `wh` · [style] [icon] [bind] [color] · `len` · label.

- `kind`: bits 0-1 kind (0 button, 1 tile, 2 label; 3 rejected); bit 4 HAS_STYLE; bit 5 HAS_ICON;
  bit 6 HAS_BIND; bit 7 HAS_COLOR (section 7); bits 2, 3 must be 0.
- `style`: bits 0-2 role (0 default, 1 primary, 2 secondary, 3 danger, 4 nav; 5-7 rejected);
  bits 3-4 shape (0 card, 1 row, 2 pill, 3 monogram); bit 5 icon_only (label kept for the editor,
  not drawn); bits 6-7 must be 0.
- `icon`: 0 none, 1..159 stock, 160..254 custom (reserved), 255 rejected.
- `bind`: role nav → target page id (< total; handled on the remote, no `key_event`); otherwise the
  activity index (widget is teal "active" while `state_update.activity == bind && power`).

Everything else as v1 (grid, overlap, unique id, UTF-8, exact length, `HP_E_TOOBIG`/`HP_E_INVAL`).
v2 never promotes a title label and never matches "Power Off".

One decoder for both versions; v1 decodes with defaults (gen 0, total 0 = unknown, kind normal, role
default, icon 0) and `version=1`; renderers apply the two old guesses only when `version==1`.
`page_codec.page_to_dict()` is the JSON contract the TypeScript renderer mirrors.

Handshake: new message `remote_info` (code 18, transport both):
`{remote u8, page_fmt u8 (=2), caps u8 (bit 0 tile_state, bit 1 page store/paging), icons u8 (highest stock id known), gen u8, pages u8}`
(`gen` = generation of the v2 set the remote's page store holds, `pages` = how many pages of it it holds; both 0 when the store is empty).
The remote sends it as its first uplink after each link-up. The hub caches it, broadcasts it on ws
and resends it to each client right after `hello_ack`. On `remote_attached`, HA waits up to 1.5 s
for `remote_info`: none → v1 pages; `page_fmt>=2` → v2. A later `remote_info` with a different
format triggers a re-push. After a (re)link the integration sends nothing when the reported format, `gen` and
`pages` equal the set it would send (and the hub has accepted that config); otherwise it pushes the whole set. The remote
announces again (one message) when its store becomes complete or empty and differs from what it last said, so the hub's
cache, and a client that reconnects, see the truth. HA never sends v2 without `page_fmt>=2`; a v1 remote that gets a v2 page
anyway rejects it at the version byte.

Budget: a plain widget costs 5 bytes as in v1. 32 plain widgets: 11+160 = 171 (80 left for title and
labels). 32 with style+icon: 235 (16 left → icon-only). Typical 12-button activity page: ~187.
Encoder: label budget = 251 − header − Σ(5 + optional bytes); default style/icon bytes are dropped;
still over → `CompileError("page X: N bytes over; shorten labels, use icon_only, or make it a list")`.
List pages are split by the compiler, by rows and by bytes.

## 2. Tile state

New RF message `tile_state` (code 19, rf):
`{remote, page, tile, state u8 (0 value-only, 1 off, 2 on, 3 unavailable), text str (≤31)}`.
HA sends the hub command `tile_update` with an optional `state` arg (an old hub ignores it). A new
hub sends `tile_state` if the cached `remote_info` has caps bit 0, else `tile_update`. HA computes
state: toggleable domains → on/off; `unavailable`/`unknown` → 3; else 0. Stale is remote-local (link
lost → keep last value, "stale" caption). On = warm fill #F8C040 with dark ink; unavailable = tx3
and "n/a". `tile_state` and `tile_update` share one pending key; `page_push` supersedes both. Active
activity uses the existing `state_update` plus the widget `bind` byte.

## 3. Icons

One source: `proto/icons.yaml` (`{id, name, keywords[], svg}`), SVGs in `assets/icons/*.svg`.
`tools/gen_icons.py` generates `harmony_core/icons.py`, `remote/app/icons.h` (glyph U+E000+id) and
the add-on's `icons.ts`. Ids are fixed forever, append-only; an unknown id draws no icon, the label
still draws.

The 44: tv, film, music, game, radio, app · power · chev_right, chev_left, back, up, down, home,
star, plus, minus, check · menu, guide, info, input, cc, audio, settings, list · play, pause, stop,
rewind, ffwd, skip_next, skip_prev, record, vol_up, vol_down, mute, ch · bulb, scene, thermo,
blinds, switch, fan, lock · rf, rf_off, sync, warn, moon, sun.

Glyph source: the inline `<symbol>`s in `docs/design/remote-ui-mockups.html` (24×24, 2 px stroke)
plus the rest drawn in the same style. Remote: icon font (strokes → fills, TTF, `lv_font_conv --bpp 4
--no-compress`, sizes 20/24/32), each icon an extra `lv_label` coloured by role. Flash ≈ 40 KB; measure.

## 4. Paging and Favorites

Remote: page store (≤32 pages) keyed by id; new gen clears it; receiving screen shows "Receiving
page k of total"; shown page = kind home (else id 0); a push re-renders only if that page is on
screen; an activity change in `state_update` shows that activity's `act` page, power-off shows Home;
Home/Favorites keys show the page of that kind if present and still forward `key_event`; Up/Down on
a paged page (seq cnt ≥ 2) page locally and are not forwarded, bar shows "idx/cnt"; Exit on a BACK
page goes Home. Pushed: all pages, order, kinds, groups, binds. Nothing about navigation is pushed
at runtime.

## 5. Config surface (`docs/ha-config.md` additions)

Page keys: `kind: home|favorites`, `title:` (else a full-width row-0 label becomes the header title,
the label is dropped and rows shift by 1), `activity: <id>`, `back: true`, `list: {rows: 5}`
(widgets without x/y become one full-width row each; the compiler splits into a group).
Widget keys: `icon: <name>|none`, `role: default|primary|secondary|danger`,
`shape: card|row|pill|monogram`, `icon_only: bool`, `nav: next|prev|home|<page id>`.

Guessed by the COMPILER only (explicit config always wins): action `{activity: off}` or a label like
"power off"/"all off" → role danger + icon power; action `{activity: X}` → bind X + icon from X's
name keywords; label keyword table from `icons.yaml`; tile entity domain → icon. The same compiler
output encodes to v1 (title as a label, no extras) or v2.

## 6. Compatibility, vectors, tasks

v2 pages flow only when HA, hub and remote are all new; every other combination stays v1.

Vectors: `pages.json` v2 entries with `page_to_dict` output (valid: minimal, all options, 32 widgets
at exactly 251 bytes; invalid: 252 bytes, reserved bits, kind 3, icon 255, bad seq, nav bind ≥ total,
tlen 25, id ≥ total); `rf/` `remote_info` and `tile_state`; `remote_render.json` role/icon/active,
tiles on/off/unavailable/stale, paged "2/3".

| id | task | dep | size | part |
|---|---|---|---|---|
| T1 | `icons.yaml` + SVG sources + `gen_icons.py` → py/h/ts | – | M | core |
| T2 | icon font build script + `ra_icons_20/24/32.c` | T1 | M | remote |
| T3 | `page_codec.py` v2 encode/decode/`page_to_dict` + vectors | T1 | M | core |
| T4 | `page.c`/`page.h` v2 decode, same vectors | T3 | M | remote |
| T5 | `schema.yaml`: `remote_info`(18), `tile_state`(19); regenerate | – | S | proto |
| T6 | remote sends `remote_info` on link-up | T5 | S | remote |
| T7 | hub caches/broadcasts `remote_info`, resends after `hello_ack` | T5 | S | hub |
| T8 | HA tracks remote format, waits 1.5 s, chooses v1/v2, re-pushes | T7 | M | HA |
| T9 | compiler: config keys + guesses + title into header + byte-budget errors | T3 | M | core |
| T10 | `ui.c`: role/shape/icon drawing; old guesses only for v1 (SHIP 1) | T2,T4,T6,T8,T9 | M | remote |
| T11 | active highlight from bind + `state_update` | T10 | S | remote/core |
| T12 | `tile_state`: hub arg + pending keys; HA state; remote drawing + stale | T5,T10 | M | all |
| T13 | `HUB_PEND_MAX` → 40, drop held pages of an older gen | – | S | hub |
| T14 | page store, gen/total, "page k of N", home selection, `act` auto-show | T10,T13 | L | remote |
| T15 | list pages: compiler split + seq; local Up/Down; "idx/cnt" | T14 | M | core/remote |
| T16 | home/favorites kinds + Home/Favorites/Exit keys | T14 | S | remote/core |
| T17 | labels and nav widgets stop using global soft-key ids | T9 | S | core |
| T18 | docs: `page-format.md` v2, `ha-config.md`, handshake | each | S | docs |
| T19 | add-on TypeScript renderer from the `page_to_dict` vectors | T3 | M | addon |

## 7. Colours and theme id (additive, still version 2)

Both additions use bits the first v2 decoders rejected, so the capability gate (another package)
must keep colour pages away from old remotes: an old remote returns `HP_E_INVAL` for a page with
kind bit 7 or pflags bits 3-5 and drops it. Pages with no colour and theme 0 encode to exactly the
same bytes as before.

- `pflags` bits 3-5: theme preset id 0..7 (0 default, 7 custom theme delivered separately).
  Bits 6-7 stay reserved and rejected.
- `kind` bit 7 HAS_COLOR: one byte after the optional `bind` byte and before `len`:
  low nibble = fill, high nibble = tile "on" fill. Nibble 0 = role default, 1..12 = palette index
  (`PALETTE_NAMES`: red, orange, amber, yellow, lime, green, teal, cyan, blue, indigo, purple, pink),
  15 = RGB565 follows, 13 and 14 rejected. A non-zero "on" nibble is only valid on tiles. A colour
  byte of 0x00 is rejected (the encoder omits the byte instead), so encodings are canonical.
- After the colour byte come 2 bytes big-endian RGB565 for the fill if its nibble is 15, then 2 for
  the on colour if its nibble is 15. Python takes `"#RRGGBB"` and snaps it (`theme_math.snap565`);
  decoding gives the snapped value back, so decode then encode is the identity.
- Cost: +0 none, +1 palette only, +3 one rgb, +5 two rgb. `page_codec.widget_cost(w)` returns
  `{base, label, style, icon, bind, color}` (base = 5: kind, id, xy, wh, len) and sums to `widget_size`.
- Model: Python `Widget.color` / `Widget.on_color` (palette name, `"#RRGGBB"` or None) and
  `Page.theme`; the same keys (`color`, `on_color`, `theme`) in `page_to_dict`. C: `hp_widget_t.color_fill`
  / `color_on` as `hp_color_t {kind: HP_COLOR_NONE|PALETTE|RGB, value: index or RGB565}`, `hp_page_t.theme`.
- v1 pages cannot carry either; the v1 view drops them.

Worked example, theme 3, 4x6 page, one tile `Hi` with fill red and on colour `#FF8040` (snapped F88040):

    02 00 04 06 01 00 01 18 00 00 00      header: v2, pflags 0x18 = theme 3<<3, total 1, tlen 0
    81 01 00 22                           kind tile|HAS_COLOR, id 1, xy 0,0, wh 2x2
    f1 fc 08                              colour byte (on nibble f, fill nibble 1 red), on RGB565 0xFC08
    02 48 69                              len 2, "Hi"

Vectors: `valid_v2c` (colour/theme pages, kept apart from `valid_v2` so the render goldens do not
need them yet) and the `v2c_` entries of `invalid_v2`.

## Implementation notes (device side)

Where the remote and hub deviate from, or settle, the text above.

- **Store**: `hp_store_t` is indexed by page id (< 32), raw wire bytes, tile overlay of 32 entries. A page with a changed
  gen **or a changed total** clears the store (gen is one byte, so a total change with the same gen is a new set). `total`
  > 32 is refused (`HP_E_TOOBIG`); the compiler caps a config at 32 pages ("at most 32 pages"). `find_kind`, `find_act`
  and `start` only look at ids < total; nav / seq targets >= total are ignored. Start page: home, else page 0, else the
  lowest stored id.
- **Completeness**: the set is complete when `count >= min(total, 32)` distinct pages of the current gen/total are held.
  Until then the screen is "Receiving page" with "k of N" below and a progress bar. **10 s after the last new page**
  (`APP_PARTIAL_MS`) the start page is shown anyway (set "partial") and filling continues in the background; completion
  does not move the user. Home / Exit / the bar's left corner (back chevron) go to the start page from every state, the
  receiving and error screens included (on the error screen any tap does, once the touch is over); on a partial set they
  also switch to partial mode. Exit on a non-BACK stored page still goes to the hub. A nav target that is not stored
  shows the toast "Page not received yet" and stays; the receiving screen is never entered by navigation.
- **Look**: role primary = teal ink on the secondary surface (not a teal fill; the teal fill is the active highlight);
  monogram shape = round badge with the icon, else the label's first letter upper-cased; `icon_only` with an icon id the
  font does not have falls back to drawing the label; a held Up / Down on a paged page is consumed at press, so hold and
  release are not forwarded and it does not repeat.
- **remote_info**: sent as the first uplink after a link-up and repeated on the 5-minute battery heartbeat. The hub
  caches it per remote and clears the cache on link loss. It broadcasts to ws clients when it changed **and for the first
  one after each observed link-up even if identical** (so Home Assistant re-pushes after a reattach); heartbeat repeats
  are not broadcast, so there is one broadcast per link-up.
- **Held queue** (`HUB_PEND_MAX` 40): when full, tile messages are evicted first (oldest first); a page is never evicted
  while a tile message is held. With only pages held a new tile is dropped, and a new page drops the oldest page (pages of
  an older gen and of the same id are already gone). Losses are counted (`pend_dropped`), the first is logged.
- **page_push retries**: a page_push that exhausts its RF retries while the remote is still attached is held again and
  resent after the next frame heard from the remote, at most 3 times (`HUB_PAGE_RESENDS`), then dropped (counted, logged).
  It is not resent when a newer push of that page is held or in flight, or a newer gen has been pushed since.
- **v1 vs v2 on one remote**: a v2 page always switches the remote to v2 mode (the identical-page shortcut applies only
  in v2 mode). A v1 page replaces the display, leaves v2 mode and drops the v2 store, which is what a v1-only Home
  Assistant needs and cannot strand the user (every pushed page of either format is shown); v1 pages are only sent by a
  Home Assistant that does not see `page_fmt` 2, so a v2 session does not meet them. The next v2 page starts a fresh set.
- **Memory**: the LVGL pool is 80 KiB. The worst legal v2 page (32 buttons with an icon and a label, plus a toast)
  peaks at 39 KB (47 %); all-tile pages peak at 38 KB. The build precheck counts only objects a page really draws.
