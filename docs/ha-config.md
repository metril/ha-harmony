# Home Assistant as the source of truth

Each hub is configured by one document in the schema below, kept in the **Harmony Editor add-on**
(draft, applied, last 5 versions per hub id). The integration (0.3.0+) pulls the *applied* config
over Supervisor discovery + long-poll, **re-validates and compiles it itself** (never trusting the
add-on) to [config format 2](proto/config-format-2.md) plus pages ([page format](proto/page-format.md)),
pushes it to the hub, and caches it in `.storage` so the remote stays configured while the add-on is
stopped. A refused config (bad schema, wrong hub, IR file references, too large) keeps the previous
one and raises a repair issue; the *Add-on link* sensor shows `ok`/`unreachable`/`rejected`/...
Nothing applied yet = the hub's own config is left alone.
**Migrating from `<config>/ha_harmony/<hub_id>.yaml`** (no longer read; one repair issue, file left
alone): import it in the add-on, then apply. See [CHANGELOG](../CHANGELOG.md).

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
      select: {device: box, command: ok}
      play: {device: box, command: play_pause}

keys:                                              # default layer, by symbolic key name
  volume_up: {device: avr, command: vol_up}
  volume_down: {device: avr, command: vol_down}
  volume_mute: {device: avr, command: mute}
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
  File references are only accepted by the add-on's importer, which inlines them; the integration
  refuses any that remain.
  Optional `repeats` 1..8 (default 1): the hub sends the frame that many times in total, one frame period
  apart (`tools/stock_to_ha_config.py` emits it for stock's `min_repeats` on frame-only signals). A hub
  older than this field ignores it and sends one frame.
- **Action** in `keys`, `steps`, `off` and page buttons: an IR action; `{device, command}` (a command,
  `power_on`/`power_off`); `{device, input}`; `{activity: <id>|"off"}` (not in `steps`/`off`);
  `{event: name}` (HA only); `{bt: usage}` (a consumer usage; see [Bluetooth devices](#bluetooth-devices-and-remotes)); `{none: true}`.
  `{ha: {service, target, data, repeat}}` (Home Assistant only): see [Home Assistant actions](#home-assistant-actions).
- **Device command names** are free text (max 31 bytes) but `power_on`, `power_off` and names starting with
  `input_` are reserved for the built-ins and rejected. Home Assistant's device buttons and
  `remote.send_command` use the hub's `device_command` verb, so a toggle device is never toggled the
  wrong way.
- **Device `source`** (optional mapping of scalars): where an imported device came from, written by
  `tools/irdb_import.py` (`kind: harmony_archive, mfr, model_file, codeset, imported_at`). Never compiled
  or sent to the hub.
- **Device `icon`** (optional, a name from `proto/icons.yaml`; valid on `ir`, `ha` and `bt` devices):
  the icon the editor shows for the device, and the default icon of a page widget whose action targets
  that device (`{device: tv, command: ...}`) when the widget sets no `icon` of its own (`icon: none`
  still means no icon). Without it the widget icon comes from its label as before. No change to the
  firmware or the page format: widgets already carry their icon.
- **Device `aliases`** (optional list of names, max 31 bytes each): other names the device answers to
  in `remote.send_command` and `ha_harmony.set_device_state`. Resolved in Home Assistant and never
  sent to the hub. Meant for migration: `tools/stock_to_ha_config.py --device-list` sets each
  device's stock numeric id and stock name as aliases, so an automation written for the stock
  integration (`device: "51887071"`) needs only its `entity_id` changed. An alias may not equal
  another device's id or be shared by two devices.
- **Keys** are the Elite's 35 physical keys, written as the snake_case form of stock's name
  (`volume_up`, `select`, `home_control_1`, ...; the stock spelling `VolumeUp` is accepted too) or
  `key_<0..127>` for a raw logical code. The table is `proto/schema.yaml` (`keys:`): up, down, left,
  right, select, menu, exit, volume_up, volume_down, volume_mute, channel_up, channel_down,
  prev_channel, play, pause, stop, record, rewind, fast_forward, dvr, guide, info, red, green,
  yellow, blue, home_control_1..4, home_control_plus, home_control_minus, power_off_activity, home,
  favorites. There are no number keys and no back key (it is `exit`). An unknown name is rejected
  and the error lists the valid ones. Event entities report that name; page widgets report
  `tap_<widget id>`. The `remote_sim` test tool's `--press`/`--hold` take a key **name**
  (`VolumeUp` or `volume_up`) or the numeric logical code.
- **Page widgets**: `button` (needs `action`), `tile` (shows the state of `entity`, updated on change),
  `label`. Grid up to 16x16, 32 widgets per page, 128 widgets over all pages, labels up to 24 bytes.
  A page's id on the remote is its position in `pages` (a list page takes one id per group).
  Only buttons and tiles take a soft-key id (`128+n`); labels and `nav` buttons never do.
- **Page keys** (page format v2; v1 remotes ignore them): `title:` (max 24 bytes; else a non-empty
  full-width label at row 0 becomes the header title, is dropped, and the rows shift up by one),
  `kind: home|favorites` (at most one each), `activity: <id>` (auto-shown when it starts), `back: true`,
  `list: {rows: N}` (replaces `cols`/`rows`; widgets omit `x`/`y` and become one full-width row each;
  the compiler splits into groups of N rows or 251 bytes, at most 15; the remote pages with Up/Down).
- **Tile taps**: a tile takes `tap:` (an action, `toggle`, or `none`). Left out, a tile on a `light`, `switch`,
  `fan`, `cover`, `input_boolean`, `media_player` or `remote` entity toggles it (`<domain>.toggle`), a `scene` or
  `script` is turned on, a `button` / `input_button` is pressed; other domains do nothing. `tap: toggle` on a
  domain with no such default is an error. The tap is the tile's soft key (`tap_<id>`), so an `ha:` tap runs in
  Home Assistant and any other action runs on the hub.
- **Widget keys**: `icon: <name>|none` (`proto/icons.yaml`), `role: default|primary|secondary|danger`,
  `shape: card|row|pill|monogram` (list pages default `row`), `icon_only: true` (label kept, not drawn),
  `nav: next|prev|home|<page id>` (button without `action`; handled on the remote, no key event).
- **Inference** (compiler only; explicit keys always win): `{activity: off}` or a label like "power
  off"/"all off" -> role `danger`, icon `power`; `{activity: X}` -> `bind` X (active highlight) and an
  icon from X's name keywords; else an icon from the label's words; tile -> icon from the entity
  domain (light bulb, climate thermo, ...); `nav` -> chevron/home icon. A page over 251 bytes fails
  with `page 'x': N bytes over; shorten labels, use icon_only, or make it a list`.
- **Hub limits** (`hub/engine/mapping.h`, reported by the hub in `hello_ack.limits`; see
  `docs/proto/config-format-2.md` "Limits"): current firmware has one memory budget instead of command,
  input and IR-pool counts (the editor shows "Config size X of Y KiB on this hub"), plus 255 devices,
  16 activities, 256 keys per map, 16 steps per list, 8 devices per activity. Firmware <= 0.3.5 still
  has 16 devices, 256 commands, 64 inputs, 128 keys per map and 128 KiB of config; the compiler
  applies those to a hub that reports no limits. Over-limit configs fail with a message and nothing
  is pushed.

- **Command buttons** (behaviour change): each device command used to become a button entity.
  - Existing buttons are kept exactly as they are: enabled stays enabled, and nothing is removed.
  - New command buttons are created **disabled**; enable the ones you use in the entity list.
  - A device with more than 200 commands gets no (new) buttons unless the integration option "Create command
    buttons for devices with more than 200 commands" (`all_command_buttons`) is on.
  - Every command is always available, whatever entities exist, through the `remote.send_command` action on the
    hub's remote entity, with `device` (the device id) and `command` (the command name):

    ```yaml
    action: remote.send_command
    target: {entity_id: remote.living_room}
    data: {device: tv, command: input_hdmi2}
    ```

### Colours and themes
Optional; a config without `color`/`theme` compiles to the same pages as before. Colours are widget keys,
a theme is chosen once per config (one active theme per remote):

```yaml
themes:                          # your own; `extends` a preset (default, midnight, daylight, high_contrast) or another of yours (<= 4 deep)
  dusk: {extends: midnight, tokens: {ACC: "#FF8A3D"}, palette: {3: {fill: "#2E7D32"}}}
theme: dusk                      # a preset or one of yours; default: "default"
pages:
  - {id: p, cols: 3, rows: 1, widgets: [
      {type: button, id: a, x: 0, y: 0, label: Netflix, color: red, action: {none: true}},
      {type: button, id: b, x: 1, y: 0, label: Custom, color: "#3355FF", action: {none: true}},
      {type: tile, id: t, x: 2, y: 0, entity: light.x, on_color: amber}]}
```

- **Palette names**: red, orange, amber, yellow, lime, green, teal, cyan, blue, indigo, purple, pink (index 1..12; a
  theme may change a fill by index). Text on the fill is chosen by the remote. Costs 1 byte per coloured widget.
- **Free colours** `"#RRGGBB"` (rounded to 5-6-5 bits): 2 more bytes per colour, so a page can run out of its 251
  bytes; the error then says `page 'tv': 6 bytes over (3 custom colours = 6 B, 2 colour bytes = 2 B); use palette
  colours, ...`. `on_color` is only for tiles (the fill while on). A free colour whose best text colour is under
  4.5:1 gives a warning with the nearest colour that passes.
- **Themes**: `tokens` (bg, s1, s2, s3, tx, tx2, tx3, acc, onacc, warm, onwarm, bad, onbad; names are case-insensitive)
  and `palette: {1..12: {fill}}` override the parent. A user theme cannot reuse a preset's name. Contrast problems
  (e.g. `tx on bg`) are warnings, never errors; the pages still build. A theme edit re-sends the pages.
- **Older remote firmware** (the remote reports `caps` bit 2 = colours, bit 3 = custom colours and themes): without
  bit 2 colours and the theme are dropped (role colours); with bit 2 only, free colours snap to the nearest palette
  entry and a user theme shows as its base preset. A remote whose firmware is updated gets its pages again. What was
  lost is listed in the *Remote N page format* sensor (`theme_notices`) and in the diagnostics. A user theme is sent
  to the remote with `theme_push` (docs/proto/themes-transport.md) before its pages; with an older hub firmware (no
  `theme_push`) bit 3 counts as bit 2 only. The sensor's `theme` attribute says whether the hub took it and the remote holds it.

## Home Assistant actions

`{ha: {service: "domain.service", target: {entity_id: <id or [ids]>}, data: {...}, repeat: false}}` is an
action kind that runs in **Home Assistant**, not on the hub. It is accepted wherever an action is: hard
keys (default and per-activity layers), page buttons, tile taps, and the `steps` / `off` of an activity.

- `service` is `domain.service` (`^[a-z_]+\.[a-z0-9_]+$`) with the domain in the allow-list: `light switch scene
  script media_player remote fan cover climate input_boolean input_button button number select`
  (`remote.send_command` covers Android TV / Roku style remotes; `homeassistant.toggle` and
  `shell_command` are not allowed). `target.entity_id` is required (1..8 ids). `data` is small JSON only:
  at most 1 KiB, 64 values, 4 levels deep, strings up to 200 characters, and **no templates** (any string
  containing a double brace, `{%` or `{#` is refused); `entity_id`, `device_id`, `area_id` and the like belong
  in `target`.
- The add-on validates and the integration validates again when it compiles the bundle, and checks the
  allow-list once more at dispatch. A hostile bundle is rejected as a whole and the previous config stays.
- The hub never sees these. The key or button stays an ordinary forwarded key (`key_event`), and the compiler
  hands Home Assistant a table keyed by *(activity layer, key code)* built with the same rule as the hub's
  lookup (the activity's own entry, else the default layer; the X-ray view uses the same function). When an
  activity maps a key to an `ha:` action that the default layer maps to IR, the hub gets a `none` there so the
  inherited IR is not sent as well.
- On a key press the integration looks the key up for the hub's **current activity** (the last `state_update`)
  and calls the service with `blocking=False` in the same callback, with its own context. Nothing is awaited
  before the call. `repeat: false` (default) fires once on PRESS. `repeat: true` also fires on every HOLD the
  remote sends (one per 400 ms while the key stays down) and stops at RELEASE; there is no timer in Home
  Assistant. A service or entity that does not exist logs one warning per (service, entity) per config
  revision and never raises. The *Remote N keys* event entity still fires for every key, so automations keep
  working.

### Home Assistant devices

A device with `kind: ha` has no IR: its `power` (`on` and `off`), `inputs` and `commands` are `ha:` actions,
and the optional `entity` is where its state comes from (the `source` attribute is the input; the entity may be
renamed in Home Assistant, the device follows). Power by domain: `cover` open / opening = on, closed / closing =
off; `lock` unlocked / unlocking / open = on, locked / locking = off; `media_player` anything but `off` and
`standby` = on; every other domain (light, switch, fan, `climate` whose state is its hvac mode, ...) on unless
`off`. `unavailable` and `unknown` (and a state a `cover` or `lock` has no rule for, such as `jammed`) are an
unknown power. Everything else works as for IR devices:
`{device: shield, command: ok}` on a key or button becomes that service call, the device gets its buttons,
`remote.send_command` and a power sensor, and activities may list it with an `input`. It does not count
against the hub's device limits and never reaches the hub.

**Order when an activity starts or stops** (the hub runs its IR sequence as always; Home Assistant runs its
steps once the hub *reports the result*, so they come after the TV's settle time): when the hub reports
activity B running, Home Assistant (1) powers off the HA devices of the previous activity that B does not use,
(2) when everything went off, runs the previous activity's HA `off` steps, then for B (3) powers on its HA
devices that are not already on (the entity is the truth), (4) selects their inputs, waiting the device's
`settle_ms` first if it was just powered on, and (5) runs B's HA `steps`. The first `state_update` after a
start of Home Assistant is only a baseline (no steps run when it restarts); a change that happened while Home
Assistant and the hub were disconnected is compared with the activity known before the drop and its steps run. A newer transition cancels the
steps still waiting. HA steps and IR steps of one list are not interleaved: IR steps run first.

### Bluetooth devices and remotes

The hub can act as a Bluetooth HID keyboard/consumer-control device towards up to **255** hosts (TVs, boxes), and
accept keys from up to **32** physical Bluetooth remotes. Both are configuration only here; the Bluetooth link is
made by the hub.

```yaml
devices:
  - id: shield
    kind: bt
    name: Shield
    address: aa:bb:cc:dd:ee:ff     # optional until paired; normalised to upper case (quote it if it is all digits)
    transport: auto                # auto (default) | classic | le
    commands:                      # optional: omitted = every built-in name (not letters, F-keys, punctuation)
      Home: Home                   # <command name>: <built-in name> | HID:<page hex>:<usage hex>
      Search: "HID:0C:0221"
      Ok: OK

bt_remotes:
  - name: Homatics
    address: "11:22:33:44:55:66"   # optional
    keys:                          # optional; <built-in name | HID:pp:uuuu>: <key name>, added to the defaults
      Red: red
      "HID:0C:0223": favorites
```

- A `kind: bt` device is used like an IR device: `{device: shield, command: Home}` in `keys`, `steps`, `off`, page
  buttons and tiles; it gets the same command buttons (disabled by default, none above 200 commands) and
  `remote.send_command`. It has no `power` or `inputs`. In a `steps`/`off` list a Bluetooth command is a tap (key
  down, key up); on a physical key it is key down at the press and key up at the release. The device's slot
  (`host`, 0..254) is its index among the `kind: bt` devices in file order; the compiled config carries a top-level
  `bt_hosts: [{slot, addr|null, name, transport}]`.
- **Codes**: only the keyboard page (`07`, usages `0001`..`00FF`) and the consumer page (`0C`, `0001`..`03FF`)
  exist. A command value is a built-in name (`DirectionUp`, `Select`/`OK`, `Back`, `Home`, `Menu`, `Info`, `Play`,
  `Pause`, `PlayPause`, `Stop`, `SkipForward`, `SkipBack`, `FastForward`, `Rewind`, `VolumeUp`, `VolumeDown`, `Mute`,
  `PowerToggle`, `ChannelUp`, `ChannelDown`, `Red`..`Blue`, `Number0`..`Number9`, `F1`..`F12`, `a`..`z`, and more;
  case does not matter, a few short aliases such as `up`, `ok`, `vol_up` work), `0x0221` (a consumer usage), or
  `HID:<page>:<usage>` such as `HID:0C:0223`.
- **`bt_remotes`**: each entry's `keys` map a usage to a key name from the same set as the Elite's hard keys (`up`,
  `volume_up`, `key_<n>`, ...), so the activity key maps apply to a Bluetooth remote unchanged. Without `keys` (and
  under your entries) a default map applies: arrows, Select/Enter, Back/Escape (`exit`), Home, Menu, Info,
  Play/PlayPause (`play`), Pause, Stop, FastForward, Rewind, volume, Mute, channels, PowerToggle
  (`power_off_activity`), the colour keys and digits 0..9 (`key_100`..`key_109`). Compiled as
  `bt_remotes: [{slot, addr|null, name, keys: [[page, usage, key_id], ...]}]`; the keys are counted as key-map
  entries, and every host and remote as a device, in the hub's memory budget. `Usage: null` removes a default
  (that button then does nothing); older configs without it are unchanged. A usage that is not mapped is ignored.
- Errors are located: an address that is not `AA:BB:CC:DD:EE:FF`, a page other than 07/0C, an unknown name or key,
  more than 255 devices or 32 remotes. The hub's own table sizes will be compared at push time (not yet).

### Which pages a remote gets

Each remote announces its page format (`remote_info`). After a link-up the integration waits up to 1.5 s for
it: nothing arrives -> page format v1; `page_fmt` 2 -> v2. A later `remote_info` with another format re-pushes
in that format. The hub's first `remote_info` after each link-up is looked at again (the remote may have
rebooted): it also reports the generation (`gen`) and page count (`pages`) of the set the remote holds, and when
format, `gen` and `pages` equal what would be sent nothing is pushed (only tile values that changed meanwhile);
otherwise the whole set is. So a Home Assistant restart, a websocket reconnect or a reload does not resend pages the remote
already has, and one link-up causes at most one push. Pages are only pushed for a config the hub has accepted. Pages go out one at a time (each command result is the
backpressure); a newer config cancels the running push and the new set starts only after it has stopped.
The *Remote N page format* diagnostic sensor shows `unknown` / `v1` / `v2` with the capabilities. For a remote
with the `tile_state` capability a tile also carries a state (on / off / unavailable / value) and a short
formatted value (units, rounded numbers, brightness as a percentage, at most 31 bytes); other remotes get the
legacy text. Only entities on pages are watched, bursts are coalesced (about 100 ms, latest value wins), only a
changed rendering is sent, every tile is sent again after a page push, and nothing is sent while no remote is
attached.

### Example: Shield over Android TV, a toggling light tile, a scene button

```yaml
devices:
  - id: tv
    name: TV
    settle_ms: 5000
    power: {on: {proto: samsung32, addr: 7, cmd: 2, repeats: 3}, off: {proto: samsung32, addr: 7, cmd: 98, repeats: 3}}
    inputs: {hdmi1: {proto: samsung32, addr: 7, cmd: 1, repeats: 3}}
    commands: {vol_up: {proto: samsung32, addr: 7, cmd: 7, repeats: 3}, vol_down: {proto: samsung32, addr: 7, cmd: 11, repeats: 3}}
  - id: shield
    kind: ha
    name: Shield
    entity: media_player.shield
    settle_ms: 1500
    power:
      on: {ha: {service: remote.send_command, target: {entity_id: remote.shield}, data: {command: HOME}}}
      off: {ha: {service: remote.send_command, target: {entity_id: remote.shield}, data: {command: POWER}}}
    inputs:
      home: {ha: {service: remote.send_command, target: {entity_id: remote.shield}, data: {command: HOME}}}
    commands:
      ok: {ha: {service: remote.send_command, target: {entity_id: remote.shield}, data: {command: DPAD_CENTER}}}
      down: {ha: {service: remote.send_command, target: {entity_id: remote.shield}, data: {command: DPAD_DOWN}, repeat: true}}
      up: {ha: {service: remote.send_command, target: {entity_id: remote.shield}, data: {command: DPAD_UP}, repeat: true}}
activities:
  - id: shield_tv
    name: Shield TV
    devices: [{device: tv, input: hdmi1}, {device: shield, input: home}]
    keys:
      select: {device: shield, command: ok}
      up: {device: shield, command: up}
      down: {device: shield, command: down}
keys:
  volume_up: {device: tv, command: vol_up}
  volume_down: {device: tv, command: vol_down}
pages:
  - id: home
    kind: home
    title: Living Room
    cols: 2
    rows: 4
    widgets:
      - {type: tile, id: lamp, x: 0, y: 0, w: 2, label: Lamp, entity: light.living_room}        # tap toggles it
      - {type: button, id: relax, x: 0, y: 1, w: 2, label: Relax, action: {ha: {service: scene.turn_on, target: {entity_id: scene.living_room_relax}}}}
      - {type: button, id: tv, x: 0, y: 2, w: 2, label: Shield TV, action: {activity: shield_tv}}
```

`remote.send_command` is Home Assistant's own service on `remote.shield` (the Android TV Remote
integration); the keys and buttons are forwarded to Home Assistant by the hub, so they need Home Assistant
to be up.

## Naming your hub

The hub id (`ha-harmony-<hostname>` by default) keys the config entry, every entity and device identifier, the cached
config and the add-on's store. The **display name** is free text (up to 48 bytes) shown as the device name, entry
title and editor header (the id stays as secondary text).

**Rename:** *Settings > Devices & services > Harmony > ⋮ > Reconfigure*, fields **Hub ID** (letters, digits, `_` `-`,
1 to 63 characters, starting with a letter or digit, not `default`) and **Name**; needs hub firmware with
`set_hub_id`, and an up-to-date add-on if you use one (an old add-on cannot move its store: the rename is refused).
Home Assistant sends the id to the hub (stored on the hub, announced by mDNS at once), moves everything keyed by the old
id (entry, devices, entity *unique* ids, cache, add-on store) and reloads. Entity ids do not change, so history,
dashboards and automations keep working (entity ids derived from the old id keep their names).

**All or nothing.** If a step fails (hub refuses, id taken, add-on already holds a config under the new id, ...) it is
all undone, the hub goes back to its old id and the form says why. If even that undo cannot reach the hub, Home
Assistant follows the hub's new id by its hardware id at the next start.

**Same hub, new id.** mDNS carries `hw=<hardware id>` (MAC derived), so discovery and setup recognise a renamed hub and
the entry follows an id changed on the device (a different hardware id on the same address is refused). If the add-on
then holds configs under both ids, both are kept and a repair issue says what to do. A hub with no readable MAC has no
hardware id: it cannot be renamed from Home Assistant.

**Pinning (advanced).** `hub_id=<id>` in `/data/ha-harmony/hub.conf` (or `tools/install_boot_hook.py --hub-id`) pins the
id; Home Assistant then refuses to rename it (`id_pinned`), the name still works. An unusable pin (`default`, over 63
characters) is logged and ignored (not pinned); the daemon never exits over an id. The default is saved on first boot. Precedence: pin, then the id saved by Home
Assistant (`/data/ha-harmony/hub_id`), then the hostname default. Delete `hub_id` and `hub_name` there to reset. Older
dotted ids keep working (announced on mDNS with `-` for the dot); new ids cannot contain a dot.

Old firmware (no `set_hub_id`): the form shows id and name read-only. More: `docs/proto/hub-identity.md`.

## Operation
The config is pushed on connect only if the hub's `config_hash` differs from the compiled one. Services:
`ha_harmony.reload_config` (pull from the add-on now, recompile, push if changed; fails when the hub is
not linked), `push_pages`, `send_ir` (no file references), `set_device_state` (tell the hub a device was
switched by hand). Rejections by the hub raise a repair issue and a log error; the previous config
keeps running. Pages are only pushed for a config the hub has accepted (its reported hash equals the compiled one).

**Service calls and errors.** A key press or step that calls a Home Assistant service only logs a failure (nothing
waits for it); a held `repeat: true` key drops a repeat while the same call is still running, so a slow service never
queues, and stops with the release. Calls the user starts (a device button, `remote.send_command`) raise a translated
error. `remote.send_command` honours `num_repeats`, `delay_secs` (default 0.4 s) and `hold_secs` (the command is
resent every 0.4 s until that time has passed).

**Key events.** The *Remote N keys* event entity fires on press and release; hold repeats are an option (*Configure*
on the integration, off by default: one recorder row per 0.4 s of holding). Home Assistant key actions with
`repeat: true` are not affected. Event attributes are `key` and `action`.

**Connection changes.** *Reconfigure* changes host, port and token, and the hub's id and name (see *Naming your hub*). The hub's mDNS announcement never retargets a
configured hub by itself: if a hub id is announced at another address Home Assistant offers *Hub moved to
`<host>`?* and sends the token only after you confirm (anyone on the LAN can answer mDNS).

**Linking the add-on.** The confirm step names the add-on (slug and address). The slug check (`*_ha_harmony_editor`)
is the only authentication of the Supervisor's discovery message, so the residual risk is another installed add-on
whose slug ends the same way; check the name and address shown before confirming. Diagnostics (*Download
diagnostics*) redact tokens and addresses.
