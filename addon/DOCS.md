# Harmony Editor

## What it is

The Harmony Editor is where you design a Harmony hub's setup: devices and their IR codes, activities,
key mappings, Home Assistant actions and the remote's touch pages. You edit a draft in your browser,
review what changes, and apply it. The **Harmony integration** (`ha_harmony`) pulls the applied config,
checks it again, and sends it to the hub; the editor never talks to the hub itself.

## Requirements

- Home Assistant OS or Supervised (add-ons need the Supervisor). Use **Supervisor 2026.10.3 or newer**:
  admin-only access to the sidebar panel and the stripping of client-supplied identity headers were
  fixed in the Supervisor from 2026.10.2 / 2026.10.3, and older versions let a non-admin who knows the
  ingress URL reach the editor (details below).
- The `ha_harmony` integration, **version 0.3.0 or newer**, installed (HACS) with your hub set up.
- A rooted hub running the ha-harmony daemon (see the repository README). The editor is useless
  without one.

## Install

1. *Settings -> Add-ons -> Add-on store -> menu -> Repositories*: add `https://github.com/metril/ha-harmony`.
2. Install **Harmony Editor**, start it, and open **Harmony** from the sidebar.

The add-on image is built by the repository's `addon-publish` workflow from an `addon-v*` tag; if the
store shows no installable version, that image has not been published yet.

## Link it to the integration

At start the add-on announces itself to Home Assistant. A discovery card for the Harmony integration
appears (*Settings -> Devices & services*): confirm it. The integration then shows an **Add-on link**
sensor:

| State | Meaning |
| --- | --- |
| Not linked | Never confirmed; restart the add-on to announce again. |
| Connecting | Reaching the add-on. |
| Connected | Linked and following the add-on's applied config. |
| Add-on unreachable | The add-on is stopped or restarting; the hub keeps its config and the integration its cached copy. |
| Token refused | The add-on's token changed; restart the add-on and confirm the discovery again. |
| Nothing applied yet | Linked, but no config has been applied in the editor. |
| Config rejected | The integration found the applied config invalid or too new; see its repair issue. |

The sensor's `config_state` attribute says whether the hub runs the newest config (`accepted`), has not
taken it yet (`pending`) or refused it (`rejected`; Home Assistant keeps acting on the config the hub
still runs).

## Import an existing YAML

Integration versions before 0.3.0 read `<config>/ha_harmony/<hub>.yaml`. That file is no longer read.
In the editor choose **Import YAML**, pick the file, review the draft and apply. IR file references
(`flipper:` / `smartir:`) cannot be resolved by the import: inline the codes first. The format is in
`docs/ha-config.md`.

## Bluetooth devices and remotes

Needs hub firmware with Bluetooth and the Harmony integration connected. Open **Bluetooth** in the editor's menu.

- **A TV box or phone the hub should control.** Choose **Add Bluetooth device** (or **Add a device**, then
  *Bluetooth device*) and **Start pairing**. A countdown starts; on the device open Bluetooth settings and pair with
  the entry whose name starts with *Harmony*. When it appears, name it, keep or change its commands and
  **Add device**. Use the test button on the Bluetooth page to send a key once. Devices the hub already knows but
  your config does not show *Not in your config* with **Add to config**.
- **A Bluetooth remote.** Put the remote in pairing mode, choose **Add Bluetooth remote**, **Scan for remotes** and
  **Pair** next to yours (stronger signal bars are closer). Then choose what each button does; defaults are
  filled in. **Learn a button** shows which button you press and lets you assign a key to it.
- Nothing reaches the hub until you apply the draft. **Forget** removes the hub's pairing only; its config entry stays.
- If pairing is refused or times out the page says what to try next. If it says the hub has no Bluetooth, is
  offline or has not reported yet, nothing is wrong with your config: update the hub firmware, check the hub and Home
  Assistant, or update the integration.

## The studio in five lines

1. The remote canvas shows the remote; click a key or a page widget to select it.
2. The palette lists devices, activities, IR commands and Home Assistant actions.
3. Assign by dragging from the palette onto a key or widget, or by clicking and choosing.
4. Pages hold the touch layout (buttons, tiles); add, reorder and preview them with the size meters.
5. **Apply** shows what changed, then publishes it; the integration pushes it to the hub.

## What it can and cannot reach

- Sidebar editor: reachable only through Home Assistant ingress, from the Supervisor ingress
  gateway, and only with the logged-in user's id (`X-Remote-User-Id`, set by the Supervisor; a
  request without it gets `401`). No port is published to the host. Apply, revert, import and IR
  test-sends are logged with that user id (`audit user=... action=... hub=...`; never config
  content or tokens).
- Who may open it: the add-on asks for `panel_admin: true`, but the add-on cannot check admin
  status itself: ingress forwards the user's id and names, and no header says whether the user is
  an administrator. Admin-only access is enforced by the Supervisor, and only from Supervisor
  2026.10.2 (Supervisor PR #7308); before that, a non-admin who knows the ingress URL could reach
  the editor. Use Supervisor 2026.10.3 or newer (it also strips client-supplied identity headers
  case-insensitively, PR #7377). This is a Supervisor version, which the add-on's `homeassistant:`
  (Core version) field cannot express, so no minimum is declared.
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

## Backups

The draft, the applied config and the last five versions per hub live in the add-on's own `/data`,
which is included in Home Assistant backups. The IR database cache (`/data/irdb`) is excluded and
refetched when needed. The integration also keeps its last applied config in `.storage`. **Revert**
returns to the previous version, and a revert can itself be reverted.

## Troubleshooting

- **Apply waits for Home Assistant.** The editor only publishes; the integration confirms once the
  hub runs the config. If Home Assistant is down or the link sensor is not *Connected*, the apply stays
  waiting and completes when the link returns.
- **401 in the editor.** The Supervisor did not forward the logged-in user's id: it is older than
  2026.10.3. Update the Supervisor.
- **IR archive offline.** The database is fetched over HTTPS from a fixed list of hosts and cached; with
  no network the editor works from the cache, and a device that was never cached cannot be browsed.
- **Integration says the config is rejected.** Open its repair issue; the hub keeps running the previous
  config until a valid one is applied and accepted.

## Image and local builds

The add-on is installed from the prebuilt image named by `image:` in `config.yaml`
(`ghcr.io/metril/ha-harmony-editor-{arch}`, published by the `addon-publish` workflow on
`addon-v*` tags). Supervisor local builds are not supported: the image needs the repository's
shared Python packages (`harmony_core`, `harmony_irdb`), which lie outside the `addon/` folder
that is the only build context a local build receives.

## Limitations

IR file references (`flipper:` / `smartir:` files) cannot be resolved when importing YAML; inline
the codes first.
