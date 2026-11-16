# Home Assistant as the source of truth

Each hub is configured by one YAML file, `<HA config dir>/ha_harmony/<hub_id>.yaml` (the hub id is
the config entry's unique id, e.g. `sim-hub`). The integration validates it, compiles it to
[config format 2](proto/config-format-2.md) plus remote pages ([page format](proto/page-format.md)),
and pushes it to the hub. No file = the integration loads, logs one line, and leaves the hub's
current config alone. Errors name the file, line and YAML path, e.g.
`.../sim-hub.yaml:14: devices[0].commands.vol_up: nec cmd 999 outside 0..255`.

```yaml
devices:
  - id: tv
    name: TV
    settle_ms: 2000                       # wait after power-on / input select
    power: {on: {proto: nec, addr: 4, cmd: 2}, off: {proto: nec, addr: 4, cmd: 3}}
    inputs:
      hdmi1: {proto: nec, addr: 4, cmd: 9}
      hdmi2: {proto: nec, addr: 4, cmd: 10}
    commands:
      menu: {proto: nec, addr: 4, cmd: 20}
  - id: avr
    name: AV receiver
    settle_ms: 1500
    power: {toggle: {proto: rc5, addr: 16, cmd: 12}}
    inputs:
      tv_audio: {proto: rc5, addr: 16, cmd: 56}
      stream: {proto: rc5, addr: 16, cmd: 57}
    commands:
      vol_up: {proto: rc5, addr: 16, cmd: 16, port_mask: 3}   # only IR ports 1 and 2
      vol_down: {proto: rc5, addr: 16, cmd: 17}
      mute: {proto: rc5, addr: 16, cmd: 13}
  - id: box
    name: Streaming box
    power: {on: {pronto: "0000 006D 0001 0000 0018 0018"}, off: {raw: {hz: 38000, us: [9000, 4500, 560, 560]}}}
    commands:
      play_pause: {proto: sirc12, addr: 1, cmd: 26}
      ok: {proto: sirc12, addr: 1, cmd: 11}

activities:
  - id: watch_tv
    name: Watch TV
    devices: [{device: tv, input: hdmi1}, {device: avr, input: tv_audio}]
  - id: stream
    name: Stream
    devices: [{device: tv, input: hdmi2}, {device: avr, input: stream}, {device: box}]
    steps: [{device: box, command: ok}]            # extra steps after power/inputs
    off: [{device: box, command: play_pause}]      # extra steps on power-off
    keys:                                          # per-activity layer
      ok: {device: box, command: ok}
      play: {device: box, command: play_pause}

keys:                                              # default layer, by symbolic key name
  vol_up: {device: avr, command: vol_up}
  vol_down: {device: avr, command: vol_down}
  mute: {device: avr, command: mute}
  home: {activity: "off"}
  menu: {device: tv, command: menu}

pages:
  - id: main
    cols: 4
    rows: 6
    widgets:
      - {type: label, x: 0, y: 0, w: 4, label: Living room}
      - {type: button, id: mute, x: 0, y: 1, w: 2, h: 1, label: Mute, action: {device: avr, command: mute}}
      - {type: button, id: off, x: 2, y: 1, w: 2, h: 1, label: All off, action: {activity: "off"}}
      - {type: tile, id: temp, x: 0, y: 2, w: 4, h: 2, label: Temp, entity: sensor.living_room_temperature}
```

## Reference
- **IR action** (devices and anywhere an action is expected): exactly one of `{proto, addr, cmd}`
  (`nec necext samsung32 rc5 rc6 sirc12 sirc15 sirc20`), `{raw: {hz, us: [...]}}`, `{pronto: "..."}`,
  `{flipper: {file, signal}}` or `{smartir: {file, command: "cool.24.auto"}}`; optional `port_mask` 1..255.
  Files are read relative to the YAML file's directory and may not leave it.
- **Action** in `keys`, `steps`, `off` and page buttons: an IR action; `{device, command}` (a command,
  `power_on`/`power_off`); `{device, input}`; `{activity: <id>|"off"}` (not in `steps`/`off`);
  `{event: name}` (HA only); `{bt: usage}`; `{none: true}`.
- **Device command names** are free text (max 31 bytes) but `power_on`, `power_off` and names starting with
  `input_` are reserved for the built-ins and rejected. Home Assistant's device buttons and
  `remote.send_command` use the hub's `device_command` verb, so a toggle device is never toggled the
  wrong way.
- **Keys** use the symbolic names in `KEY_CODES` (`custom_components/ha_harmony/const.py`, provisional) or
  `key_<0..127>`. Event entities report that name; page widgets report `tap_<widget id>`.
  The `remote_sim` test tool's `--press`/`--hold` take the numeric key **code** (a value from `KEY_CODES`), not the name.
- **Page widgets**: `button` (needs `action`), `tile` (shows the state of `entity`, updated on change),
  `label`. Grid up to 16x16, 32 widgets per page, 128 widgets over all pages, labels up to 24 bytes.
  A page's id on the remote is its position in `pages`.
- **Hub limits** (`hub/engine/mapping.h`): 16 devices, 256 commands, 64 inputs, 16 activities,
  128 keys per map, 16 steps per list, 8 devices per activity, 128 KiB of config. Over-limit configs
  fail with a message and nothing is pushed.

## Operation
The config is pushed on connect only if the hub's `config_hash` differs from the compiled one. Services:
`ha_harmony.reload_config` (re-read, recompile, push if changed), `push_pages`, `send_ir`,
`set_device_state` (tell the hub a device was switched by hand). Rejections by the hub raise a
repair issue and a log error; the previous config keeps running.
