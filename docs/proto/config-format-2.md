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
Exactly one of (plus optional `"port_mask"` 1..255, default 255 = all ports):
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
- Limits: see `hub/engine/mapping.h` (`HP_MAP_MAX_*`): 16 devices, 256 commands and 64 inputs in total,
  16 activities, 128 keys per map, 16 steps per list, 8 devices per activity, 128 KiB config text.
- Engine details: `{"type":"activity","activity":"<id>"|"off"}` is accepted in key maps only (not in
  `steps`/`off`); activity `name` defaults to `id`, `"off"` is not a valid id; `power` is either `toggle`
  or both `on` and `off`; for toggle devices `power_on`/`power_off` both resolve to the toggle code.
  Every IR action is encoded at load time, so an out-of-range code fails `set_config`.
- Activity execution (device power/input tracking, transitions, `device_state`, `set_device_state`,
  the `--state` file and key HOLD timing) is specified in `docs/proto/activity-state.md`.
