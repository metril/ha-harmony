# Changelog

## 0.4.9 (2026-11-23)

- Pressing the running activity again re-sends its power and input commands, so a TV turned off outside the hub comes back on. Update the hub.

## 0.4.8 (2026-11-23)

- Fix false loop-stall warnings that flooded the hub log. Update the hub.

## 0.4.7 (2026-11-23)

- Diagnostics for stalled remote commands: timestamped hub log lines, a loop-stall watchdog, and logs for dropped keys, the RF link and Home Assistant connection drops. Update the hub and the remote.

## 0.4.6 (2026-11-22)

- Now Playing shows cover art for media-session sources (e.g. SmartTube) from the session's `media_image_url` (HTTPS only, 10 s / 2 MiB bounded). Needs the Shield launcher's updated `Now playing` sensor; in SmartTube, turn off Player → "Disable playback notifications" so its session is visible.

## 0.4.5 (2026-11-22)

- Now Playing: new optional `media.session_entity` (the Shield launcher's `Now playing` MQTT sensor) gives a title, channel and position for apps the Android TV integration reports untitled, such as SmartTube. Plex keeps priority. Update the integration together with the add-on.
- Repo-wide `ruff format` (generated icon/theme tables excluded).

## 0.4.4 (2026-11-22)

- Hub: the boot hook now holds /dev/i2s for the daemon's whole life and hands it over (`--i2s-fd`), so a daemon crash, kill or OTA restart can no longer close the device and hard-freeze the hub. A missing or unopenable device counts as a failed boot. Update the hub (and re-run `tools/install_boot_hook.py`).
- Labels never end in an ellipsis: common abbreviations (Vol, Ch, Pg Up, Pg Dn, Fwd), two-line wrap, then icon-only. Icons are also inferred from the command or service name, and select tiles get the input glyph.
- Studio measures the label the remote actually shows (fixes a false "too wide" warning that shrank the preview).
- The hub announces a silently attaching remote reliably (test harness).

## 0.4.3 (2026-11-21)

- Now Playing appears when the activity starts while something is already playing (the remote clears the card on an activity change; the integration now sends the current state again, with the right position). Update the integration; no hub or remote update needed.

## 0.4.2 (2026-11-21)

- Now Playing works: the cover is read from the Plex entity in-process (no Home Assistant URL needed; a missing URL used to stop the feed before the title was sent), the feed sends what is already playing on start and when the remote reattaches, failed sends are retried, and the position after a reattach is correct. Send counters are in the integration diagnostics; the hub and remote log Now Playing and cover traffic.
- Device groups: devices that control the same box (for example the Shield over Bluetooth and its Home Assistant media player) share `group:` and show as one row and one page on the remote, with keys, inputs and app cards together; nothing is dropped, long pages split 1/2.
- Update the integration with the add-on. Flashing the remote and updating the hub only adds the Now Playing log lines.

## 0.4.1 (2026-11-21)

- A config can have up to 252 pages (was 32); ids 252-255 stay reserved for the remote's built-in screens. The integration's wait for the remote to confirm a config scales with the page count and restarts while delivery is progressing, so large configs are no longer re-sent mid-delivery.
- Studio: the inspector loads after the editor paints, keeping the first download under budget.
- Flash the remote, update the hub, update the integration with the add-on.

## 0.4.0 (2026-11-21)

- Remote redesign. Every page uses fixed sizes per widget type (44 px rows, 108x80 cards, 52 px keys, 64 px tiles), so nothing stretches; overflow pages show 1/2. Hand-written pages are converted automatically (opt out with `layout: grid`).
- The header back button returns to the previous page (8-deep history); Home clears it; Exit sends the activity's Exit command, or goes back when none is mapped.
- A symbol on every device, input and app: 18 new glyphs (device types, HDMI 1-4, AV, USB, cameras, garage), brand logos in brand colours, icons inferred for every device.
- Inputs are derived from input commands (HDMI 2 and 3 added to the example TV from the IR archive); every device gets a page; Inputs page per activity; Favorites is a real page; HA devices can take inputs from `source_list`.
- App buttons show which app is open on the Shield, with play/pause marks (activity `media.apps_entity`); a Now Playing card with cover art, title and progress from the Plex integration (`media.now_playing_entity`). Event-driven, no polling.
- The hub logs why its daemon died (signal and last actions) before it is respawned.
- Flash the remote, update the hub, update the integration with the add-on.

## 0.3.31 (2026-11-21)

- The hub logs the RF data-frame link-up; the remote logs each rejoin pulse with its trigger and latency; the hub installer removes the stock /cache/ota-update.log once. Flash the remote, update the hub, re-run `tools/install_boot_hook.py`. Add-on and integration unchanged in behaviour.

## 0.3.30 (2026-11-21)

- Fix: the hub no longer freezes after activity IR (/dev/i2s is held open), and the IR path is flushed after a failed send.
- The remote re-joins the RF link after a loss, on loss, key, touch, wake and resume.
- Hub logs are shipped to Home Assistant and the add-on log (rate-limited); the boot-hook log moved off flash to tmpfs (/var/volatile/ha-harmony-hook.log). Update the integration with the add-on (HACS), flash the remote, update the hub, and re-run `tools/install_boot_hook.py`.

## 0.3.29 (2026-11-21)

- Fix: the Devices starter page now takes the remote's Devices key (it had no favorites kind, so the key did nothing). Existing configs: set the device list page's kind to favorites. Add-on only.

## 0.3.28 (2026-11-21)

- Brand logos on buttons: Plex, YouTube, Netflix, Prime Video, Disney+, HBO Max, Hulu, Apple TV, Paramount+, Peacock, Crunchyroll, Twitch, Jellyfin, Emby, Kodi, Plexamp, Spotify, Apple Music, Tidal, SoundCloud, Steam, Xbox, PlayStation, Nintendo Switch, Nvidia, Sonos and Chromecast (icons 65-91). Name a button after one and the studio suggests the logo; picking it fills the button in the brand colour, and it shows the logo alone. The remote needs the new firmware to draw them, and the integration must be updated with the add-on (HACS) before Apply.

## 0.3.27 (2026-11-21)

- Fix: Apply failed in Home Assistant with "unknown icon" for the new icons when the integration was older than the add-on; a hub rejection now stays visible as a banner in the editor. Names keep brands ("YouTube", "iPhone", "PlayStation"). Update the integration together with the add-on. No hub or remote change.

## 0.3.26 (2026-11-21)

- Editor: the palette, page strip and action picker load on demand, keeping the first download under budget. No remote change.

## 0.3.25 (2026-11-21)

- Elite redesign: new icon fonts (more glyphs, icon keywords pick them), 1-2 character pills render in b20, pixel-budget text fit and LVGL-style clipping in the studio preview, starter pages, BT pairing fixes. Needs the matching remote firmware.

## 0.3.24 (2026-11-21)

- Alignment: every button is centred by default, rows included (centred in the space left of the chevron). Per-widget `align: left | center | right`; the editor's Align control is a three-icon toggle, disabled where alignment has no effect (small buttons, tiles, icon-only).
- The remote and the editor preview now draw buttons the same way at every size (wide buttons were silently left-aligned on the remote). Needs the matching remote firmware.

## 0.3.23 (2026-11-20)

- Elite strip keys fixed: ACTIVITIES now opens the `kind: home` page and DEVICES the `kind: favorites` page (the driver's key codes were swapped). Needs the matching remote firmware.
- New per-widget `align: left | center` (editor: Align next to Shape). Centred rows keep the trailing chevron at the right edge. Needs the matching remote firmware.
- Editor preview draws the trailing chevron on nav rows, matching the remote.

## 0.3.22 (2026-11-20)

A hub request that gets no answer now reports "request timed out after 10 s" instead of an empty "hub is unreachable" reason.

## 0.3.21 (2026-11-20)

Off always sends power-off to HA devices, even when their state still reads off.

## 0.3.20 (2026-11-20)

Release fix; no functional change from 0.3.19. The integration 0.3.19 release was never published because CI failed (lint and the add-on image smoke test); this version carries the same changes.

## 0.3.19 (2026-11-20)

Editor, integration and remote firmware; no hub firmware change. Update the add-on, then the integration, then flash the remote.

- Config library: keep any number of named configs, each with its own draft. Save Revision takes a numbered snapshot with an optional note.
- Revisions view (replaces History): restore, duplicate, compare, export or apply any revision to any hub. The header shows what each hub is running.
- Switching configs happens in place. Import YAML can create a new config. A banner warns when the open config is deleted elsewhere.
- Existing per-hub configs move into the library automatically on first start, keeping their history and draft.
- Remote: buttons that open a page are rows with the chevron on the right.
- Remote: tapping the running activity opens its page again.
- Power Off now turns off every HA device (e.g. an NVIDIA Shield) even if the activity being left did not list it; a media player in standby counts as on.
- `examples/living-room.yaml`: the Devices key lists devices; Lights, Scenes and Blinds move to Home.

## 0.3.18 (2026-11-20)

Editor and integration; no hub firmware change. Update the add-on, then the integration.

- Direction keys read "Up", "Down", "Left", "Right" instead of "Direction Up".
- Inherited keys show their marker dot inside the key, with a tooltip and a legend entry.
- Rules can now call `input_select` (`select_option`, `select_next`, `select_previous` only).
- Cover tiles show the current position.
- New example config: `examples/living-room.yaml`.

## 0.3.17 (2026-11-20)

Editor only; no hub firmware change. Update the add-on, then the integration.

- Readable names everywhere in Title Case: "Power On", "Channel Up", "Previous Channel", "Input A1", "1A", "HDMI 1", "Play/Pause", "Home Control +". Codes and acronyms stay whole and upper case; names you typed are shown in Title Case but stored as you wrote them.
- Buttons, tabs and headings in Title Case ("Review & Apply", "Free Keys", "Default Layer"); IR protocols in upper case ("NEC 9/2").
- Keys and commands follow one order, with pairs side by side: Up | Down, Left | Right, Play | Pause, Rewind | Fast Forward, Volume Up | Volume Down, Channel Up | Channel Down, Red | Green, Yellow | Blue, keypad order for numbers. A pair never splits or gets cut off.
- The "High Contrast" theme and the remote's generated "Power Off" / "All Off" labels use Title Case.

## 0.3.16 (2026-11-20)

**Upgrade order: hub firmware first, then the add-on, then the integration, all in the same sitting.** Older versions are no longer supported alongside the new ones: an integration refuses a hub whose firmware does not announce TCP keepalive, and an add-on without the new channel shows a repair issue.

- No more polling. The integration and the add-on talk over one push connection (the add-on's `channel`), and the editor gets live updates over its own connection. Keeping the links alive is left to the operating system's TCP keepalive, so there are no ping timers and nothing wakes up every few seconds.
- Bluetooth LE fixes on the hub: Service Changed is sent once per pairing, the hub offers the standard Generic Attribute service, and remote keys wait until the host is ready to receive them.
- Fixed the hub showing as offline in Home Assistant ("Connect call failed") when the hub had not answered Home Assistant's ARP request: the hub now refreshes that entry itself, when a peer connects or goes quiet, instead of on a timer.
- If the add-on says it cannot talk to this integration version, the link shows "integration outdated" and stays that way until the integration is reloaded.
- A device that has just paired is added to the draft straight away, so it is not lost if you take a while to finish the form.

- Hub: the stock `bluetoothd` is now really stopped before the Bluetooth helper starts: it was never found because the hub's kernel has no `/proc/<pid>/comm` (the name now comes from `stat`, else `cmdline`). `hciattach` is left alone. The log says stopped / not found / ignored SIGKILL. The LE "pairing start" line shows the connection's peer address instead of zeros.

- Hub: Bluetooth LE now uses its own random static address (made once, kept in `le_addr` next to the bond file) instead of the controller's public address, so a host that already knows the hub over Classic cannot pair "LE" onto BR/EDR. LE bonds made before this change must be paired again. LE Secure Connections (Just Works, software P-256 from vendored micro-ecc) with legacy fallback. The advertising flags follow the pairing window even while advertising is already on.
- Hub: the Bluetooth helper logs Classic connection requests/completions, LE connection complete/failed (role, peer, status), every LE pairing result with its reason, link disconnections with the reason, and the status of the LE advertising commands. Optional HCI packet trace to a size-capped file in RAM: create `bt/hci_trace` in the data directory and restart the helper (see `docs/proto/bluetooth.md`).

- Hub: a Bluetooth pairing window now honours `transport`. `le` is LE-only (no Classic inquiry or page scan for new bonds, Classic PIN/SSP refused; page scan stays on only while a Classic bond exists so that host can reconnect), `classic` does not advertise LE, `any` keeps both; the previous scan and advertising state returns when the window closes. Before, the helper ignored `transport` and a host could bond over Classic in an `le` window.
- Hub: the Bluetooth helper logs, at the default level, the start, any refusal and every failure of a pairing with transport, peer address and the stack's reason (SMP reason, HCI or authentication status), one line each, no key material. The repeated "HID ... refused by the stack (status 0x57): waiting to send" lines are now debug level (`HH_BT_DEBUG`); behaviour unchanged.

## 0.3.15 (2026-11-20)

- Hub: ARP keepalive for peers on the hub's own segment. After a reboot a peer such as Home Assistant could fail to ARP for the hub (broadcast ARP missed on the hub's Wi-Fi) and its connection failed at once. The hub now remembers the IPv4 address of each peer that authenticated (newest 64, private addresses only, `peers` file next to `state.json`, atomic, written only when a new address appears) and, at startup and every 30 s, sends one UDP byte to port 9 of each remembered peer that has no live authenticated connection; the kernel ARPs for the peer, which learns the hub's MAC. Stripped hub grows by about 3 KiB. See `docs/runbooks/phase4-persistence.md`.
- Add-on: the read-only registry client may now also send `config/label_registry/list` (optional), so the Home Assistant tab can group and filter by label. Nothing can be written.
- Editor: the filter buttons no longer switch between icon + word and icons when a group is opened (the list scrollbar no longer changes the width they are measured against), and changing Group by no longer moves the box into the filter row.

## 0.3.14 (2026-11-19)

- Editor: Bluetooth fixes. A pairing run only accepts a result that differs from the one held when its window opened, with a newer `gen`, and (when ok) an address the hub lists, so an earlier pairing is never reported for a timed-out run; the hub now clears the last pairing result when a window opens. Status polling keeps one timer and drops overtaken responses; a retry id is bound to its action; leaving the key learner or the pairing wizard mid-request turns listening off and closes the window; a learned press on a page the hub cannot map is explained instead of assigned; Bluetooth commands (default set included) count against the command limit like the backend counts them; the palette shows a default-set device's commands; the countdown is no longer read out every second; remote renames refuse duplicates; the phone key map takes two lines per key. The bundle budget for all JS is 200 KiB (lazy chunks), the entry stays at 90 KiB.


- Editor: Bluetooth. New Bluetooth section (lazy loaded): the hub's Bluetooth state, paired devices and remotes with connection, link type and battery, a test-fire for a device, Forget with an in-page confirmation, and clear messages when the hub has no Bluetooth, is offline, or Home Assistant has not reported it. "Add Bluetooth device" (also a tile in Add a device) opens a pairing window with a live countdown and plain instructions, then names the new host and picks its commands (default set preselected, searchable built-ins, your own `HID:` codes) and saves a `kind: bt` device; hosts the hub holds but the config does not list get "Add to config". "Add Bluetooth remote" lists scan candidates with signal bars, pairs the one you pick and opens a key map (defaults filled in) with a "press a button on the remote" learn mode; saved as a `bt_remotes` entry and editable later. Bluetooth commands work in the palette, key assignment, activities and tiles; a Bluetooth device page has no IR parts. Every error is a sentence with a next step; retries after a timeout reuse the command id. Add-on: new read-only `GET bt/codes` (built-in command names, default command set, default remote key map, key names).
- Integration: Bluetooth support, only on a hub whose `hello_ack` carries `bt`. The coordinator keeps the latest `bt_state` (a push with an older `gen` is ignored, reset on reconnect, every field re-checked and capped) and has `bt_pair`, `bt_forget`, `bt_key`, `bt_listen` with the hub's error codes turned into sentences. New `Bluetooth` diagnostic sensor (off/starting/ready/no_adapter/disabled; `le`, `limits`, `window`, `discoverable`) and a connectivity `binary_sensor` for each Bluetooth device and remote in the loaded config (keyed by address, so a hub rename migrates them; remote `battery` as an attribute; bonds that are not in the config get none). Services `ha_harmony.bt_pair`, `bt_stop_pairing`, `bt_forget` (by address), `bt_listen`; the `ha_harmony_bt_remote_key` event fires only while a listen lease or the editor asked for keys (never by default). Diagnostics include `bt_state` and `hello_ack.bt` with addresses redacted. See `docs/bluetooth.md`.
- Add-on: Bluetooth relay for the editor, no hub access: `GET bt/status`, `POST bt/pair`, `bt/pair/stop`, `bt/pick`, `bt/forget`, `bt/key`, `bt/listen`, `GET bt/events` (ingress) and the integration's `POST bundle/bt` (bearer) exchange. Commands wait in a bounded per-hub queue, are collected by the integration (about every 5 s, every second while a pairing window, a key listen or the editor's Bluetooth view is active), re-validated there against the hub's limits (addresses, slots, pages 7 or 12, usage range, seconds up to 300) and answered with the hub's result. See `docs/addon-api.md` "Bluetooth relay".
- Tests: scripted-hub tests for the integration (no Bluetooth hub, gen ordering, reconnect reset, entities added and removed with the config, rename migration, services, events, diagnostics, relay round trip, re-validation of hostile add-on commands), add-on relay route tests, shared validator tests.

- Hub: the Bluetooth link runs and supervises the helper (`hub/bt_link.c`, builds with the helper only; the stub stays for `HH_BT=OFF`). It starts `hub bt-helper` on a socketpair only while the config has `bt_hosts` / `bt_remotes` or a pairing window is open, stops the stock `bluetoothd` once before the first start of a boot (nothing on disk changes), pings every 5 s, replaces a helper that is silent for 15 s or sends a malformed frame (backoff 5/30/120 s, at most 5 restarts a boot, then `disabled`), and treats every helper byte as untrusted. A helper death clears the cached state, releases a held host key, reports connected remotes lost to the key mapping, and never counts as a boot failure or touches IR/RF. Config bt slots are bound to the helper's bonds by address; keys, activity-start connects, remote keys and pairing run through it (`hw_sinks` `bt_key`). New daemon options `--bt-helper EXE` and `--bt-dir DIR` (tests; the default data dir is next to the state file); `--sim` without `--bt-helper` behaves as a hub without Bluetooth. `bt_state` gains optional `window: "host"|"remote"` while a pairing window is open, and host/remote entries gain optional `configured` (bonded in the hub and in the config; unconfigured entries are still listed so the editor can offer them). See `docs/proto/bluetooth.md`, `docs/runbooks/bluetooth-bench.md` (the bench gate).
- Firmware size: hub cross builds are size-first by default (MinSizeRel `-Os`, `-ffunction-sections -fdata-sections`, `-Wl,--gc-sections` on every hub link). The build strips `hub` itself (symbols kept in `hub.debug`), so what `deploy_hub.py push` copies and `ota_sign` signs is the stripped binary (`push` refuses an unstripped one). `tools/check_hub_size.py` fails over 600 KiB or when 2x image + slack does not fit the ~2.4 MB /data and checks the build-id tag survives; CI runs it on the mips build. Stripped hub: 241 KiB without Bluetooth, 386 KiB Classic only, 478 KiB full. The build-id tag is now referenced from `hp_build_id` so the linker keeps it. Bluetooth link: at most 32 helper key frames handled per loop pass, a key up is never dropped for lack of room (and a dropped down drops its up), a pair request waits and retries when the control queue is full, and an activity connects all its bt hosts.
- Tests: a fake helper process drives the supervisor (spawn, silence, backoff and cap, malformed frames, flood, slow reader, slot binding, key dispatch, held-key release, pairing window), an end-to-end pairing flow through the daemon's websocket in `--sim`, and the activity-switch / power-off release of a held Bluetooth key.

- Protocol: pairing a Bluetooth LE remote no longer bonds the first advertiser. `bt_state` gains optional `candidates` (`[{addr, name?, rssi}]`, max 16, only while a remote-mode window is open) and `bt_pair` gains optional `addr`: `mode: remote` without it scans and lists, with it bonds that candidate (`no_candidate` / `bad_addr` errors). `hub/bt_link.h` adds the candidate list and a non-blocking `bt_link_pair_remote`. See `docs/proto/bluetooth.md`.
- Bluetooth HID in the config, compiler and hub mapping (no transport yet): `kind: bt` devices (up to 255; `address`, `transport: auto|classic|le`, `commands` of built-in names or `HID:<page>:<usage>` on the keyboard and consumer pages) are used like IR devices and compile to `{"type":"bt","page","usage","host"}` actions plus a `bt_hosts` table. `bt_remotes` (up to 32) map the usages of physical Bluetooth remotes to the Elite key names and compile to a `bt_remotes` table counted in the hub memory budget. The hub parses both, the BT sink takes `(host, page, usage, down)`, a physical key press sends key down and its release key up, and `hp_map_bt_remote_key` feeds a remote's usages into the normal key path. See `docs/ha-config.md` "Bluetooth devices and remotes".
- Protocol (groundwork, no visible change): Bluetooth HID hub messages `bt_pair` (35), `bt_state` (36), `bt_forget` (37), `bt_key` (38), `bt_remote_key` (39), `bt_listen` (40) and an optional `hello_ack.bt` ({hosts_max, conn_max, remotes_max, pages, le}), present only on builds with Bluetooth. Hosts are classic or LE, up to the hub's `hosts_max`; bonded LE remotes are input only. The hub handlers call a non-blocking `hub/bt_link.h` interface; this build ships a stub (state `off`, no `bt`), so nothing is served yet. Codegen now sizes the handler table by the highest message code. See `docs/proto/bluetooth.md`.
- Add-on editor: the palette's tab is called **Home Assistant** (it was "Home", which clashes with the remote's Home key and page) and the tab strip shows icons with the selected tab's full name. It is rebuilt for installs with thousands of entities: groups (Area under its floor, Type, Device, Integration, Label) are collapsed by default and only open groups draw rows; search matches name, entity id, area, device, integration and labels and opens the groups it finds; type chips with counts and Area/Label/Integration filters combine and show an "n filters" state; Favourites, Used in this config and Recently used sit at the top; arrow-key navigation, a live result count, a loading skeleton, a Retry that says why Home Assistant cannot be read, and a notice when only type grouping is available. The action picker and the add-device wizard use the same list.
- Add-on: reads Home Assistant's area, device, entity and floor registries over the Supervisor's WebSocket proxy (read only, an allow-list of four `*/list` commands enforced in code, cached 60 s) so entities carry their area, device and integration; disabled and hidden entities and, unless asked for, diagnostic and config ones are left out. Without the registries the list falls back to the REST data (type grouping). `GET /api/v1/ha/entities` gains `area`, `diagnostic`, a richer row, `areas`, `registries` and an `ETag`.

## 0.3.13 (2026-11-19)

- Add-on editor: the palette lists commands in a uniform grid (equal-height rows, ellipsis and tooltip for long names); a selected key or widget clears on a second click, a click on the remote body, the canvas background or Escape; key labels, the Vol and Ch legends and glyphs keep AA contrast in every key state; the four colour keys are wide dark keys with a coloured ring, and the light and socket keys show a spotlight and a US outlet.

## 0.3.12 (2026-11-19)

- Add-on: the editor no longer reports "Home Assistant cannot be read" on a large install. The add-on read only the first network chunk of the entity list, so any answer over one chunk looked like broken JSON; it now reads the whole answer (still capped at 8 MB, 20 s total). When Home Assistant cannot be read the add-on logs one warning with the path and cause (not repeated while the cause stays the same, plus a line on recovery), and the 503 carries a short `reason`.

## 0.3.11 (2026-11-19)

- The integration never marks a custom-theme set as pushed until the remote reports it complete: after the one ack-timeout re-send it stays pending and re-pushes with a bounded exponential backoff (15 s doubling to 5 min, cancelled on unload, per remote), and it re-pushes when the remote reports an empty or partial store while we thought it held the set (a reboot the hub never saw, a theme the hub gave up on). A hub that refuses a theme or page for a transient reason is retried the same way instead of sending the pages without their theme.
- Hub: a full link queue holds a `theme_push` or `page_push` instead of failing it; tile messages wait behind the theme and their page (page first, latest value per tile), also for pages held for a failed delivery; a queued object that finds nobody home is reported to its owner and a new link starts with nothing counted in flight.
- The add-on's editor shows what a linked remote cannot display: the integration sends the remotes' colour tier and current notices with its polls (`X-Hub-Remotes`) and `config/status` returns `remotes` and `theme_notices`.
- Custom themes reach the remote: the new message `theme_push` (code 28, `{remote, gen, data}`, hub and RF) carries a user theme's 99 bytes. The integration sends it before the pages of its generation to remotes that announce caps bit 3 (the remote now does), waits for the remote to report the complete set and re-sends once if it does not; the hub queues it like a page (superseded, dropped by a newer generation, resent while attached) and delivers it ahead of the set's pages. The remote keeps one theme slot in RAM; a page with theme id 7 counts as received (display and `remote_info` gen/pages) only while the slot is of its generation, so a page never shows with a stale or missing theme. Older hub firmware (`unknown_type`), older remotes and an integration without it keep the base-preset behaviour. The remote page format sensor and diagnostics report the theme (name, id, sent, acked). See `docs/proto/themes-transport.md`, `docs/runbooks/theme-transport-bench.md`.
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
