# Changelog

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

**Upgrade order: hub firmware first, then this add-on, then the integration, all in the same sitting.**

- No more polling: the add-on talks to Home Assistant over one push connection and to the editor over another. The editor refreshes the instant something changes (applied configs, Bluetooth state, remote keys) and no page timer asks again and again.
- Bluetooth labels now read "Automatic", "Bluetooth BR/EDR" and "Bluetooth LE".
- Pairing: a device that pairs goes straight to "Name and commands" and is saved in the draft at once. Applying a config no longer restarts the pairing view.
- Remote editor: readable names for keys and devices, tidier grids, and a choice of icon per device.
- The add-on is no longer marked experimental in the add-on store.
- Fixed the hub showing as offline: the hub now refreshes Home Assistant's network entry itself (firmware), and commands waiting on a hub that disconnects report "hub offline" immediately.

## 0.3.15 (2026-11-20)

- The Home Assistant tab can now group and filter by label: the add-on also reads Home Assistant's label list (read-only, like the other registries).
- Fixed the filter buttons switching to icons only when a group was expanded, and the list overlapping the filter row after changing Group by.

## 0.3.14 (2026-11-19)

- The Home Assistant tab is rebuilt for large installs: it is called "Home Assistant", everything starts collapsed with counts, and you can search by name, entity id, area, device or integration, filter by type, area and integration, and group by area (with floors), type, device or integration. Favourites, "Used in this config" and "Recently used" are pinned at the top. Hidden and disabled entities are left out; diagnostic ones are behind an option. The same browser is used when picking an entity for an action and when adding a device.
- To group by area the add-on now also reads Home Assistant's area, floor, device and entity lists over a second read-only connection that can send only those four requests. If it cannot, the tab still works, grouped by type.
- Bluetooth: a new Bluetooth page to pair devices (Shield, Apple TV, Fire TV…) and Bluetooth LE remotes, map a remote's buttons and test commands. It needs hub firmware with Bluetooth, which is not released yet; until then the page says so.
- The Home Assistant tab and the palette hold up at any font or width: the filter buttons show an icon and word, or four icons with tooltips when they do not fit; the group-by row stays on one line; the selected tab name is shown whole or not at all; long names wrap or have a tooltip instead of being cut; nothing scrolls sideways.
- Test hardening and a smaller hub firmware build (no visible change).

## 0.3.13 (2026-11-19)

- The command list in the left sidebar and the free-keys list on the right are now tidy grids: equal rows, a common left edge, long names shortened with a tooltip.
- A selection can be cleared: click the key or widget again, click empty space on the remote or canvas, or press Escape.
- Key labels stay readable when a key is selected, including the Vol and Ch legends.
- The drawn remote's colour keys are dark keys with a coloured ring, and the light and socket keys show the symbols printed on the real remote.

## 0.3.12 (2026-11-19)

- Fixed "Home Assistant cannot be read" on larger installs: the add-on cut Home Assistant's answer short when it arrived in more than one piece, so entities and services never loaded. The add-on log now says why whenever Home Assistant cannot be read.

## 0.3.11 (2026-11-19)

- Colours and themes in the editor: give any button or tile a colour from the palette or a custom one, pick a built-in theme (Cards, Midnight, Daylight, High contrast) or create your own in the new theme editor with a live preview. The remote needs a firmware update to show them; the editor says what a linked remote cannot display yet.
- Rename a hub from Home Assistant (Reconfigure → Hub ID / Name); the editor keeps the hub's draft, applied config and history under the new id. Needs a hub firmware update.
- Integration: custom themes are re-sent until the remote confirms them.

## 0.3.10 (2026-11-18)

- The editor's History page shows one status on the newest revision (On the remote, Sending…, Not sent, or Rejected by hub with a See why reason); older revisions show only Compare and Restore. The apply dialog and the pending banner use the same words and never show a raw hash.
- The "Config size" meter bar uses the same net figures as its label.

## 0.3.9 (2026-11-18)

- The editor shows one "Config size X of Y KiB on this hub" meter using the hub's own limits (current firmware has no fixed 256-command limit; old firmware keeps its count meters). Over budget it says what to remove; a full hub flash is reported plainly.
- Integration (behaviour change): command buttons are now created disabled, and a device with more than 200 commands gets none unless the new option "Create command buttons for devices with more than 200 commands" is on. Existing buttons stay enabled; every command remains available through `remote.send_command`.
- Hub firmware (in the repo, needs a firmware update): per-load command, input and IR tables with one memory budget, chunked config transfer, boot refusal of an oversized cached config.

## 0.3.8 (2026-11-18)

- The editor's drawing of the Elite now looks like the real remote: dome top and matte grip, a rounded-diamond D-pad with OK, single Vol and Ch/Pg keys, notched Exit/Menu/Mute/Back, one play/pause key, one DVR/Guide/Info bar, outlined ACTIVITIES/DEVICES, ringed colour keys, one +/- key and printed icons. Key positions were measured on the device and a photo.
- Assigned keys are outlined in teal instead of filled; the D-pad, Vol, Ch/Pg and +/- keys show their halves only on hover, focus, selection or when assigned.

## 0.3.7 (2026-11-18)

- Fix: the integration's protocol decoder ignores unknown fields from a newer hub (0.3.6 could not connect to the current hub firmware).
- Studio layer pills fit narrow widths; opt-in RF slot learning; hub signed OTA firmware updates.

## 0.3.6 (2026-11-18)

- Editor: the Elite drawing follows the photo proportions.
- Fix: the "upload your YAML in the add-on" repair clears once the hub accepts an add-on config and can be dismissed.

- History page: each earlier version has the same two actions in an aligned column, Compare and Restore (Restore loads it as the draft; nothing reaches the hub until Apply). The Revert button is gone (Restore, then Apply, covers it). Rows show the revision, relative time (exact on hover), a one-line summary and Applied / Acknowledged by Home Assistant / Rejected badges.
- The editor header always shows "Hub: id" with whether Home Assistant is polling for it (the hub switcher when several are linked).

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

- Fix: the add-on failed to start under a Supervisor ("/bin/sh: can't open '/init': Permission denied"). The
  AppArmor profile allowed executing the s6 scripts but not reading them; it now grants read as well.

## 0.3.0 (2026-11-18)

- Reverting to the previous version keeps the config it replaces in the history, so a revert can be reverted.
- Editor: Home Assistant service actions and the older HA event action are labelled apart in the key
  map ("HA light.toggle light.x" vs "HA event name"); the action picker offers "Home Assistant action".
- `GET /api/v1/config/history?rev=` returns the source config of an applied or history version, so the
  editor can restore it as a draft and diff it.
- Editor UI speaks the real API: nested errors, draft `rev` with `If-Match`, all-errors validation,
  metered preview, hub picker, apply status with acknowledgement, Send IR.
- Editor API: Home Assistant entity/service pickers (read-only Core API), IR database browsing and
  import, IR test-fire (calls only `ha_harmony.send_ir`), draft revisions (`If-Match`), all
  validation errors, hub list, apply acknowledgements and status.
- Bundle token is checked before the rate limiter; long polls are woken at shutdown.

## 0.2.0

- Bundle feed for the integration (`/api/v1/bundle`, `/bundle/wait`) behind a bearer token, and
  Supervisor discovery of service `ha_harmony`.

## 0.1.0

- Initial skeleton: ingress-only API server (draft, validate, preview, apply, revert, YAML
  import/export) and a placeholder page. The editor UI is not built yet.
