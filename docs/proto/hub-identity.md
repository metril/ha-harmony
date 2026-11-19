# Hub identity: id, name, hardware id

Additive to protocol version 1 (see [compat](compat.md)); an older peer ignores every field and message below.

## Messages

| code | name | transport | fields |
|------|------|-----------|--------|
| 26 | `set_hub_id` | ws, authenticated | `id: str`, `name: str?` |
| 27 | `hub_id_ack` | ws | `hub_id: str`, `name: str?` |

`set_hub_id` changes the hub's id and display name. `id` is required (sending the current id leaves it as it is, which
is how a rollback or a name-only change looks). `name` absent keeps the stored name, `""` clears it. The hub validates
before it touches anything:

- id: `[A-Za-z0-9_-]`, 1 to 63 characters, first character a letter or digit, not `default` (no dot: it is not valid in an mDNS label; ids with a dot that were already pinned in hub.conf keep working and are announced with `-` for the dot);
- name: valid UTF-8, at most 48 bytes, no control characters (below 0x20, 0x7f).

It saves the values atomically (temp file, fsync, rename, directory fsync) and only then applies them: the next
`hello_ack` carries them and the mDNS record is re-announced under the new name (goodbye for the old one first).
If the second of the two files cannot be written the first is restored; on any error nothing has changed. Reply: `hub_id_ack`
(same request id) with the id and name now in effect, or `error` with one of

| code | meaning |
|------|---------|
| `bad_id` | the id breaks the rule above |
| `bad_name` | the name breaks the rule above |
| `id_pinned` | `hub_id` in hub.conf pins the id (`--hub-id`); the daemon never rewrites hub.conf. The same id (and a name) is still accepted |
| `io` | the state directory could not be written |
| `unsupported` | the hub has no identity store (never on a real hub) |

## hello_ack additions (all optional)

| field | meaning |
|-------|---------|
| `name` | the display name; absent when none is set |
| `hw_id` | stable hardware id: the first usable MAC of wlan0, eth0, wlan1, eth1, br0, ath0, as 12 lowercase hex; absent when the hub cannot read one. The same hub keeps it across renames |
| `rename` | `true`: the hub understands `set_hub_id` (absent on older firmware) |
| `id_pinned` | `true`: hub.conf pins the id |

The mDNS TXT record (`hub_id=<id>`, `v=<proto>`) gains `hw=<hw_id>` when the hub has one.

## Where it is stored, and precedence

The id is the first of: (1) `--hub-id` (hub.conf `hub_id`, installed by `tools/install_boot_hook.py --hub-id`): pinned;
(2) the file `hub_id` in the daemon's state directory (the directory of `--state`, `/data/ha-harmony`; `--ident-dir`
overrides it), written by `set_hub_id`; (3) the default, `ha-harmony-<hostname>`. The name is the file `hub_name` there.
An unreadable or invalid file is ignored. Delete both files to go back to the default.

## Who moves what on a rename

The hub only changes its own answer. The Home Assistant integration moves every record keyed by the id (see
`docs/ha-config.md`, "Naming your hub") and tells the add-on (`docs/addon-api.md`, "Renamed hubs"); it matches the
same hub across ids by `hw_id`.
