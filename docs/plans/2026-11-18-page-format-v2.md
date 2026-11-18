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
| 7 | pflags | bits 0-1 kind (0 normal, 1 home, 2 favorites); bit 2 BACK (back chevron, Exit goes Home); other bits 0 |
| 8 | seq | `idx<<4 \| cnt`; 0 = not paged; else 1≤idx≤cnt, cnt≥2; group = consecutive ids `id-idx+1 .. id+cnt-idx`, all < total |
| 9 | act | 0, or activity index+1: page auto-shows when that activity starts |
| 10 | tlen | 0..24 |
| 11.. | title | UTF-8; tlen 0 → "Harmony" |

Widget, 5 + optional bytes + len: `kind` · `id` · `xy` · `wh` · [style] [icon] [bind] · `len` · label.

- `kind`: bits 0-1 kind (0 button, 1 tile, 2 label; 3 rejected); bit 4 HAS_STYLE; bit 5 HAS_ICON;
  bit 6 HAS_BIND; bits 2, 3, 7 must be 0.
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
`{remote u8, page_fmt u8 (=2), caps u8 (bit 0 tile_state, bit 1 page store/paging), icons u8 (highest stock id known)}`.
The remote sends it as its first uplink after each link-up. The hub caches it, broadcasts it on ws
and resends it to each client right after `hello_ack`. On `remote_attached`, HA waits up to 1.5 s
for `remote_info`: none → v1 pages; `page_fmt>=2` → v2. A later `remote_info` with a different
format triggers a re-push. HA never sends v2 without `page_fmt>=2`; a v1 remote that gets a v2 page
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
