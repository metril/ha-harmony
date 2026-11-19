# Changelog

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
