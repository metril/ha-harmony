# Bluetooth HID messages

The hub can act as a Bluetooth HID keyboard/consumer-control device towards phones and TVs ("hosts") and, over LE,
take input from bonded HID remotes. These are protocol messages only; the hub reaches the Bluetooth helper through
the narrow `hub/bt_link.h` interface (non-blocking by contract). All are ws, authenticated; an unauthenticated socket
gets the normal `unauthorized` `error`. Compatibility rules: [compat.md](compat.md) (additive, unknown fields ignored).

| Code | Type | Direction | Fields |
|---|---|---|---|
| 35 | `bt_pair` | client -> hub | `on: bool`, `secs: u16?` (default 120, 1..300), `transport: "any"\|"classic"\|"le"?` (default any; which radio takes a new host bond, see "Window transport"), `mode: "host"\|"remote"?` (default host), `addr: str?` (`AA:BB:CC:DD:EE:FF`, either case; see "Picking a remote") |
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

`{state, discoverable, discoverable_secs?, le?, gen?, pairing?, hosts, remotes?, candidates?, window?}`

- `state`: `off` | `starting` | `ready` | `no_adapter` | `disabled`.
- `discoverable_secs`: seconds left while discoverable.
- `pairing`: the last pairing outcome `{result: "ok"|"failed"|"refused", addr, name?, transport?, kind?: "host"|"remote"}`. Opening a pairing window (`bt_pair` on) clears it, bumps `gen` and pushes, so a result seen afterwards belongs to the new window. A client should still compare it with the snapshot it took and with `gen` (older firmware kept the last result after the window closed).
- `hosts`: `[{slot, addr, name?, transport: "classic"|"le", connected, configured?}]`.
- `remotes`: `[{slot, addr, name?, connected, battery?, configured?}]` (`battery` percent, omitted when unknown).
- `configured` (optional, additive): `true` when a `bt_hosts` / `bt_remotes` entry of the loaded config carries this address,
  and `slot` is then that entry's config slot; `false` for a peer that is bonded in the hub but not in the config (an editor
  can offer "add to config"). `slot` of an unconfigured entry is a free number that no config entry uses (so `bt_key` /
  `bt_forget` by that slot still work), and may change when the config or the bonds change. A client that does not know
  `configured` can ignore it.
- `window` (optional): `"host"` or `"remote"` while a pairing window is open (or requested while the helper is still
  starting); absent = no window. This tells "window open, nothing seen yet" (`window: "remote"`, no `candidates`) from "no
  window".

- `candidates`: `[{addr, name?, rssi}]`, at most 16, `rssi` dBm (int8). Present (non-empty) only while a remote-mode
  window is open and LE remotes have been seen; absent otherwise. Same `addr` / `name` validation as hosts. Membership
  changes (a device appears or drops out) push a `bt_state`; RSSI-only changes do not, so `rssi` can be stale.

Valid slots are `0 .. hosts_max-1` (hosts) and `0 .. remotes_max-1` (remotes). A host that the config lists by address but
that is not bonded has no entry in `hosts`; `bt_key` / `bt_forget` for its slot answer `no_slot`.

## Supervision (hub/bt_link.c)

The hub runs the Bluetooth helper as a child process (`hub bt-helper`, `hub/bt/bt_ipc.h`) only while it is needed: the loaded
config has `bt_hosts` or `bt_remotes`, or a pairing window is open (or closed less than 60 s ago, so a fresh bond stays
visible). Before the first start of a boot the stock `bluetoothd` is stopped if it is running (nothing is changed on disk, so
the stock firmware gets it back at the next boot). The hub pings every 5 s and kills a helper that has been silent for 15 s,
or that sends a malformed frame; it restarts it after 5, 30, then 120 s, at most 5 times a boot, after which `state` is
`disabled` until the hub restarts. A helper death is not a boot failure: `state` leaves `ready`, `hosts` / `remotes` /
`candidates` / `pairing` / `window` are cleared, `gen` changes, a key held down for a host is released and every connected
remote is reported lost to the key mapping; IR and RF are unaffected. Config bt slots are bound to the helper's bonds by address.

## Window transport

`bt_pair {transport}` decides which radio takes a new host bond while the window is open (`mode: host`; a remote window is
always LE):

| transport | Classic inquiry / page scan | LE advertising |
|---|---|---|
| `any` | discoverable | on, general discoverable |
| `classic` | discoverable | off (a bonded LE host asked to connect may still get a short reconnect advertisement) |
| `le` | not discoverable; page scan off unless a Classic bond exists | on, general discoverable |

In an `le` window Classic cannot bond: the PIN / SSP requests are refused and an unknown Classic host's HID channel is
dropped. Page scan stays on only when at least one Classic host is bonded, so that host can still reconnect (an unbonded
host cannot find or pair with the hub). In a `classic` window an LE central's pairing request is declined. When the window
closes the usual state returns (connectable, not discoverable, no pairing advertising). Without LE support an `le` window
falls back to `any` in the helper (the hub already answers `no_le`).

The helper logs one line (default level, no key material) for the start of each pairing (`pairing start: transport le|classic,
peer ...`), each refusal (`pairing refused: ...`) and each failure (`pairing failed: transport ..., peer ..., SMP reason 0x..` /
`HCI status 0x..` / `auth status 0x..`). Routine HID back-pressure lines ("refused by the stack ... waiting to send") are debug
level: set `HH_BT_DEBUG` in the helper's environment to see them.

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
