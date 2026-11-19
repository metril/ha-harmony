# Changelog

## Unreleased

- Fix: the editor applied the config to hub id `default` while the integration polled the add-on with its own hub id, so nothing was ever fetched or acknowledged and the apply dialog waited forever. The add-on now records the hub ids integrations poll for (`GET hubs`, `config/status`), the editor opens on the linked hub id (an explicit `?hub=` still wins; a switcher appears when several are linked), and a config stored under `default` moves to the single linked hub id automatically (several: an explicit "move" action, `POST hubs/adopt`).
- The apply dialog and the editor header say "Home Assistant is not polling for this hub (id)" with the linked ids, and show applied vs acknowledged rev after you navigate away.
- The Add-on link sensor says "No config applied for this hub in the add-on" instead of `connecting` when the add-on has nothing for the hub id, and exposes `hub_id`.

## 0.3.4 (2026-11-18)

- The editor's drawing of the Elite now matches the real remote: Off above the screen, ACTIVITIES and DEVICES strip buttons, play/pause centre column, Exit left and Menu right, single Vol and Ch/Pg rockers around the D-pad, Mute and Back, DVR/Guide/Info, tinted colour keys, and the 2x2 home-control block with a +/- rocker. Key positions were measured on the device (the Back key is PrevChannel).

## 0.3.3 (2026-11-18)

- Fix: the add-on never saw its SUPERVISOR_TOKEN (log: "SUPERVISOR_TOKEN is not set"), so discovery, the Home Assistant entity pickers and IR test-fire were all off. s6-overlay v3 does not pass the container environment to services; the editor run script now starts with `#!/command/with-contenv sh`. The manifest grants were checked against the endpoints the code calls and are unchanged.

## 0.3.2 (2026-11-18)

- Fix: the editor service never started under a Supervisor (UI: "app seems to not be ready"). The AppArmor
  profile's file rules did not cover the directory /etc/services.d, so s6 found no service to start. The profile
  now uses the official example's broad `file,` with host mappings denied. The service logs a "listening on" line.

## 0.3.1 (2026-11-18)

- Add-on: fix start-up failure under a Supervisor (AppArmor profile now grants read on the s6 scripts).

## 0.3.0 (2026-11-18)

### BREAKING

- **The config now comes from the Harmony Editor add-on.** The `<config>/ha_harmony/<hub_id>.yaml` file is no
  longer read and there is no fallback: upload it in the editor (import YAML, review, apply). See
  *Migrating from the YAML file* below.
- **Hold-repeat key events are opt-in.** Hold repeats no longer fire `event` entities unless enabled in the
  integration's options; Home Assistant actions with `repeat: true` are unaffected.
- **The key event's `remote` attribute is gone** (the event entity is per remote): update automations that read it.
- **Hub and remote firmware must be updated together** (page format v2, `remote_info` with `gen` and `pages`). A
  mixed pair keeps working on v1 pages but loses the v2 features.
- **A hub discovered at a new address needs confirmation** in Home Assistant before the integration retargets it.

### Changes

- Home Assistant actions, activity ids and HA-side steps follow the config the hub *accepted*: a config the hub
  rejects no longer changes what a key press does (the *Add-on link* sensor shows `config_state`: accepted, pending
  or rejected). Hub firmware: link loss is ordered with received frames and `remote_attached` is announced once per
  link-up, before the remote's `remote_info`.
- The config now comes from the Harmony Editor add-on (Supervisor discovery, long-poll, cached in
  `.storage`). The integration re-validates and compiles it itself.
- **Removed:** the `<config>/ha_harmony/<hub_id>.yaml` source and its loader. No fallback. A
  leftover file raises one repair issue and is never read or deleted: upload it in the add-on
  (import YAML, apply). See `docs/ha-config.md`.
- `reload_config` now pulls from the add-on; `send_ir` no longer accepts IR file references.
- New diagnostic sensor *Add-on link*; repair issues for rejected bundles and add-on/integration
  schema mismatch.
- **Home Assistant actions**: `{ha: {service, target, data, repeat}}` on keys, page buttons, tile taps and
  activity steps, run by the integration with the hub's current activity (allow-listed domains, no templates,
  validated in the add-on and again in the integration). `repeat: true` follows the remote's hold events.
- **Home Assistant devices** (`kind: ha`): power, inputs and commands as service calls, state from an entity,
  mixed with the hub's IR sequence (HA steps run once the hub reports the activity).
- **Tile taps** (`tap:`; tiles toggle / activate their entity by default) and tile state (on / off /
  unavailable / formatted value) for remotes with the `tile_state` capability.
- **Page format per remote**: the integration waits 1.5 s for `remote_info`, then pushes v1 or v2 pages and
  re-pushes on a format change or a new link-up; new *Page format* diagnostic sensor per remote.
- **Per-command IR `repeats`** (1..8) in config format 2 (hub firmware; older hubs ignore it and send one frame).
  `tools/stock_to_ha_config.py` emits it for stock's `min_repeats` on frame-only signals.
- **Audit fixes (integration)**: the add-on link sensor survives restarts; pages reach a remote only for a config
  the hub accepted and are not restarted by an unchanged reload; held repeats never queue on a slow service;
  `remote.send_command` honours `num_repeats`/`delay_secs`/`hold_secs`; failed user-started calls raise translated
  errors; an activity change during a disconnect still runs its HA steps; reconnects back off with jitter (reset
  only after 30 s up).
- **Performance**: entities are written only for what changed (a playing media_player no longer rewrites every
  entity); `remote_info` now carries `gen` and `pages` of the remote's stored set so a reconnect resends nothing the
  remote has (hub and remote firmware must be updated together); tile bursts are pipelined; first setup pulls
  the add-on once and never waits for the hub. *Behaviour change:* hold repeats are no longer events unless enabled
  in the integration's options, and the key event's `remote` attribute is gone (the entity is per remote).
- **Quality / security**: diagnostics, reconfigure, icons, translated errors; zeroconf no longer retargets a
  configured hub without confirmation; the add-on link confirm shows what is linked; per-domain on/off for HA
  devices (a closed cover is off); tiles follow an entity rename.
- The add-on's `GET /api/v1/health` reports `capabilities: {ha_actions, page_format}`.

### Migrating from the YAML file

`<config>/ha_harmony/<hub_id>.yaml` is no longer read, with no fallback. One repair issue per hub
points at a leftover file; it is never read, moved or deleted. Open the Harmony Editor add-on,
import the YAML (`POST /api/v1/config/import_yaml`), review, apply. The add-on cannot resolve
`flipper:`/`smartir:` file references: inline the codes first. Until you apply, the hub keeps the
config it already has.
