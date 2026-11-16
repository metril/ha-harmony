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

## Key maps
- Key code is the decimal string of 0..255: physical keys 0-127, soft keys 128-255 (touch widgets).
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
- Limits are inherited from `mapping.h` (`HP_MAP_MAX_*`) until the engine task revises them.
