# Harmony Editor add-on: HTTP API

The contract for the editor frontend (and the integration's channel). Source of truth is the
code in `harmony_addon/`; `tests/py/test_addon_api_doc.py` fails if a served route is missing from
the table below. There is no polling anywhere: the add-on pushes over two WebSockets (`/channel`, `/live`).

## Conventions

- **Ingress** routes are reachable only through Home Assistant ingress (the Supervisor gateway
  peer). Pages additionally need `X-Ingress-Path`. **Bearer** routes (`/api/v1/channel`) need
  `Authorization: Bearer <token>` from the Supervisor network, and are refused via ingress.
- Every ingress request that changes state (POST/PUT/PATCH/DELETE; a DELETE sends `{}`) needs `Content-Type: application/json`
  (`text/yaml` or `application/yaml` for `import_yaml`), the header `X-HH-CSRF: 1`, and a same-origin
  `Origin`/`Sec-Fetch-Site`; otherwise `403 csrf`. Bearer POSTs need only the JSON content type.
- Bodies and query strings are schema-validated, unknown keys and repeated query params are
  `400 bad_request`. Request bodies are at most 1 MiB. `hub` defaults to `default` and must match
  `[A-Za-z0-9][A-Za-z0-9._-]{0,62}` (the dot is accepted for ids already in use; new ids have none).
- Errors always look like `{"error": {"code": "...", "msg": "...", ...extra}}`. Responses to `/api/`
  carry `Cache-Control: no-store`.
- Validation entries are `{path: [..], msg: str, line: null}`; at most 100 are returned.

## Routes

| Method | Path | Auth | Summary |
| --- | --- | --- | --- |
| GET | `/` | ingress | The editor UI (`index.html`; placeholder page when the frontend is not built) |
| GET | `/assets` | ingress | Hashed UI assets under /assets/… (immutable cache; only present when the frontend is built) |
| GET | `/api/v1/health` | ingress | Versions and `capabilities` (`ha_actions` probed from the schema, `page_format: 2`) |
| GET | `/api/v1/configs` | ingress | The config library: every named config with draft/revision summary and the hubs running it |
| POST | `/api/v1/configs` | ingress | Create a config: blank, a duplicate of another config (or one of its revisions) or from YAML |
| GET | `/api/v1/configs/{id}` | ingress | One config: draft, revision list, hubs running it |
| PATCH | `/api/v1/configs/{id}` | ingress | Rename |
| DELETE | `/api/v1/configs/{id}` | ingress | Delete a config with its draft and all revisions (hubs keep their own snapshot) |
| PUT | `/api/v1/configs/{id}/draft` | ingress | Store the config's draft (honours `If-Match`) |
| POST | `/api/v1/configs/{id}/revs` | ingress | Save the draft as the next immutable numbered revision |
| GET | `/api/v1/configs/{id}/revs/{n}` | ingress | One revision with its config |
| POST | `/api/v1/configs/{id}/restore` | ingress | Copy a revision into the draft |
| POST | `/api/v1/configs/{id}/import_yaml` | ingress | Upload YAML as the draft |
| GET | `/api/v1/configs/{id}/export_yaml` | ingress | Draft or a revision as YAML |
| POST | `/api/v1/config/validate` | ingress | All validation errors (stateless: config in the body) |
| POST | `/api/v1/config/preview` | ingress | Decoded pages, usage meters; error tolerant (stateless) |
| POST | `/api/v1/config/keymap` | ingress | Effective hard-key action per layer (stateless) |
| GET | `/api/v1/hubs` | ingress | Hubs known to the store with the config and revision each runs, hub ids integrations are connected for, adoptable stores |
| GET | `/api/v1/hubs/{hub}` | ingress | One hub: running config and revision, history, ack |
| POST | `/api/v1/hubs/{hub}/apply` | ingress | Run a config revision on the hub (no `rev`: save the draft as a revision first) |
| POST | `/api/v1/hubs/{hub}/revert` | ingress | Re-apply the hub's previous version |
| POST | `/api/v1/hubs/adopt` | ingress | Move a hub's store (default: `default`) to a linked hub id |
| GET | `/api/v1/ha/entities` | ingress | Home Assistant entities for pickers, with area, device, integration and labels |
| GET | `/api/v1/ha/services` | ingress | Home Assistant services for pickers |
| POST | `/api/v1/ir/test` | ingress | Fire one IR code through `ha_harmony.send_ir` |
| GET | `/api/v1/ir/brands` | ingress | Brand search |
| GET | `/api/v1/ir/models` | ingress | Models of a brand (paged) |
| GET | `/api/v1/ir/device` | ingress | A converted device block |
| GET | `/api/v1/ir/candidates` | ingress | Power code candidates |
| POST | `/api/v1/ir/parse` | ingress | Parse pasted Pronto / Flipper / SmartIR |
| GET | `/api/v1/bt/codes` | ingress | Bluetooth: built-in command names, default command set, default remote key map, key names |
| POST | `/api/v1/bt/pair` | ingress | Bluetooth: open a pairing window (host or remote) |
| POST | `/api/v1/bt/pair/stop` | ingress | Bluetooth: close the pairing window |
| POST | `/api/v1/bt/pick` | ingress | Bluetooth: bond a scanned remote by address |
| POST | `/api/v1/bt/forget` | ingress | Bluetooth: remove a bond |
| POST | `/api/v1/bt/key` | ingress | Bluetooth: test-fire a key at a paired host |
| GET | `/api/v1/live` | ingress | WebSocket push to the editor: hubs, config status, Bluetooth state, remote keys |
| GET | `/api/v1/channel` | bearer | WebSocket to the integration: bundle push, acks, Bluetooth relay |

Query/body details follow. `hub` is a query parameter on `config/validate|preview|keymap` (it selects the hub limits the integration reported) and a path
segment on `hubs/{hub}/...`. Config ids are `[A-Za-z0-9][A-Za-z0-9._-]{0,62}` (generated lowercase slugs).

### Health

`GET health` -> `{ok, addon_version, core_version, bundle_schema, capabilities: {ha_actions: bool,
page_format: int}}`. `capabilities` says what this build can store: `ha_actions` is true when the
config schema accepts a Home Assistant service action (`ha: {service, target: {entity_id}, ...}`),
probed from the schema itself; `page_format` is the page format the compiler and editor speak. The
editor disables the Home Assistant action picker (with a reason) when `ha_actions` is false.

### Config library

Configs are a global library, independent of hubs. Each has an id (slug generated from its name, unique), a free name, **one autosaved
draft** and any number of **immutable numbered revisions** (1, 2, 3, ... never reused, never edited, no cap; saved on demand). Any
revision of any config can be applied to any hub. A hub records which config and revision it runs and keeps its own **snapshot** of
that config, so deleting or renaming a config never changes or breaks a hub. Library changes are pushed on the `configs` topic of `/live`.

Summary object (the `configs[]` rows, and the head of every config response):
`{id, name, created, updated, latest_rev: int|null, rev_count, draft_rev, has_draft, draft_updated: int|null,
base_rev: int|null, dirty}`. `draft_rev` is the integer **draft revision** (autosave counter, used by `If-Match`), unrelated to
revision numbers. `base_rev` is the revision the draft started from (set by `restore` and `revs`; null for a never-saved draft).
`dirty` is true when the draft differs from revision `base_rev` (or has no base). Times are Unix seconds.

- `GET configs` -> `{configs: [{...summary, hubs: [{hub, rev}]}]}` (`hubs`: the hubs whose current version is a revision of that config).
- `GET configs/{id}` -> `{...summary, draft: {config, updated, base_rev}|null, revs: [{rev, at, note, hash, parent: int|null}],
  hubs: [{hub, rev}]}` (`revs` oldest first, no configs; `hash` is a content hash of the config). `404 not_found`.
- `POST configs` body `{name, from?: {id, rev?}, yaml?: str}` (`from` and `yaml` are mutually exclusive) -> `201 {ok:true, ...GET configs/{id}}`.
  No source: a blank config (no draft). `from`: its draft becomes the new config's draft (`from.rev`: that revision's config; `404 not_found`
  when unknown; `409 store` when the source has no draft). `yaml`: parsed and checked like `import_yaml` (`400 bad_yaml | yaml_alias`,
  `422 ir_files_unresolvable | invalid_config`). `name` is 1-80 chars after trimming/collapsing whitespace, no control characters.
- `PATCH configs/{id}` body `{name}` -> `{ok:true, ...GET configs/{id}}`. The id never changes; hub snapshots keep the name they were applied with.
- `DELETE configs/{id}` -> `{ok:true}`; `404 not_found`. Allowed while hubs run it.
- `PUT configs/{id}/draft` body `{config}`; optional `If-Match: 3` (also `"3"`, `W/"3"`). Stale -> `409 conflict` with `error.rev`
  set to the current `draft_rev`. No header = unconditional. -> `{id, draft_updated, draft_rev, base_rev, dirty}`. `import_yaml`
  and `restore` also bump `draft_rev`; `revs` does not.
- `POST configs/{id}/revs` body `{note?: str <= 200}` -> `201 {ok:true, rev: {rev, at, note, hash, parent}, id, draft_updated, draft_rev,
  base_rev, dirty}`. Saves the draft as revision `latest+1` (`parent` = the draft's `base_rev`); the draft stays and now has
  `base_rev` = the new revision (`dirty:false`). `409 store` when there is no draft.
- `GET configs/{id}/revs/{n}` -> `{id, rev, at, note, hash, parent, config}`; `404 not_found`.
- `POST configs/{id}/restore` body `{rev}` -> `{ok:true, id, draft_updated, draft_rev, base_rev: rev, dirty:false}`. Replaces the draft with
  that revision's config (the old draft is lost; save a revision first to keep it).
- `POST configs/{id}/import_yaml` YAML body (max 1 MB, no anchors; `Content-Type: text/yaml` or `application/yaml`) ->
  `{ok:true, id, draft_updated, draft_rev, base_rev, dirty}`; `422 ir_files_unresolvable | invalid_config`; `400 bad_yaml | yaml_alias | too_large`.
  Honours `If-Match`.
- `GET configs/{id}/export_yaml?source=draft|rev:N` (default `draft`) -> `text/yaml`; `404 empty` (no draft) or `404 not_found`.
- `POST config/validate?hub=&all=1|0` body `{config}` (required; stateless) -> `{ok, errors}`;
  `all=0` returns only the first error. After a valid parse, `errors` may also hold non-blocking warnings
  `{path, msg, line:null, severity:"warning"}` (theme contrast, weak-ink free colours); they never make `ok` false, and
  errors carry no `severity` key. `config/preview` adds the same warnings to its `errors` (its `ok` ignores them).
  The editor keeps them apart from errors (a "N warnings" chip; they never block Apply).
- `POST config/keymap?hub=` body `{config}` (stateless), see Keymap below.
- `GET hubs/{hub}` -> `{hub, applied: {config_id, rev, name, at, hash}|null, history: [{config_id, rev, name, at, hash}] (newest
  first), history_max: 5, config_exists: bool, ack: {rev, result, at, detail?}|null}`. `applied` is what the hub runs; `name` is the
  config's name when it was applied, `hash` the compiler's config hash, `at` the apply time. `config_exists` is false once the config
  was deleted from the library (the hub still runs its snapshot).
- `POST hubs/{hub}/apply` body `{config_id, rev?}` -> `{ok:true, hub, applied, history, history_max}`. With `rev`: that revision of
  that config. Without `rev`: the config's draft is validated and compiled, saved as a new revision with note `Applied to <hub>`
  (the newest revision is reused instead when the draft is unchanged since it), then applied; `409 no_draft`. `404 not_found` for
  an unknown config or revision. `422 {ok:false, errors}` when the config does not validate or compile (strict; the preview's tolerance
  does not apply; no revision is created). The previous version goes to `history` (cap `history_max`). The integration is then pushed the new bundle.
- `POST hubs/{hub}/revert` body `{}` -> `{ok:true, hub, applied, history, history_max}`. Re-applies the newest `history` entry; the version
  left goes on top of `history`, so a revert can itself be reverted. `409 store` when there is no previous version.
- Migration: a hub state file written by an older add-on (one config per hub) is converted once at start-up. It becomes a library config
  named after the hub id whose revisions are its old history and applied versions (oldest first, note `Migrated`), with its old draft
  as the draft; the hub then runs the newest of them.
- `editor:` is an optional top-level mapping of opaque editor metadata (depth <= 8, <= 5000 nodes,
  <= 64 KiB, plain JSON). It is stored, exported and imported with the config, ignored by the
  compiler, and **stripped from the bundle**, so editor-only changes never change the bundle `rev` or
  reach the hub.

### Preview

`POST config/preview?hub=` body `{config}` (required; the editor sends its current text) always answers `200`, with every page that compiles even
when the draft has errors elsewhere:

```
{ok, hash|null, errors: [{path,msg,line}],
 missing_pages: [{index, id}],
 pages: [{index, id, ok, error|null, page|null, wire_pages: [...], bytes|null, bytes_v1|null, limit,
          widgets|null, widget_limit}],
 usage: {json_bytes, json_max, pool_words, pool_max, keys, keys_max, devices, devices_max,
         activities, activities_max, commands, commands_max, inputs, inputs_max,
         config_bytes, config_budget, soft_keys, soft_keys_max} | null}
 # the *_max of json, pool, commands and inputs are null on a current hub (no such limit): it has one
 # memory budget, config_bytes of config_budget. They come from the limits the hub's integration
 # reported (`limits` in its channel `hello`); a hub that never did gets the old numbers.
```

`pages` has one entry per **config** page. `page` is the page-format-v2 model of its first wire page
(`harmony_core.page_codec.page_to_dict`, the editor's rendering contract: `version, id, cols, rows,
gen, total, kind, back, seq, act, title, widgets: [{kind, id, x, y, w, h, label, role, shape,
icon_only, icon, bind}]`, plus `trailing` (only when true) / `align` (`left` or `right`, only when set; centre is the default); roles, shapes, icons and nav binds are already resolved). `wire_pages` lists
every wire page the config page compiles to: one for an ordinary page, several when the compiler
splits a `list:` page ("2/3" groups), each `{page_id, seq: {idx, cnt}|null, bytes, bytes_v1,
widgets, page}` (`page` is the same model shape; `page_id` is the wire page number that nav `bind`s
use). `bytes`/`limit` follow the **v2** size rule (the remote speaks v2 once deployed), `bytes_v1` is
the v1 size of the same page; on the config page they are the largest over its wire pages. A page
that does not fit still reports its sizes but is `ok:false` with `page:null`, `wire_pages:[]`. The
integration still pushes v1 pages for now; this only affects what the editor shows. `usage` and
`hash` are only present when the whole config validates. Widget wire ids on later pages are exact
only when the draft is valid. Results are memoised by content (editor metadata excluded), computed
off the event loop; a 12-page config answers in about 20 ms cold and a few ms from the memo.

Page-level compile problems (a list too long, a nav target that does not exist, a page over its byte
budget) are reported with the path of the config page (`["pages", i, ...]`); the other pages are still
returned.

`POST config/keymap?hub=` body `{config}` -> `{ok:true, default: [row], activities: [{id, name, keys: [row]}]}`
with one row per hard key (35, ordered by code): `{key, code, source: "default"|"activity"|"none",
action: null | {kind, label, ...}}`. Lookup is the hub's: activity keys, then the default layer, else
none (`source:"none"`, `action:null`). Default-layer rows use `source:"default"`. `action.kind` is
`device` (`device`, `command` or `input`), `activity` (`activity`), `ha` (`event` for an event action,
or `service` + `entity` for a Home Assistant service action; `entity` is the first one, `"x +N"` for more), `bt` (`usage`),
`ir` (`form`) or `none`. The `ha` action table is flattened with the same rule. Invalid config -> `422 {ok:false, errors}`.

### Status and hubs

- Status is pushed, not polled: see `Live (editor WebSocket)` (`hub:<id>` topic = the old `config/status` payload; `hub_last_seen`
  and `linked_hubs` are replaced by `connected`, below).
- Linked hub ids: every authenticated channel `hello` with a valid `hub` records the id and the time, in memory and in
  `/data/linked_hubs.json` (at most 32 ids). A hub is `connected` while its channel socket is open.
- `GET hubs` -> `{hubs: [{id, has_applied, config_id, config_name, rev, applied_rev, applied_at}], linked: [{id,
  last_seen, name?}], now, adopt: {from: "default", to: [id]}|null}`. `adopt` is set when a store exists
  under `default` and some linked hub id has no store of its own (the editor offers the move).
- **Adoption.** When exactly one hub id other than `default` is linked and has no store, and `default`
  has one, the add-on moves the `default` store file to that id automatically (one atomic rename,
  logged, repeating it does nothing), so a config applied under `default` is servable to the
  integration without re-importing. With several linked ids it only offers it (`adopt` above).
- `POST hubs/adopt` body `{from?: "default", to}` -> `{ok:true, hub}`; `409 not_linked` (no integration
  has connected for `to`), `409 hub_exists` (`to` already has a store), `404 nothing_to_adopt`.
  Optional `theme_notices: [string]` (not sent by this add-on yet): what the linked remote's firmware cannot show of the
  config's colours or theme (the compiler's `theme_notices`). The editor shows one non-blocking notice in the Themes view
  and the apply dialog when it is non-empty.

### Home Assistant (read only)

- `GET ha/entities?domain=&q=&area=&diagnostic=0|1&state=0|1` -> `{entities: [{entity_id, name, domain,
  device_class?, area_id?, device_id?, platform?, label_ids?: [str], diag?: true, state?}],
  areas: [{id, name, floor_id?, floor?}], devices: [{id, name}], labels: [{id, name}], registries: bool,
  total: int, truncated?: true}`, sorted by domain then name. Rows refer to areas, devices and labels by
  **id**; the lists carry the names (ids are the keys, names are for display, so two areas called "Hall"
  on different floors stay apart). `area_id` is the entity's own area, else its device's; `device_id` is
  set when the device has a name (the user's name wins); `platform` is the integration domain; the
  lists hold only what the rows use. Disabled and hidden entities are always left out, and so are
  `diagnostic` and `config` entities unless `diagnostic=1` (they then carry `diag: true`). `state` is
  left out unless `state=1` (only the Overview shows it), so the answer, and its ETag, do not change
  with every state change. `domain` must be on the allow-list; `q` (at most 64 characters, words are
  ANDed over name, entity id, area, device and label names and the integration) and `area` (an area id)
  filter on the server.
  The editor fetches the whole list once and filters in the browser. A row is about 130 bytes: 3,000
  entities are ~450 KB, and the answer is cut at 20,000 entities (~2.6 MB), sorted first, so the cut
  keeps the first by domain and name, with `truncated: true` and `total` the number that matched (the
  editor then says so and `q`, `domain` and `area` narrow it). The answer carries an `ETag` and
  `Cache-Control: no-cache`; a request whose `If-None-Match` matches (a `W/` prefix and quotes are
  ignored, `*` matches) gets an empty `304`. The serialised answer is memoised per query until Home
  Assistant's states or the registries are read again, and concurrent requests share one read.
  `registries` is false when the registries could not be read (see below): the rows then carry no
  area, device, integration or labels and nothing is excluded by flag, so the editor can only group by
  type. That is not an error. Wrong-typed registry fields are ignored, never an error.
- `GET ha/services?domain=` -> `{services: [{domain, service, name, fields: [str]}]}`.
- Both: `503 ha_unavailable` when Home Assistant cannot be read (no token, network, bad answer);
  `error.reason` is a short cause (`no_access`, `status_503`, `too_large`, `not_json`, `TimeoutError`, ...).
- Registries: the add-on reads the area, device, entity, floor and label registries through the Supervisor's
  WebSocket proxy (`ws://supervisor/core/websocket`, authenticated with the add-on's own token). The
  client can send exactly five commands, enforced in code: `config/area_registry/list`,
  `config/device_registry/list`, `config/entity_registry/list`, `config/floor_registry/list` and
  `config/label_registry/list` (floors and labels are optional; all are reads). Messages are capped at 8 MiB and the whole conversation at 20 s; the result is
  cached for 60 s (a failed refresh keeps the last good copy and is not retried for 15 s) and a failure
  is logged once per cause, never with the token.

### IR database

- `GET ir/brands?q=` -> `{brands: [{name, slug, count}]}` (at most 50; empty `q` = most models first).
- `GET ir/models?brand=<slug>&q=&offset=0&limit=50` -> `{brand: {name, slug}, total, offset, limit,
  models: [{file, name, id}]}`; `limit` 1..200.
- `GET ir/device?brand=<slug>&model=<file.json>` -> `{device, forms: {command: "proto"|"pronto"},
  notes, skipped: [{name, reason}], skipped_total, device_type: {id, name}|null, source}`; `device` is a
  config device block.
- `GET ir/candidates?brand=&type=<tv|number|name>&limit=60` (limit 1..200) -> `{candidates: [{kind, action,
  form, count, models, model_files, codesets}], examined, matched, errors, truncated}`; `model_files[i]` is
  the archive file of `models[i]` (what `ir/device?model=` takes), so the wizard can open a likely model
  without searching by name; `truncated` is true when the
  per-request budget (16 MiB of codeset text) ran out before every codeset was looked at.
- `POST ir/parse` body `{kind: "pronto"|"flipper"|"smartir", text (<= 1 MiB)}` ->
  `{ok, actions: [{name, action, form}], skipped: [{name, reason}]}` (at most 4096 actions);
  bad input -> `422 bad_ir` with `error.errors: [{path, msg}]`.
- Errors: `404 unknown_brand`, `400 bad_request` (unsafe names), `504 archive_offline | archive_timeout`,
  `502 archive_error`, `502 archive_too_large` (a codeset with more than 2000 commands; its cache
  entry is deleted). Work on archive data runs off the event loop. The archive cache (`/data/irdb`) keeps it working offline.

### IR test-fire

`POST ir/test` body `{hub, ir, port_mask?}` where `ir` is one inline action (`proto`, `raw` or
`pronto`; file references are refused). -> `{ok:true, action}`. Errors: `422 invalid_ir` (with
`error.errors`), `503 ha_unavailable`, `502 send_failed` (`error.http_status`), `429 rate_limited`
(`error.retry_after_ms`, `Retry-After`; at least 0.5 s between sends, at most 30 per minute for the
whole add-on). The add-on calls exactly `POST /services/ha_harmony/send_ir` with
`{config_entry: <hub>, ir}` and nothing else.

### Channel (integration)

`GET /api/v1/channel?hub=<id>` upgrades to a WebSocket (bearer auth, Supervisor network only, refused via ingress; a failed
bearer is `401`/`429` before the upgrade). One socket per hub id; a second socket for the same id replaces the first (old one
closed `4002 replaced`). Frames are text JSON `{"t": <type>, ...}`; unknown `t` and unknown fields are ignored (debug log).
Replaces the removed `GET bundle`, `GET bundle/wait`, `POST bundle/ack`, `POST bundle/bt`.

**Liveness.** No application ping, no aiohttp `heartbeat`. Both ends set kernel TCP keepalive on the socket (`SO_KEEPALIVE`,
`TCP_KEEPIDLE=60`, `TCP_KEEPINTVL=10`, `TCP_KEEPCNT=3`; helper `harmony_addon.sockopts.set_keepalive`), so a dead peer is
detected within about 90 s and the socket errors/closes. On close the add-on marks the hub not connected (`connected:false`
pushed to `/live`) and fails pending Bluetooth commands with `hub_offline`.

**Reconnect.** The integration reconnects with jittered exponential back-off (1 s doubling to 60 s, +-25 % jitter, reset after a
socket lived 30 s) and always starts with a `hello`. No timers run while connected.

Integration -> add-on:

| `t` | Fields | Meaning |
| --- | --- | --- |
| `hello` | `v: 1`, `hub`, `known_rev?: str\|null`, `hw?`, `prev?`, `name?`, `remotes?`, `limits?` | first frame; must arrive within 10 s of the upgrade or the add-on closes `4000`. `hw`, `prev`, `name`, `remotes` have exactly the rules of the former `X-Hub-Hw/Prev/Name/Remotes` headers (below). `known_rev` is the applied rev the integration already holds. `limits` is the hub's `hello_ack.limits` (the former `X-Hub-Limits` header) |
| `identity` | same fields as `hello` minus `v`/`known_rev` (all optional) | the hub's name, hardware id or linked remotes changed (e.g. a rename); replaces the stored values, may trigger a store move |
| `ack` | `rev`, `result: "ok"\|"error"`, `detail?: str (<= 200)` | what the integration did with a `bundle` |
| `bt_state` | `online: bool`, `supported: bool`, `state?`, `limits?` | the hub's Bluetooth state changed (also sent once after `hello`); `state` is the hub's `bt_state` |
| `bt_result` | `id`, `ok: bool`, `code?`, `msg?`, `state?` | outcome of a `bt_cmd` (sent as soon as it finishes) |
| `bt_key` | `slot`, `page`, `usage`, `down: bool` | a remote key, only while the add-on asked `listen on` |
| `hub` | `online: bool` | the hub link went up or down (also sent once after `hello`) |

Add-on -> integration:

| `t` | Fields | Meaning |
| --- | --- | --- |
| `bundle` | `rev`, `body` | `body` = `{schema, core_version, hub_id, applied_at, rev, config, ...}` (`config` never contains `editor`); sent when a config is applied, and right after `hello` when `rev != known_rev` |
| `nothing_applied` | | answer to `hello` when no config is applied (sent once; a later apply sends `bundle`) |
| `bt_cmd` | `id`, `op: "pair"\|"pair_stop"\|"pick"\|"forget"\|"key"`, op fields | a validated editor command, delivered once. `id` matches `[A-Za-z0-9_-]{1,16}` |
| `listen` | `on: bool` | opt the hub's remote keys in or out (sent after `hello` when a `keys:<hub>` subscriber exists, then on change) |
| `error` | `code`, `msg?` | `bad_hello`, `bad_frame`, `stale_rev`, `hub_mismatch`, `store_error` |

Close codes: `4000` no/invalid hello, `4001` `v` unsupported (add-on closes after sending `error unsupported_version`; the integration
reports `addon_outdated` or `integration_outdated` and does not retry for 10 minutes), `4002` replaced. A `404` on the upgrade (an
add-on without `/channel`) makes the integration set link_state `addon_outdated` and raise a repair issue; there is no fallback.

- **`known_rev`/dedupe.** The add-on keeps no per-socket bundle cursor: after `hello` it sends `bundle` once if the rev differs,
  then one per apply. `ack` for a rev other than the applied one gets `error stale_rev`.
- **Renamed hubs.** The identity fields in `hello`/`identity`: `hw` (stable hardware id, `[A-Za-z0-9._:-]`, 1 to 64), `prev` (the id
  the hub had before a rename, a valid hub id other than `default`), `name` (display name, plain UTF-8, at most 48 bytes, no
  control characters; empty clears it). The add-on records hardware id and name per linked hub (`linked_hubs.json`). When `hub`
  has no store of its own and a store exists under `prev`, or under a linked id with the same hardware id, that store (applied version,
  history, ack) is moved to `hub` in one atomic rename (the generalisation of the `default` adoption; the old
  id is dropped from the linked list). A `prev` whose recorded hardware id differs is another hub and is skipped. It never
  overwrites. The add-on answers with `{"t":"adopt","result":"moved"|"exists"|"none"}` whenever `prev`/`hw` was sent
  (`exists`: `hub` already has a store; `none`: nothing to move, also the answer to a repeat). Moving back is the same call the
  other way round, which the integration uses to undo a rename.
- **Linked remotes.** `remotes`: a list of `{remote: int, caps: int, tier: "none"|"palette"|"custom", notices: [str <= 200]}` (at
  most 8 remotes, 8 notices each). `tier` is the colour support: `none`, `palette` (palette colours and presets) or `custom` (free
  colours and user themes). The add-on keeps the last well-formed report per hub in memory (a malformed one is ignored), derives
  `theme_notices` (every remote's notices, once each) and shows both in the `hub:<id>` topic of `/live`. Also exposed on the
  editor's Themes view and apply dialog.
- Failed bearer attempts are rate limited (`429`); a valid token never is.

### Live (editor WebSocket)

`GET /api/v1/live` upgrades to a WebSocket through ingress (ingress auth plus same-origin `Origin`/`Sec-Fetch-Site` checked at
the upgrade; `403` otherwise). Replaces `GET config/status`, `GET bt/status`, `GET bt/events` and every editor poll. No ping
timers; the server sets TCP keepalive on the accepted socket (as above) and the browser relies on `onclose`, then reconnects
with jittered exponential back-off (1 s to 30 s) and re-sends its subscription.

Client -> server: `{"sub": ["hubs", "configs", "hub:<id>", "bt:<id>", "keys:<id>"]}` (replaces the whole subscription set; the topics are
validated, at most 16, unknown are ignored). Topics:

| Topic | Delivers |
| --- | --- |
| `hubs` | the `GET hubs` payload |
| `configs` | the `GET configs` payload (any library change: create, rename, delete, draft, revision, apply) |
| `hub:<id>` | config status of one hub |
| `bt:<id>` | Bluetooth status of one hub |
| `keys:<id>` | remote keys of one hub; being subscribed is the **listen lease** |

Server -> client, all `{"t": ...}`:

- `snapshot` `{topics: {"hubs": {...}, "configs": {...}, "hub:<id>": {...}, "bt:<id>": {...}}}`: sent right after every `sub` for the newly added
  topics, and after a `resync` (below). `keys:` has no snapshot.
- `hubs` (payload as `GET hubs`), `configs` (payload as `GET configs`), `hub` `{hub, ...status}`, `bt` `{hub, ...status}`, `key` `{hub, seq, slot, page, usage, down, ts}`:
  deltas, each carrying the full new payload (no patches) except `key`.
- `resync` `{topic}`: the server's per-socket queue overflowed and dropped the oldest messages; a fresh snapshot of that topic
  follows. `key` events lost this way are not replayed.
- `error` `{code}` for a bad `sub`.

Payloads (same shapes as the removed endpoints):

- `hub:<id>` -> `{hub, applied_rev, config_id, config_name, rev, acked_rev, result: "ok"|"error"|null, detail, acked_at, connected: bool, connected_at,
  remotes, theme_notices, now}`. `applied_rev` is the bundle rev (a content hash), `connected` is whether the integration's
  channel for that hub is open, `now` the add-on clock (unix seconds).
- `bt:<id>` -> `{hub, status: "ok"|"unsupported"|"offline"|"unknown", state, limits, listening}`. `unknown`: no integration has
  connected (or an integration without Bluetooth); `offline`: the hub is disconnected or the channel is closed; `unsupported`:
  the hub has no Bluetooth. `state` and `limits` as in Bluetooth below. No `age_s`: freshness is push-driven.

**Listen lease.** There is no 30 s lease or renewal. The add-on sends `listen {on:true}` on the channel when the first socket
subscribes to `keys:<id>`, and `listen {on:false}` when the last one unsubscribes or closes, and after a channel reconnect
re-asserts the current state. Keys flow `bt_key` (channel) -> `key` (live) with no ring and no replay.

### Bluetooth relay

The hub's Bluetooth pairing and key learning (protocol: `docs/proto/bluetooth.md`; user guide: `docs/bluetooth.md`) reach the
editor through the integration: the add-on never talks to the hub. An editor command POST becomes a `bt_cmd` on the hub's channel,
the integration runs it against the hub and answers `bt_result` at once, and the new `bt_state` is pushed as `bt_state`. Every
queue is bounded (16 commands in flight per hub, 32 hubs); the integration validates every command again against the hub's
limits. No cadence, no fast mode.

- **Command ids.** Every command POST (`pair`, `pair/stop`, `pick`, `forget`, `key`) takes an optional `cmd_id`
  (`[A-Za-z0-9_-]{1,16}`; generated when absent) and returns it as `id` (also in error bodies). Re-posting a `cmd_id` that is in
  flight joins it, and one that has finished (the last 32 per hub) returns the stored result: a retry after a timeout never runs
  the command twice. A command is delivered once. If the channel closes with a command in flight it is failed `409 hub_offline`.
- `GET bt/codes` -> `{builtins: [{name, page, usage, default}], remote_keys: [{name, page, usage, key}], keys: [str]}`: static
  tables from `harmony_core.bt_codes` / `keys` (needs no hub): every built-in command name with its HID code (`default`: part of
  the default command set), the default usage -> key map of a remote (`name` is the built-in whose code it is), and the key
  names a remote's `keys` may map to (`key_<n>`, n < 128, also works).
- `state` (in `bt:<id>`, `bt_state`) is the hub's `bt_state`, cleaned: `{state, discoverable, discoverable_secs?, le?, gen?, window?,
  pairing?: {result, addr?, name?, transport?, kind?}, hosts: [{slot, addr, name?, transport, connected, configured?}],
  remotes: [{slot, addr, name?, connected, battery?, configured?}], candidates?: [{addr, name?, rssi}]}`, addresses upper-case;
  `null` when none is known. `limits` is `hello_ack.bt`: `{hosts_max, conn_max, remotes_max, le_bonds_max, pages, le}`.
- Commands, each `{hub?, ...}`, answered `200 {ok: true, state}` once `bt_result` arrives (up to 20 s; the pairing result itself
  arrives later in the `bt:<id>` topic):
  - `POST bt/pair` `{mode?: "host"|"remote" (host), transport?: "any"|"classic"|"le" (any), secs?: 1..300 (120)}`.
    Remote mode scans: found devices appear as `state.candidates`; nothing is bonded until `bt/pick`.
  - `POST bt/pair/stop` `{}`.
  - `POST bt/pick` `{addr}` bonds one candidate (`AA:BB:CC:DD:EE:FF`, either case).
  - `POST bt/forget` `{slot: 0..254, kind?: "host"|"remote" (host)}` (a slot of the latest `state`).
  - `POST bt/key` `{slot: 0..254, page: 7|12, usage: 0..65535, action: "down"|"up"|"press"}` test-fires a key at a paired host.
  - Errors: `400 bad_request` (schema), `422 invalid_bt` (`error.bt_code`, e.g. a slot beyond the hub's limits, `no_le`), `409
    bt_unknown | hub_offline | bt_unsupported`, `429 bt_busy` (too many waiting), `502 hub_refused` (`error.hub_code` is the hub's
    code: `no_candidate`, `no_slot`, `bt_not_ready`, `bt_busy`, ...; `msg` a sentence), `504 hub_timeout` (no `bt_result` in 20 s; a
    retry with the same `cmd_id` gets its result).
- `POST bt/listen` is removed: listening is the `keys:<id>` subscription (see Live).
- Messages for the channel are as listed under Channel; `bt_result` carries `{id, ok, code?, msg?, state?}` with the same
  `code`/`msg` the old `results` entries had.

### Cadence

None. Nothing polls and nothing renews: the integration and editor hold WebSockets, state changes are pushed, and a dead peer is
found by TCP keepalive (60 s idle, 10 s interval, 3 probes), then reconnected with back-off and resynced from `hello` /
`snapshot`.

### Implementer notes (routes)

- Remove `GET config/status`, `GET bt/status`, `GET bt/events`, `POST bt/listen`, `GET bundle`, `GET bundle/wait`, `POST bundle/ack`,
  `POST bundle/bt` and their schemas/constants (`WAIT_SECONDS`, `BT_POLL_*`, `fast`).
- Add `GET /api/v1/channel` (bearer) and `GET /api/v1/live` (ingress). `tests/py/test_addon_api_doc.py` and `test_addon.py`
  classify auth by `path.startswith("/api/v1/bundle")` / `_is_bundle`: change both to `/api/v1/channel`. `/health.capabilities`
  gains `"channel": 1`.

## Decisions

- Errors keep the nested envelope `{error:{code,msg,...}}`: 503 `{error:{code:"ha_unavailable"}}`; 409
  draft conflict `{error:{code:"conflict",msg,rev}}`; 429 `{error:{code:"rate_limited",msg,retry_after_ms}}`
  plus a `Retry-After` header.
- Draft rev (`draft_rev`) is an integer counter per config, returned by the draft, import, restore and revision calls; `If-Match`
  takes `3`, `"3"` or `W/"3"`. It is separate from the numbered revisions (`rev`) of a config.
- `GET /ha/entities` returns `{entities:[{entity_id,name,domain,device_class?,area_id?,device_id?,platform?,label_ids?,diag?,state?}],areas,devices,labels,registries,total}` and
  `GET /ha/services` returns `{services:[{domain,service,name,fields:[...]}]}`. Both reject repeated
  query params.
- Validation: `errors:[{path:[...],msg,line:null}]` (up to 100). `POST validate?all=0` gives the first
  error only. `harmony_core.config_model.validate_all()` / `validate_data()` are new; `parse_model` is
  unchanged and still fail-fast.
- `GET /hubs` returns `{hubs:[{id,has_applied,config_id,config_name,rev,applied_rev,applied_at}]}`. Here `rev` is the library revision, `applied_rev`
  and the status endpoint's `applied_rev` are the bundle rev (content hash), the same string the
  integration acks.
- Acks travel as `ack` messages on the channel (see Channel). The add-on answers an `ack` for a rev that is not the applied one with `error stale_rev`.
- Config status is the `hub:<id>` topic of `/live` (fields under Live).
- IR routes: brands `{brands:[{name,slug,count}]}` at most 50; models `{brand,total,offset,limit,
  models:[{file,name,id}]}` limit 1-200; device `{device,forms,notes,skipped:[{name,reason}],
  skipped_total,device_type:{id,name}|null,source}`; candidates `{candidates:[{kind,action,form,count,
  models,model_files,codesets}],examined,matched,errors:<count>}`. `brand` is the slug from `/brands` and must be a
  known brand; `model` is the `file` from `/models`, ending in `.json`. Archive failures: 504
  `archive_offline` or `archive_timeout`, 502 `archive_error`, 404 `unknown_brand`. An offline archive
  with a cache still answers.
- `POST /ir/parse` returns `{ok,actions:[{name,action,form}],skipped}` (at most 4096 actions). Bad input
  gives 422 `bad_ir` with `error.errors:[{path,msg}]`; a Flipper file with an unsupported protocol
  fails whole with 422.
- `POST /ir/test` takes `{hub,ir,port_mask?}` and returns `{ok,action}`. Invalid IR is 422 `invalid_ir`
  and does not use up the rate budget; a refusal from Home Assistant is 502 `send_failed` with
  `http_status`. The service call is exactly `POST /services/ha_harmony/send_ir` with
  `{config_entry:<hub id>,ir}`.
