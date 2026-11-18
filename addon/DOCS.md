# Harmony Editor

Hosts the visual editor, IR database, config store and preview for the Harmony hub. It **feeds**
the Harmony custom integration, which stays the hub's runtime client, re-validates and compiles
what it pulls, and keeps its own copy of the last applied config.

## Status

Skeleton: the API and the integration feed work, the editor UI is a placeholder page.

## Moving from the integration's YAML file

The integration (0.3.0) no longer reads `<config>/ha_harmony/<hub>.yaml`. Upload that file once
with `POST /api/v1/config/import_yaml` (the editor's import button), then apply it; the
integration picks it up within moments.

## What it can and cannot reach

- Sidebar editor: reachable only through Home Assistant ingress (admins only), from the
  Supervisor ingress gateway. No port is published to the host.
- Integration feed: `GET /api/v1/bundle` and `/api/v1/bundle/wait` (long-poll) serve the applied
  config, and `POST /api/v1/bundle/ack` lets the integration report whether it applied it. They need a bearer token (32 random bytes, generated at first start, stored `0600` in
  `/data`, never logged or shown in the UI) and a peer on the Supervisor network; they refuse
  anything arriving through ingress. Repeated failures are rate limited.
- Discovery: at start the add-on tells the Supervisor "service `ha_harmony` is at
  `<hostname>:8099` with this token", and withdraws it at stop; Home Assistant then offers to link
  the Harmony integration. This uses the add-on's own Supervisor token and the `discovery:` entry
  only: no `hassio_api`, no role (per the Supervisor API docs, `/discovery` and `/addons/self/*`
  need no extra permission). The token is handed over only this way.
- Home Assistant Core API (`homeassistant_api: true`, through the Supervisor proxy with the
  add-on's own token): the add-on **reads** your entity list (`GET /states`) and service list
  (`GET /services`) so the editor can offer pickers, filtered to the domains a Harmony action can
  target and cached for a few seconds. Its HTTP client can only issue GET requests. It calls
  **exactly one service**: `ha_harmony.send_ir`, when you press "test" on an IR code in the editor
  (rate limited to one send per half second and 30 per minute). It calls nothing else, reads no
  configuration files, and never writes to Home Assistant. Without a Supervisor token (plain
  docker) both are off and the editor falls back to typed entity ids.
- No Supervisor API beyond discovery, no folder mappings. It cannot read or write your Home
  Assistant configuration.
- The IR database (Harmony archive over HTTPS from a fixed list of hosts) is cached in
  `/data/irdb`, which is excluded from backups; it works offline from that cache.
- Everything it stores lives in its own `/data` (draft, applied config, last 5 versions per hub).
- State-changing requests need a JSON (or YAML for import) content type and a custom header, and
  cross-site requests are refused.
- The server runs as an unprivileged user; only the s6 init stays root.

## Options

| Option | Meaning |
| --- | --- |
| `log_level` | `debug`, `info`, `warning` or `error`. |

## Installation note

The add-on is installed from the prebuilt image named by `image:` in `config.yaml`
(`ghcr.io/metril/ha-harmony-editor-{arch}`, published by the `addon-publish` workflow on
`addon-v*` tags). Supervisor local builds are not supported: the image needs the repository's
shared Python packages (`harmony_core`, `harmony_irdb`), which lie outside the `addon/` folder
that is the only build context a local build receives.

## Limitations

IR file references (`flipper:` / `smartir:` files) cannot be resolved when importing YAML; inline
the codes first.
