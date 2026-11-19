# Bluetooth HID messages

The hub can act as a Bluetooth HID keyboard/consumer-control device towards phones and TVs ("hosts") and, over LE,
take input from bonded HID remotes. These are protocol messages only; the hub reaches the Bluetooth helper through
the narrow `hub/bt_link.h` interface (non-blocking by contract). All are ws, authenticated; an unauthenticated socket
gets the normal `unauthorized` `error`. Compatibility rules: [compat.md](compat.md) (additive, unknown fields ignored).

| Code | Type | Direction | Fields |
|---|---|---|---|
| 35 | `bt_pair` | client -> hub | `on: bool`, `secs: u16?` (default 120, 1..300), `transport: "any"\|"classic"\|"le"?` (default any), `mode: "host"\|"remote"?` (default host) |
| 36 | `bt_state` | hub -> client | see below; also the reply to `bt_pair` / `bt_forget` (same id), and pushed (id 0) on any change |
| 37 | `bt_forget` | client -> hub | `slot: u8`, `kind: "host"\|"remote"?` (default host) |
| 38 | `bt_key` | client -> hub | `slot: u8`, `page: 7\|12`, `usage: u16`, `action: "down"\|"up"\|"press"` (test-fire at a host) |
| 39 | `bt_remote_key` | hub -> client | `slot: u8`, `page: u8`, `usage: u16`, `down: bool` (key from a bonded remote) |
| 40 | `bt_listen` | client -> hub | `on: bool`: opt this socket in/out of `bt_remote_key`; reset by every `hello` |

Codes 30..34 stay reserved for the remote's RF OTA.

## hello_ack.bt

Present only when the hub build has Bluetooth: `{hosts_max, conn_max, remotes_max, pages: [7, 12], le: bool}`
(paired-host limit, simultaneous connections, bonded-remote limit (0 = no remote input), HID usage pages the hub can
send, LE supported). Values come from the link, never constants. Absent: no `bt_*` messages are served
(requests answer `error` `unsupported`).

## bt_state

`{state, discoverable, discoverable_secs?, le?, pairing?, hosts, remotes?}`

- `state`: `off` | `starting` | `ready` | `no_adapter` | `disabled`.
- `discoverable_secs`: seconds left while discoverable.
- `pairing`: the last pairing outcome `{result: "ok"|"failed"|"refused", addr, name?, transport?, kind?: "host"|"remote"}`.
- `hosts`: `[{slot, addr, name?, transport: "classic"|"le", connected}]`.
- `remotes`: `[{slot, addr, name?, connected, battery?}]` (`battery` percent, omitted when unknown).

Valid slots are `0 .. hosts_max-1` (hosts) and `0 .. remotes_max-1` (remotes).

## Errors

`unsupported` (no Bluetooth, or `mode: remote` without remote support), `bad_secs`, `bad_transport`, `bad_mode`,
`bad_kind`, `bad_page`, `bad_action`, `no_le`, `no_slot` (out of range or nothing paired there), `not_connected`,
`bt_not_ready`, `bt_busy`. Wrong types, missing fields and out-of-range numbers (for example `slot` > 255) are the
codec's ordinary `error`.

## Remote key learning

`bt_remote_key` is only for the editor's "press a key on the remote to learn it": a client sends `bt_listen {on: true}`
and receives a push per key until it opts out or says `hello` again. Normal key handling happens inside the hub and does
not depend on it.
