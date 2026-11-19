# Changelog

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
