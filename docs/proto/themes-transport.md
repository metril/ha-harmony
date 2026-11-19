# Custom theme transport (`theme_push`)

A user theme (`themes:` in the config, `theme: <name>`) is 99 wire bytes (`harmony_core/themes.py`; layout in
`docs/plans/` themes plan, decoded and validated by `remote/app/theme_derive.c`). Its pages carry header theme id 7; the theme
itself travels in its own message.

## Message

`theme_push` (code **28**, transport `both`): `{remote u8, gen u8, data bytes}`.

- WS (integration -> hub): `data` is base64 of the 99 bytes; the reply is `command_result {ok}` (the hub took it) or an error
  (`no_remote`, `too_big`, `rf_error`). A hub without the message answers `unknown_type`.
- RF (hub -> remote): `remote, gen, len, data`, one object (body 102 of 254 bytes), acked at the link like `page_push`.
- `gen` is the page set's generation byte (`RemotePages.gen`): the theme belongs to the set with that gen.

## Hub

Handled like `page_push` for queueing: held while the remote is away, superseded by (type, remote), dropped when a push of a
newer gen arrives (the older set's pages, tiles and theme all go), resent after the remote is heard again when its RF retries
fail (at most `HUB_PAGE_RESENDS`, then dropped and counted), and not resent when a newer gen has been pushed since.

**Order.** For a generation the theme is delivered before its pages: a page of a v2 set is held back while a `theme_push` of that
remote is in flight on the link or held for a resend. A theme that was given up on no longer holds the pages back; they are
then held by the remote (below) until a later push brings the theme. A newer config cancels a running push in the integration,
and sets never interleave (one task per remote, the old one has finished before the new one sends).

## Remote: the slot and the completeness rule

One slot `{gen, 99 bytes}` in RAM next to the page store (`hp_store_t`, `remote/page/store.c`).

| event | effect |
|---|---|
| `theme_push`, bytes invalid (length, version, ranges) | ignored; nothing changes |
| `theme_push`, gen differs from the held set's | the held pages are dropped (as a page of a new gen does); the slot is replaced |
| `theme_push`, same gen, other bytes | slot replaced, the visible page repainted |
| `theme_push`, same gen, same bytes | no change (idempotent; still acked at the link) |
| restart | slot and store are empty |

**A page with theme id 7 counts as received only while the slot's gen equals the set's gen.** That one rule drives both
the display (`hp_store_ready`: such a page is never drawn with a stale or missing theme; the "Receiving" screen shows instead) and
`remote_info` (`hp_store_complete` / `hp_store_theme_missing`: gen and page count are reported only for a complete set, so a
set waiting for its theme reports 0 pages and the hub / integration still sees it as missing). A set whose pages use presets
only needs no theme. Pages in any order relative to the theme are accepted; whichever arrives last completes the set.

The remote announces caps **bit 3** (`THEME_CUSTOM`: user theme and free colours).

## Integration

For a remote whose caps include bit 3, on a hub that knows `theme_push`, `for_caps` yields the user theme (`theme_wire`) and
free colours; the coordinator sends `theme_push` before the pages of that generation, in the same task (so it is redone
exactly when the pages are: reconnect, remote re-attach, caps change, config change). After the push it waits up to
`THEME_ACK_WAIT` (20 s) for the `remote_info` that reports the complete set (gen and page count); without it the set (theme
and pages) is sent once more, e.g. when the hub gave up while the remote was away.

Fallbacks, all the bit-2 behaviour (user theme shows as its base preset, free colours snapped, the existing notice):

- the remote lacks bit 3 (older remote firmware);
- the hub answers `unknown_type` to `theme_push` (older hub firmware; retried after the next reconnect);
- an integration without the transport (`const.THEME_TRANSPORT` false).

The *Remote N page format* sensor and the diagnostics carry `theme: {name, id, sent, acked}`: `sent` = the hub took the push,
`acked` = the remote reported the complete set.

## Compatibility

Additive (docs/proto/compat.md). Verified for the transports: an unknown WS type is answered `unknown_type` (hub); an unknown RF
type is acked at the link and dropped by the remote (`tests/c/test_remote_rf.c`, type 29), the hub drops unknown remote types
(`tests/c/test_link_v2.c`). Pairings: old hub + new integration (base preset), old remote + new everything (base preset), new
remote + old integration (never receives a theme or id 7 pages).
