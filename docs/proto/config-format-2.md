# Config format 2 (frozen contract)

Pushed by HA in `set_config.config`. Implemented by later tasks; this file only freezes the shape.
Format 1 (see `hub/engine/mapping.h`) has no `"format"` key; a hub accepts both, selected by `"format"`.

```json
{
  "format": 2,
  "devices": [
    {"id": "tv", "name": "TV",
     "power": {"on": <ir>, "off": <ir>},          // or {"toggle": <ir>}
     "inputs": {"hdmi1": <ir>},
     "settle_ms": 2000,                            // wait after power/input before next step
     "commands": {"vol_up": <ir>}}
  ],
  "activities": [
    {"id": "watch", "name": "Watch TV",
     "devices": [{"device": "tv", "input": "hdmi1"}],   // input optional
     "steps": [<action>, ...],                          // optional extra steps after power/input
     "off": [<action>, ...],                            // optional extra steps on power-off
     "keys": {"<code>": <action>}}                      // per-activity layer
  ],
  "keys": {"<code>": <action>}                          // default layer
}
```

## IR action
Exactly one of (plus optional `"port_mask"` 1..255, default 255 = all ports, and optional `"repeats"` 1..8,
default 1 = the frame is sent that many times in total, one frame period apart, like the SIRC copies; SIRC
stays at 3 or more. A hub older than this field ignores the key and sends the frame once):
- `{"proto": "nec"|"necext"|"samsung32"|"rc5"|"rc6"|"sirc12"|"sirc15"|"sirc20", "addr": int, "cmd": int}`
- `{"raw": {"hz": int, "us": [int, ...]}}` (mark/space durations in microseconds, mark first)
- `{"pronto": "0000 006D ..."}` (hex words, space separated)

Zero or several of the three forms is a load error.

Per-protocol ranges (addr max / cmd max); out of range is a load error:
`nec` 0xFF/0xFF (inverse bytes computed), `necext` 0xFFFF/0xFFFF (16-bit addr and 16-bit cmd sent
as given, no inverse), `samsung32` 0xFF/0xFF, `rc5` 0x1F/0x7F (cmd bit 6 set = RC5X, field bit 0),
`rc6` 0xFF/0xFF, `sirc12` 0x1F/0x7F, `sirc15` 0xFF/0x7F, `sirc20` 0x1FFF/0x7F.
Raw: even, non-zero count of durations, each 1..65535 us; `hz` 1000..1000000.

## Key maps
- Key code is the decimal string of 0..255: the logical id of a physical key (1-35, `keys:` in `proto/schema.yaml`; not the evdev code), soft keys 128-255 (touch widgets).
- Lookup order: current activity `keys`, then top-level `keys`; no match = forward to HA only.
- `<action>` is a format-1 action (`ir`, `bt`, `ha`, `none`) tagged by `"type"`; an `ir` action
  carries the IR action fields inline: `{"type":"ir", "proto":"nec", "addr":4, "cmd":8}`.
  A device reference `{"type":"device","device":"tv","command":"vol_up"}` resolves to
  `devices[].commands[command]`; `"power_on"`, `"power_off"` and `{"input":"<name>"}` are also accepted.

## Reconciliation with format 1 (deviations)
- Kept: `activities[].name/steps/off/keys`, top-level `keys`, decimal-string key codes, action `"type"`
  tags (`ir`/`bt`/`ha`/`none`), `port_mask`. The engine diff is the IR parser plus `devices`.
- Changed: `ir.pronto` was an int array in format 1; in format 2 it is a hex string (what HA/learners
  emit). Int-array pronto stays valid only when `format` is absent.
- Added: `format`, `devices`, activity `id` and `devices` list.
- Narrowed: key range 0..65535 -> 0..255 (the wire `key` is u8).
- Activity index (`state_update.activity`) remains array order.
- Limits: see "Limits" below.
- Engine details: `{"type":"activity","activity":"<id>"|"off"}` is accepted in key maps only (not in
  `steps`/`off`); activity `name` defaults to `id`, `"off"` is not a valid id; `power` is either `toggle`
  or both `on` and `off`; for toggle devices `power_on`/`power_off` both resolve to the toggle code.
  Every IR action is encoded at load time, so an out-of-range code fails `set_config`.
- Activity execution (device power/input tracking, transitions, `device_state`, `set_device_state`,
  the `--state` file and key HOLD timing) is specified in `docs/proto/activity-state.md`.

## Limits

Firmware <= 0.3.5 had fixed totals: 16 devices, 256 commands, 64 inputs, 16384 IR pool words,
128 keys per map, 128 KiB of config text. They were array sizes of one statically sized map
(`HP_MAP_MAX_COMMANDS 256` was a memory-sizing constant, never a wire limit). Current firmware has
no count limit for commands, inputs, the IR word pool or the key maps: every table is allocated for
the config being loaded, and the only size limit is ONE memory budget.

- **Streaming load.** The config text is never held whole in RAM and never parsed into one tree: the
  reader walks it (the cache file being received) and parses ONE value at a time with cJSON (a command, a
  device's header, `keys`, one activity), then frees it. Pass 1 counts what the tables need, pass 2 fills
  them. So memory is the final tables plus the value in hand; a raw `us` array costs its 2 bytes per
  duration, not a parsed tree. A single command may be at most 64 KiB of text, `keys` / one activity /
  `format` at most 128 KiB.
- **Budget** (`HP_MAP_BUDGET_BYTES`, 8 MiB; `hub.conf` `config_budget_kib`, hard-capped by the daemon at 20%
  of the memory free at its start and reported in `hello_ack.limits.budget`). It bounds the new map:
  `slack_b + devices * dev_b + (commands + inputs) * cmd_b + keys * key_b + pool_words * word_b`
  (`hp_map_estimate`; `harmony_core.hub_limits.estimate_bytes` is the same formula and
  `tests/c/test_budget.c` / `tests/py/test_limits.py` assert the same golden number). Checked before
  anything is allocated; over it: `-HP_E_TOOBIG`, `config needs ~N KiB of hub memory, the hub allows M KiB`.
  A load builds the new map beside the old one, so its peak is up to twice the budget: at most 40% of the
  free RAM at start (the Elite hub had 39.5 MB free with the daemon at ~1 MB RSS).
- **All-or-nothing, memory and flash.** The received text is written to `cache.json.new` as it arrives, the
  config is loaded from that file, and only then is the file renamed over `cache.json` and the new map
  swapped in. Any failure (bad config, over budget, out of memory, a full flash, a lost connection) deletes
  the temp file and leaves the previous map, its hash, the running activity AND the previous cache untouched.
  The hub refuses a transfer that cannot fit before writing (`config needs ~N KiB of flash, the hub has M KiB
  free`, from `statvfs` of the cache directory; `hello_ack.limits.flash_free` reports it so clients refuse
  first). The cache is read back the same streaming way at boot.
- **Lookups**: commands, inputs and devices are found through hash indexes built at load time
  (`hp_map_find_cmd`); nothing on the key path is linear in the number of commands.
- **Per-item bounds that remain**: 255 devices (uint8 index in the activity plan, the state file and
  `device_state` fan-out; the plan powers every other device off on each activity start), 16
  activities (uint8 on the wire in `state_update.activity`), 256 entries per key map (the key code space), 16
  steps per list and 8 devices per activity (activity plan size), 520 Pronto words / 512 raw durations
  per IR action (encoder), 31-byte names. The remote's page format has its own limits; commands never
  enter pages.
- **Reporting**: `hello_ack.limits` carries the budget, the estimate constants (`slack_b`, `dev_b`,
  `cmd_b`, `key_b`, `word_b`), `flash_free`, `chunk` and the per-item bounds. A hub that sends none is
  firmware <= 0.3.5: clients (`harmony_core.hub_limits.LEGACY`) apply the old fixed totals and do
  not use `config_chunk`. The add-on learns the connected hub's limits from the integration
  (`X-Hub-Limits` header on its polls) so the editor shows that hub's real caps.
- **Transport**: a WebSocket frame is capped at 64 KiB, so a config over ~40 KB is sent as
  `config_chunk {seq, total, data}` messages (<= `chunk` bytes of the compact text each, in order, each
  answered by a `config_ack`; the last carries the load result). The text is appended to the new cache file
  (header, text, crc32 trailer: the same file format as before); the hash is the CRC-32 of the text the
  client computed. An out-of-order or over-long chunk, or a new `seq 0`, drops the transfer.
- **Ceilings** (8 MiB budget, 32-bit hub, `tests/c/test_budget.c` + `harmony_core` estimate): about 56,000
  protocol-coded commands, 34,000 learned Pronto strings (~40 words) or 26,000 raw `us` arrays (70 durations)
  fit in RAM. Flash binds first for learned codes: the cache is stored as text, `/data` is a 5 MiB jffs2
  partition shared with the daemon, `remote_sim`, `rf_probe`, dropbear (about 2.4 MB) and a `hub.prev`, so
  realistically 1.5 to 2 MiB of config text fit uncompressed (the check does not count on jffs2 compression,
  which gets 10:1 on repetitive configs): about 35,000 protocol codes, 8,000 Pronto strings or 4,000 raw arrays.
  The refusal is exact: it uses the filesystem's free space at the time.
- **Sizes** (32-bit hub): `sizeof(hp_devcmd_t)` 104, `hp_device_t` 208, `hp_keybind_t` 72, the `hp_map_t` struct
  37 KB (was 250 KB, statically sized).
