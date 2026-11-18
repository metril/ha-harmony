# Changelog

## Unreleased

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

## 0.3.0 (integration)

- The config now comes from the Harmony Editor add-on (Supervisor discovery, long-poll, cached in
  `.storage`). The integration re-validates and compiles it itself.
- **Removed:** the `<config>/ha_harmony/<hub_id>.yaml` source and its loader. No fallback. A
  leftover file raises one repair issue and is never read or deleted: upload it in the add-on
  (import YAML, apply). See `docs/ha-config.md`.
- `reload_config` now pulls from the add-on; `send_ir` no longer accepts IR file references.
- New diagnostic sensor *Add-on link*; repair issues for rejected bundles and add-on/integration
  schema mismatch.

### Migrating from the YAML file

`<config>/ha_harmony/<hub_id>.yaml` is no longer read, with no fallback. One repair issue per hub
points at a leftover file; it is never read, moved or deleted. Open the Harmony Editor add-on,
import the YAML (`POST /api/v1/config/import_yaml`), review, apply. The add-on cannot resolve
`flipper:`/`smartir:` file references: inline the codes first. Until you apply, the hub keeps the
config it already has.
