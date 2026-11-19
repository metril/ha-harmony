# Bluetooth HID messages

The hub can act as a Bluetooth HID keyboard/consumer-control device towards phones and TVs ("hosts") and, over LE,
take input from bonded HID remotes. These are protocol messages only; the hub reaches the Bluetooth helper through
the narrow `hub/bt_link.h` interface (non-blocking by contract). All are ws, authenticated; an unauthenticated socket
gets the normal `unauthorized` `error`. Compatibility rules: [compat.md](compat.md) (additive, unknown fields ignored).

| Code | Type | Direction | Fields |
|---|---|---|---|
| 35 | `bt_pair` | client -> hub | `on: bool`, `secs: u16?` (default 120, 1..300), `transport: "any"\|"classic"\|"le"?` (default any), `mode: "host"\|"remote"?` (default host), `addr: str?` (`AA:BB:CC:DD:EE:FF`, either case; see "Picking a remote") |
| 36 | `bt_state` | hub -> client | see below; also the reply to `bt_pair` / `bt_forget` (same id), and pushed (id 0) on any change |
| 37 | `bt_forget` | client -> hub | `slot: u8`, `kind: "host"\|"remote"?` (default host) |
| 38 | `bt_key` | client -> hub | `slot: u8`, `page: 7\|12`, `usage: u16`, `action: "down"\|"up"\|"press"` (test-fire at a host) |
| 39 | `bt_remote_key` | hub -> client | `slot: u8`, `page: u8`, `usage: u16`, `down: bool` (key from a bonded remote) |
| 40 | `bt_listen` | client -> hub | `on: bool`: opt this socket in/out of `bt_remote_key`; reset by every `hello` |

Codes 30..34 stay reserved for the remote's RF OTA.

## hello_ack.bt

Present only when the hub build has Bluetooth: `{hosts_max, conn_max, remotes_max, le_bonds_max, pages: [7, 12], le: bool}`
(paired-host limit (up to 255), simultaneous connections, bonded-remote limit (up to 32; 0 = no remote input), LE bond limit, HID usage pages the hub can
send, LE supported). Values come from the link, never constants. Absent: no `bt_*` messages are served
(requests answer `error` `unsupported`).

## bt_state

`{state, discoverable, discoverable_secs?, le?, gen?, pairing?, hosts, remotes?, candidates?}`

- `state`: `off` | `starting` | `ready` | `no_adapter` | `disabled`.
- `discoverable_secs`: seconds left while discoverable.
- `pairing`: the last pairing outcome `{result: "ok"|"failed"|"refused", addr, name?, transport?, kind?: "host"|"remote"}`.
- `hosts`: `[{slot, addr, name?, transport: "classic"|"le", connected}]`.
- `remotes`: `[{slot, addr, name?, connected, battery?}]` (`battery` percent, omitted when unknown).

- `candidates`: `[{addr, name?, rssi}]`, at most 16, `rssi` dBm (int8). Present (non-empty) only while a remote-mode
  window is open and LE remotes have been seen; absent otherwise. Same `addr` / `name` validation as hosts. Membership
  changes (a device appears or drops out) push a `bt_state`; RSSI-only changes do not, so `rssi` can be stale.

Valid slots are `0 .. hosts_max-1` (hosts) and `0 .. remotes_max-1` (remotes).

## Picking a remote

The hub never bonds the first advertiser. `bt_pair {on: true, mode: "remote"}` without `addr` opens the window and
scans: it only fills `candidates`. The client shows them and sends `bt_pair {on: true, mode: "remote", addr}` for the
chosen one; the reply is the usual snapshot and the outcome arrives as a `bt_state` push with `pairing`
(`kind: "remote"`). `addr` is matched case-insensitively against the current candidates. Errors: `no_candidate` (not
in the current list, e.g. it went away), `bad_addr` (malformed, or `addr` with `mode: host` or `on: false`).
Old clients that send no `addr` and ignore `candidates` keep working but get no bond.

## Errors

`bad_addr`, `no_candidate`, `unsupported` (no Bluetooth, or `mode: remote` without remote support), `bad_secs`, `bad_transport`, `bad_mode`,
`bad_kind`, `bad_page`, `bad_action`, `no_le`, `no_slot` (out of range or nothing paired there), `not_connected`,
`bt_not_ready`, `bt_busy`. Wrong types, missing fields and out-of-range numbers (for example `slot` > 255) are the
codec's ordinary `error`.

## Remote key learning

`bt_remote_key` is only for the editor's "press a key on the remote to learn it": a client sends `bt_listen {on: true}`
and receives a push per key until it opts out or says `hello` again. Normal key handling happens inside the hub and does
not depend on it.

## Frame size

Slots are one byte, so `bt_state` can list up to 255 hosts and 32 remotes. The hub keeps it inside one 64 KiB frame:
every entry's fixed part is always sent, but the optional `name` is dropped from the entries that would push the message
past a 40,000-byte budget (worst-case escaped names included). Clients must treat `name` as optional.

## Semantics

- The hub sends `bt_state` (id 0) right after `hello_ack` on builds with Bluetooth; that is the read-only way to get it.
- The reply to `bt_pair` / `bt_forget` is a snapshot taken when the request was accepted, not its outcome. The outcome
  arrives as a later `bt_state` push. `bt_key` is best effort: an asynchronous failure is not reported.
- `gen` is bumped by the hub's Bluetooth link on every change. The `discoverable_secs` countdown does not push anything:
  count down locally from the last value.
- Bursts of changes are coalesced into one push per event-loop pass.
- If the helper dies, `state` leaves `ready`, `hosts` / `remotes` / `pairing` are cleared and a push follows.
- Values from nearby devices are untrusted: an entry whose `addr` is not `AA:BB:CC:DD:EE:FF` is omitted, and a `name`
  that is not valid UTF-8 is omitted.
