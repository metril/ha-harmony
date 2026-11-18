# Harmony Editor

Hosts the visual editor, IR database, config store and preview for the Harmony hub. It **feeds**
the Harmony custom integration, which stays the hub's runtime client, re-validates and compiles
what it pulls, and keeps its own copy of the last applied config.

## Status

Skeleton: the API works, the editor UI is a placeholder page.

## Security model

- Reachable only through Home Assistant ingress (the sidebar panel, admins only). No port is
  published and the add-on only accepts connections from the Supervisor ingress gateway.
- No Home Assistant Core API access, no Supervisor API access, no folder mappings. It cannot read
  or write your Home Assistant configuration.
- Everything it stores lives in its own `/data` (draft, applied config, last 5 versions per hub).
- State-changing requests need a JSON (or YAML for import) content type and a custom header, and
  cross-site requests are refused.
- The server runs as an unprivileged user; only the s6 init stays root.

## Options

| Option | Meaning |
| --- | --- |
| `hub_host` | Hub address (optional). |
| `hub_port` | Hub port, default 8099. |
| `hub_token` | Hub pairing token (optional). Never returned by the API or written to logs. |
| `log_level` | `debug`, `info`, `warning` or `error`. |

## Limitations

IR file references (`flipper:` / `smartir:` files) cannot be resolved when importing YAML; inline
the codes first.
