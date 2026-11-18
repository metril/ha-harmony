# Changelog

## Unreleased

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
