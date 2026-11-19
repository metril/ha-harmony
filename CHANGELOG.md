# Changelog

## Unreleased

- Bluetooth HID in the config, compiler and hub mapping (no transport yet): `kind: bt` devices (up to 255; `address`, `transport: auto|classic|le`, `commands` of built-in names or `HID:<page>:<usage>` on the keyboard and consumer pages) are used like IR devices and compile to `{"type":"bt","page","usage","host"}` actions plus a `bt_hosts` table. `bt_remotes` (up to 32) map the usages of physical Bluetooth remotes to the Elite key names and compile to a `bt_remotes` table counted in the hub memory budget. The hub parses both, the BT sink takes `(host, page, usage, down)`, a physical key press sends key down and its release key up, and `hp_map_bt_remote_key` feeds a remote's usages into the normal key path. See `docs/ha-config.md` "Bluetooth devices and remotes".
- Protocol (groundwork, no visible change): Bluetooth HID hub messages `bt_pair` (35), `bt_state` (36), `bt_forget` (37), `bt_key` (38), `bt_remote_key` (39), `bt_listen` (40) and an optional `hello_ack.bt` ({hosts_max, conn_max, remotes_max, pages, le}), present only on builds with Bluetooth. Hosts are classic or LE, up to the hub's `hosts_max`; bonded LE remotes are input only. The hub handlers call a non-blocking `hub/bt_link.h` interface; this build ships a stub (state `off`, no `bt`), so nothing is served yet. Codegen now sizes the handler table by the highest message code. See `docs/proto/bluetooth.md`.
- Editor: widget colours and themes. The page preview draws from the shared theme tables with the remote's own colour rules (themed surfaces, ink and pressed shades, automatic outline, active border, tile states, coloured labels), checked against the firmware's reference screens. The widget inspector has a Colour row (Default, the theme's 12 palette colours with their text, Custom with hex, picker, recent colours, a live contrast read-out, "Use nearest readable" and "Add to my theme's palette"; tiles also get "When on"), and shows the colour bytes and the compiler's over-budget message. New Themes view: built-in and your own themes as thumbnails, Use / Duplicate / Edit / Export / Import, a token editor with plain-language labels, contrast problems with Fix and Fix all, and a live preview. Theme and colour edits are ordinary config edits (undo, autosave, YAML). Warnings from the add-on show as a chip instead of errors; a notice appears when the remote's firmware lacks colours.
- Remote renders widget colours and page themes: a coloured button or tile uses its palette or free RGB565 fill with the derived ink for label and icon, the derived pressed shade and an automatic 1 px outline when the fill is too close to the ground; the active highlight on a coloured button is a 3 px accent border; tiles keep the theme's off, stale and unavailable colours (an ON tile uses its own colour); coloured labels fall back to the text colour below 4.5:1. Each v2 page applies its theme preset on show (id 7 uses the custom slot, `ra_ui_set_custom_theme`, else the default). The remote announces caps bit 2 (theme_color). Midnight's card surfaces are lifted so they read against the black ground.
- Themes groundwork (no visible change yet): `proto/themes.yaml` is the single source for the remote UI themes (Cards plus midnight, daylight and high-contrast presets, a named 12-colour palette each); `tools/gen_themes.py` derives each colour's ink and pressed shade, enforces WCAG contrast on every text/surface pair, and generates the Python, C and editor tables. Integer-only ink/pressed/RGB565 maths, a 99-byte wire theme, user-theme resolution and contrast fixes are pinned across Python, TypeScript and C by shared vectors (`tests/vectors/themes.json`).
- Page format v2 learns widget colours (palette index or RGB565, plus a tile "on" colour) and a theme preset id in the page header; the Python and C codecs, vectors and fuzz tests cover them. Pages without colours encode to the same bytes as before. Not sent to remotes yet: older remotes reject such pages, so a capability gate comes first.
- Remote: the UI theme is now a runtime value (built-in presets or a 99-byte wire theme) applied in place with a full repaint, no allocation and no widget re-creation; no colour literals remain outside the theme tables. Default look is pixel-identical. Bench aid: `HARMONY_THEME=<preset key>` (default, midnight, daylight, high_contrast) at remote start.
- Config and compiler learn colours and themes: widgets take `color` (palette name or `#RRGGBB`) and tiles `on_color`; `theme:` picks a preset or one of your `themes:` (with `extends`). Located errors for unknown colours/themes/tokens, warnings for contrast, a byte-cost breakdown when a page is over budget, the theme in the page generation so an edit re-sends pages, and per-remote degrading by `caps` bit 2 (colours) / bit 3 (custom) with notices. Sending a custom theme to a remote is not wired yet. See `docs/ha-config.md` "Colours and themes".
- Rename a hub from Home Assistant: *Reconfigure* now has **Hub ID** and **Name** fields. The hub id was `ha-harmony-<hostname>` (Logitech's codename) and only changeable over SSH. The new hub message `set_hub_id` (code 26, reply `hub_id_ack` 27; needs a firmware update) validates and saves the id and an optional display name atomically in the daemon's own state directory (`hub_id`, `hub_name` next to `state.json`) and applies them at once (hello_ack, mDNS re-announced under the new name). Precedence: `hub_id` in hub.conf (pinned; refused with `id_pinned`), then the saved id, then the hostname default; hub.conf is never rewritten. `hello_ack` gains `name`, `hw_id` (MAC-derived), `rename`, `id_pinned`, and the mDNS TXT gains `hw=`.
- Hub ids: letters, digits, `_` and `-` (no dot: not valid in an mDNS label);  the daemon never exits over an id (a bad pin is logged and ignored, the boot hook starts it unpinned; the hostname-derived default stays exactly what older firmware used whenever that id was usable, is made valid only when it was not, and is saved so it never changes). Ids with a dot already in use keep working and are announced with `-`. A rename needs an add-on that answers the rename headers (else `addon_too_old`) and a hub that reports its hardware id; a hub renamed on the device whose add-on already holds a store under the new id raises a repair issue instead of failing setup.
- The integration moves everything keyed by the id in one transaction (entry unique id and title, device identifiers, entity unique ids through the entity registry helper, the cached config, the add-on's store) and undoes everything, including the hub, if any step fails. Entity ids are not renamed, so history, dashboards and automations keep working. A hub renamed on the device is recognised by its hardware id (discovery and setup). See `docs/ha-config.md` "Naming your hub", `docs/proto/hub-identity.md`.
- Add-on: a poll that names the previous id and/or hardware id (`X-Hub-Prev`, `X-Hub-Hw`, `X-Hub-Name` headers) moves that hub's store (draft, applied, history) to the new id, never overwriting an existing store; the editor header shows the hub name with the id as secondary text and the hub switcher uses names. See `docs/addon-api.md`.

## 0.3.10 (2026-11-18)

- Editor: History shows one status on the newest revision (On the remote, Sending..., Not sent, or Rejected by hub with a See why reason) instead of three badges; older revisions show only Compare and Restore. One shared status chip now draws the Valid chip and the History status, and the apply dialog, the pending banner (no more raw hash) and the toasts use the same wording: saved, sending, on the remote, rejected.
- Editor: the "Config size" meter's bar now uses the same net figures as its label (the hub's fixed load overhead left out), so 18 KiB of 6.5 MiB no longer draws about 19% full.

## 0.3.9 (2026-11-18)

- Hub items below are in the repo and need a firmware update on the hub; the integration and editor fall back to the old fixed limits for older firmware.
- Hub: no more 256-command (or 64-input, 16384-word, 128 KiB) limit. Those were array sizes of one statically sized map, not wire limits. Commands, inputs, the IR word pool and key maps are allocated per load, exactly as large as the config, and the config is streamed (one command parsed and freed at a time, never one big tree; the text goes to a new cache file as it arrives). The one size limit is a memory budget (`config_budget_kib` in hub.conf, default 8 MiB, capped at 20% of MemFree+Buffers+Cached at daemon start, applied to new pushes only: the cache already accepted always loads at boot) checked up front with `config needs ~N KiB of hub memory, the hub allows M KiB`, plus a flash check (`config needs ~N KiB of flash, the hub has M KiB free`, with 2 x the hub binary + 256 KiB kept free for OTA staging). `config_chunk` is message code 25. A stale `cache.json.new` is removed at start, an abandoned transfer is dropped (connection closed, or 60 s idle), duplicate top-level `keys`/`activities`/`format`/`devices` are rejected, and `config_budget_kib` below 256 is ignored (hook) or clamped (daemon) instead of failing the boot. A failed load (bad config, over budget, out of memory, full flash) leaves the old map and the old cache file untouched. About 56,000 protocol-coded, 34,000 Pronto-string or 26,000 raw-array commands fit in RAM; the 5 MiB flash partition limits learned codes to roughly 8,000 Pronto strings or 4,000 raw arrays. Remaining per-item bounds: 255 devices, 16 activities, 256 keys per map, 16 steps, 8 devices per activity.
- Hub: `hello_ack.limits` reports the budget and bounds; new `config_chunk` message sends configs over the 64 KiB frame size in pieces. The integration and add-on use the hub's reported limits and fall back to the old fixed ones for older firmware (no chunks). The editor shows "Config size X of Y KiB on this hub" for a current hub.
- Integration (behaviour change): command buttons are created disabled, and a device with more than 200 commands gets none unless the new option `all_command_buttons` is set. Existing buttons keep working and stay enabled; every command stays reachable with `remote.send_command`. See `docs/ha-config.md`.
- Editor: over budget, the "Config size" meter says "over, remove commands" next to the precise message; a hub with no flash left is reported as "the hub's flash is full" instead of skipping the check (`hello_ack.limits.flash_low`, with `flash_free` 0; a bare 0 still means unknown).
- Hub: a cached config whose text is above 4 x the memory budget is refused at boot unparsed (logged, boots with no config) rather than attempted.
- Add-on: request body cap 4 MiB, 250k JSON values per config, `ir/parse` 4096 actions / 1 MiB; the integration compiles a pulled bundle through the fast dict path (was ~0.5 ms per command).

## 0.3.8 (2026-11-18)

- Editor: assigned keys are outlined in teal instead of filled (filled only for the selected key); the D-pad is one plain rounded diamond and Vol, Ch/Pg and the +/- key are plain keys, their halves showing only on hover, focus, selection or when assigned.
- Editor: the Elite drawing is now shaped like the real remote: a rounded-diamond D-pad with OK, single tall Vol and Ch/Pg keys with two hit areas, notched Exit/Menu/Mute/Back, one play/pause key, one DVR/Guide/Info bar, outlined ACTIVITIES/DEVICES, ringed colour keys, a single +/- key and printed icons, with a dome top and a matte grip.
- Editor: the Elite drawing's proportions now follow the photo (taller body, Off lower and inset, screen centred, keys and the D-pad ring placed from one table of body fractions, empty rounded grip at the bottom).
- CI: the mips qemu test build no longer fails on a `-Werror=maybe-uninitialized` in `test_hub_ota.c` (gcc 13); the boot-hook selftest for an update applied inside the health window no longer races the (tiny) test window. Tests only, no firmware change.

## 0.3.7 (2026-11-18)

- Fix: integration 0.3.6 and older refuse to connect to hub firmware that reports extra handshake fields; receivers now ignore unknown fields (hub and integration alike; unknown message types from a newer peer are ignored too; what each side sends is still strictly validated).
- Fix: the studio's layer pills are no longer squeezed to a few pixels at narrow widths or with wider fallback fonts; the pill row measures its room and moves the rest into the "More" menu.
- Hub and remote can learn their RF slot from the radio (link-up report, or the first data frame when the link was already up; the learned slot is saved in `rf_slot` next to the hub state file / in `/data/app` and used as a provisional start slot). Opt-in until the bench gate passes: `install_boot_hook.py install --learn-slot`, `deploy_remote.py install-hook --learn-slot`; `--rf-slot` / `--slot` still pass a fixed slot by default and override learning. Nothing is sent to a guessed slot before it is known. The remote's `--slot 0` is now rejected (omit the flag to learn).
- Hub OTA: signed firmware upload over the hub websocket (`ota_begin`/`ota_chunk`/`ota_end`/`ota_abort`/`ota_status`; resumable by offset, 4 KiB chunks, one upload at a time, ENOSPC-safe), verified and applied with `ota_apply_staged`, then the daemon exits with status 42 and the boot hook relaunches the new build. A freshly applied build that dies inside its health window, or never reaches the health mark before a reboot, is rolled back once to `hub.prev` and reported (`ota-rollback`) as a repair issue in Home Assistant. New service `ha_harmony.install_firmware` and a diagnostic "Firmware update" sensor. A hub with the public dev key accepts updates only with `ota_allow_dev=1` (hub.conf) and the service's `allow_dev_key`; updates are refused until the boot health mark and the downgrade floor includes the running build (`-DHP_BUILD_ID`). Message ids 30-34 are reserved for the remote's RF transport. See `docs/runbooks/ota-update.md`.

## 0.3.6 (2026-11-18)

- Editor: History page simplified (Compare and Restore actions per entry) and the hub indicator sits in the header.
- Fix: the "upload your YAML in the add-on" repair clears itself once the hub accepts a config applied in the add-on, is not raised again while an add-on config is cached (a log line says the file is safe to delete), and can be dismissed. The file is never touched.
- Hub boot hook: bounded daemon respawn (`respawn_max`, default 3, 0 = off) with a heartbeat wedge cover, an `rf_debug` conf key and uptime-stamped logs; the boot-strike counter is never touched by a respawn.
- Remote: opt-in suspend to RAM after idle (`HARMONY_SUSPEND=battery|always`) with an hourly timed wake (msp430 `wakeup`, 3600 s) so the battery guard still runs, resume re-link and sleep-gap accounting. Touch does not wake it; buttons and motion do. See `docs/runbooks/remote-suspend-soak.md`.
- OTA: signed image container with Ed25519 verification (`ota_sign` tool, staged apply helper on the hub). Builds that bake in the public dev key need `-DHP_ALLOW_DEV_OTA_KEY=ON`.
- Tools: `deploy_remote.py disable|enable` (kill switch) and a longer `reboot` wait; both deploy tools need Python 3.14 (`uv run`).
- Tests: the on-hub boot-hook selftest synchronises on events instead of sleeps (no more intermittent failures on loaded runners).

## 0.3.5 (2026-11-18)

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
