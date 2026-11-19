# Bluetooth in Home Assistant

A hub build with Bluetooth can type to a phone or TV as a keyboard and remote ("hosts"), and take input from a bonded
Bluetooth LE remote. Configuring what the keys do is in [ha-config.md](ha-config.md) ("Bluetooth devices and remotes");
the wire protocol is in [proto/bluetooth.md](proto/bluetooth.md). This page is what the Home Assistant integration adds.

Everything here exists only when the hub reports Bluetooth. A hub without it (older firmware, or a build without the helper)
has no Bluetooth entities, the services answer "this hub has no Bluetooth", and a config that lists Bluetooth devices is
refused by the hub with that reason.

## Entities

- `sensor` **Bluetooth** (diagnostic): `off`, `starting`, `ready`, `no_adapter` or `disabled` (the helper gave up after
  repeated failures; restart the hub). Attributes: `le`, `limits` (the hub's maxima), `window` (`host` / `remote` while a
  pairing window is open), `discoverable`.
- `binary_sensor` (connectivity) for each Bluetooth device and each Bluetooth remote **in the loaded config**, named after it.
  On while the hub holds a connection to it. Attributes: `address`, `kind`, `bonded`, `slot`, `transport` (devices),
  `battery` (remotes, percent, only once the remote has reported one). A configured peer the hub has not bonded yet is off.
  Bonds that are not in the config get no entity (the editor offers to add them).

Entity ids are keyed by address, not by slot, so editing the config order never moves them. A hub rename moves them with the
others (entity ids stay).

## Pairing

From the Harmony Editor add-on (Bluetooth view): pick "device" or "remote", start pairing, pair from the phone / TV, or pick
the remote from the scan list. Or by service:

- `ha_harmony.bt_pair`: `mode` (`host` default, `remote`), `transport` (`any`, `classic`, `le`), `seconds` (1 to 300, default
  120), optional `address`. Mode `remote` without an address scans; with an address from the scan it bonds that remote.
  The outcome is not the service's result: watch the Bluetooth sensor (`window`) and the connectivity sensors.
- `ha_harmony.bt_stop_pairing`: close the window.
- `ha_harmony.bt_forget`: `address` of a bond the hub holds (looked up in the hub's current list).
- `ha_harmony.bt_listen`: `on`, `seconds` (default 60, max 300). See below.

All take `config_entry` when several hubs are set up. Errors are plain sentences (for example "the Bluetooth adapter is not
ready yet", "that device is not in the list any more; scan again").

## Remote key events

The hub only sends a Bluetooth remote's keys to Home Assistant while something asked for them: `ha_harmony.bt_listen` (for
its `seconds`) or the editor's "press a key on the remote" while it is open. Meanwhile each key fires an event
`ha_harmony_bt_remote_key` with `hub_id`, `slot`, `page` (7 keyboard, 12 consumer), `usage`, `down` and, when known, the
remote's `address` and `name`. Normal key handling happens in the hub and needs none of this; no events fire otherwise.

## The editor relay

The add-on never talks to the hub. The editor's Bluetooth requests wait in the add-on, the integration collects them (every
5 s, every second while a window or a key listen is active or the editor has the view open), runs them on the hub and
reports the result and the hub's state back. If Home Assistant is down or the hub is offline the editor says so instead of
guessing. See [addon-api.md](addon-api.md) "Bluetooth relay".

## Diagnostics

The diagnostics download includes the hub's Bluetooth limits and last state, with every address redacted.
