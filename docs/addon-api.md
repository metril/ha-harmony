# Harmony Editor add-on: HTTP API

The contract for the editor frontend (and the integration's bundle feed). Source of truth is the
code in `harmony_addon/`; `tests/py/test_addon_api_doc.py` fails if a served route is missing from
the table below.

## Conventions

- **Ingress** routes are reachable only through Home Assistant ingress (the Supervisor gateway
  peer). Pages additionally need `X-Ingress-Path`. **Bearer** routes (`/api/v1/bundle*`) need
  `Authorization: Bearer <token>` from the Supervisor network, and are refused via ingress.
- Every ingress request that changes state (POST/PUT) needs `Content-Type: application/json`
  (`text/yaml` or `application/yaml` for `import_yaml`), the header `X-HH-CSRF: 1`, and a same-origin
  `Origin`/`Sec-Fetch-Site`; otherwise `403 csrf`. Bearer POSTs need only the JSON content type.
- Bodies and query strings are schema-validated, unknown keys and repeated query params are
  `400 bad_request`. Request bodies are at most 1 MiB. `hub` defaults to `default` and must match
  `[A-Za-z0-9][A-Za-z0-9._-]{0,62}`.
- Errors always look like `{"error": {"code": "...", "msg": "...", ...extra}}`. Responses to `/api/`
  carry `Cache-Control: no-store`.
- Validation entries are `{path: [..], msg: str, line: null}`; at most 100 are returned.

## Routes

| Method | Path | Auth | Summary |
| --- | --- | --- | --- |
| GET | `/` | ingress | The editor UI (`index.html`; placeholder page when the frontend is not built) |
| GET | `/assets` | ingress | Hashed UI assets under /assets/… (immutable cache; only present when the frontend is built) |
| GET | `/api/v1/health` | ingress | Versions and capabilities |
| GET | `/api/v1/config` | ingress | Draft, applied metadata, history, draft `rev` |
| GET | `/api/v1/config/history` | ingress | Source config of one applied or history version |
| PUT | `/api/v1/config/draft` | ingress | Store the draft (honours `If-Match`) |
| POST | `/api/v1/config/validate` | ingress | All validation errors |
| POST | `/api/v1/config/preview` | ingress | Decoded pages, usage meters; error tolerant |
| POST | `/api/v1/config/keymap` | ingress | Effective hard-key action per layer |
| POST | `/api/v1/config/apply` | ingress | Make the draft the applied config (strict) |
| POST | `/api/v1/config/revert` | ingress | Discard the draft, or re-apply the previous version |
| POST | `/api/v1/config/import_yaml` | ingress | Upload YAML as the draft |
| GET | `/api/v1/config/export_yaml` | ingress | Draft or applied config as YAML |
| GET | `/api/v1/config/status` | ingress | Did the integration apply it? |
| GET | `/api/v1/hubs` | ingress | Hubs known to the store |
| GET | `/api/v1/ha/entities` | ingress | Home Assistant entities for pickers |
| GET | `/api/v1/ha/services` | ingress | Home Assistant services for pickers |
| POST | `/api/v1/ir/test` | ingress | Fire one IR code through `ha_harmony.send_ir` |
| GET | `/api/v1/ir/brands` | ingress | Brand search |
| GET | `/api/v1/ir/models` | ingress | Models of a brand (paged) |
| GET | `/api/v1/ir/device` | ingress | A converted device block |
| GET | `/api/v1/ir/candidates` | ingress | Power code candidates |
| POST | `/api/v1/ir/parse` | ingress | Parse pasted Pronto / Flipper / SmartIR |
| GET | `/api/v1/bundle` | bearer | The applied config for the integration |
| GET | `/api/v1/bundle/wait` | bearer | Long poll for a newer bundle |
| POST | `/api/v1/bundle/ack` | bearer | The integration reports what it did |

Query/body details follow. `hub` is a query parameter wherever a hub applies.

### Health

`GET health` -> `{ok, addon_version, core_version, bundle_schema, capabilities: {ha_actions: bool,
page_format: int}}`. `capabilities` says what this build can store: `ha_actions` is true when the
config schema accepts a Home Assistant service action (`ha: {service, target: {entity_id}, ...}`),
probed from the schema itself; `page_format` is the page format the compiler and editor speak. The
editor disables the Home Assistant action picker (with a reason) when `ha_actions` is false.

### Config

- `GET config?hub=&full=0|1` -> `{hub, draft: {config, updated}|null, applied: {rev, at, hash}|null,
  history: [{rev, at, hash}], history_max, rev}`. `full=1` returns `applied` with its `config`.
  `rev` is the integer **draft revision**; it is separate from `applied.rev`.
- `GET config/history?hub=&rev=<int>` -> `{hub, rev, at, hash, config}` for the applied version or one
  history row (the integer `rev` those rows carry); `404 unknown_rev`. Used for "restore as draft" and diffs.
- `PUT config/draft?hub=` body `{config}`; optional `If-Match: 3` (also `"3"`, `W/"3"`). Stale ->
  `409 conflict` with `error.rev` set to the current revision. No header = unconditional.
  -> `{hub, draft_updated, applied, history, history_max, rev}`. Import and revert also bump `rev`.
- `POST config/validate?hub=&all=1|0` body `{config?}` (default: the draft) -> `{ok, errors}`;
  `all=0` returns only the first error.
- `POST config/apply?hub=` body `{}` -> `{ok:true, ...meta}`; `409 no_draft`; `422 {ok:false, errors}`.
  Apply is strict; the preview's tolerance does not apply to it.
- `POST config/revert?hub=` body `{target: "applied"|"previous"}` -> `{ok, ...meta}`; `409 store`
  when there is no previous version.
- `POST config/import_yaml?hub=` YAML body (max 1 MB, no anchors) -> `{ok, hub, draft_updated, ...meta}`;
  `422 ir_files_unresolvable | invalid_config`; `400 bad_yaml | yaml_alias | too_large`. Honours `If-Match`.
- `GET config/export_yaml?hub=&source=draft|applied` -> `text/yaml`; `404 empty`.
- `editor:` is an optional top-level mapping of opaque editor metadata (depth <= 8, <= 5000 nodes,
  <= 64 KiB, plain JSON). It is stored, exported and imported with the config, ignored by the
  compiler, and **stripped from the bundle**, so editor-only changes never change the bundle `rev` or
  reach the hub.

### Preview

`POST config/preview?hub=` body `{config?}` (without `config` it uses the saved draft: `409 no_draft` when there is none, so send the applied config to preview that) always answers `200`, with every page that compiles even
when the draft has errors elsewhere:

```
{ok, hash|null, errors: [{path,msg,line}],
 missing_pages: [{index, id}],
 pages: [{index, id, ok, error|null, page|null, wire_pages: [...], bytes|null, bytes_v1|null, limit,
          widgets|null, widget_limit}],
 usage: {json_bytes, json_max, pool_words, pool_max, keys, keys_max, devices, devices_max,
         activities, activities_max, commands, commands_max, inputs, inputs_max,
         soft_keys, soft_keys_max} | null}
```

`pages` has one entry per **config** page. `page` is the page-format-v2 model of its first wire page
(`harmony_core.page_codec.page_to_dict`, the editor's rendering contract: `version, id, cols, rows,
gen, total, kind, back, seq, act, title, widgets: [{kind, id, x, y, w, h, label, role, shape,
icon_only, icon, bind}]`; roles, shapes, icons and nav binds are already resolved). `wire_pages` lists
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

`POST config/keymap?hub=` body `{config?}` -> `{ok:true, default: [row], activities: [{id, name, keys: [row]}]}`
with one row per hard key (35, ordered by code): `{key, code, source: "default"|"activity"|"none",
action: null | {kind, label, ...}}`. Lookup is the hub's: activity keys, then the default layer, else
none (`source:"none"`, `action:null`). Default-layer rows use `source:"default"`. `action.kind` is
`device` (`device`, `command` or `input`), `activity` (`activity`), `ha` (`event`), `bt` (`usage`),
`ir` (`form`) or `none`. Invalid config -> `422 {ok:false, errors}`.

### Status and hubs

- `GET config/status?hub=` -> `{hub, applied_rev, acked_rev, result: "ok"|"error"|null, detail, acked_at,
  linked, last_seen}`. `applied_rev` is the bundle rev (a content hash). `linked` is true once any
  authenticated bundle request was seen since the add-on started.
- `GET hubs` -> `{hubs: [{id, has_draft, has_applied, applied_rev, applied_at}]}`.

### Home Assistant (read only)

- `GET ha/entities?domain=&q=` -> `{entities: [{entity_id, name, domain, state?, device_class?}]}`,
  sorted by domain then name; `domain` must be on the allow-list, `q` at most 64 characters.
- `GET ha/services?domain=` -> `{services: [{domain, service, name, fields: [str]}]}`.
- Both: `503 ha_unavailable` when Home Assistant cannot be read (no token, network, bad answer).

### IR database

- `GET ir/brands?q=` -> `{brands: [{name, slug, count}]}` (at most 50; empty `q` = most models first).
- `GET ir/models?brand=<slug>&q=&offset=0&limit=50` -> `{brand: {name, slug}, total, offset, limit,
  models: [{file, name, id}]}`; `limit` 1..200.
- `GET ir/device?brand=<slug>&model=<file.json>` -> `{device, forms: {command: "proto"|"pronto"},
  notes, skipped: [{name, reason}], skipped_total, device_type: {id, name}|null, source}`; `device` is a
  config device block.
- `GET ir/candidates?brand=&type=<tv|number|name>&limit=60` (limit 1..200) -> `{candidates: [{kind, action,
  form, count, models, codesets}], examined, matched, errors, truncated}`; `truncated` is true when the
  per-request budget (16 MiB of codeset text) ran out before every codeset was looked at.
- `POST ir/parse` body `{kind: "pronto"|"flipper"|"smartir", text (<= 256 KiB)}` ->
  `{ok, actions: [{name, action, form}], skipped: [{name, reason}]}` (at most 256 actions);
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

### Bundle (integration)

- `GET bundle?hub=` -> `{schema, core_version, hub_id, applied_at, rev, config}` with `ETag`;
  `If-None-Match` -> `304`; `404 nothing_applied`. `config` never contains `editor`.
- `GET bundle/wait?hub=&rev=` -> the bundle when its rev differs from `rev`, else `204` after about
  55 s (or at once on shutdown).
- `POST bundle/ack` body `{hub, rev, result: "ok"|"error", detail?: str (<= 200)}` -> `{ok:true}`;
  `404 nothing_applied`; `409 stale_rev` (with the current `rev`) when `rev` is not the applied one.
- Failed bearer attempts are rate limited (`429`); a valid token never is. `401 unauthorized` for
  every other miss.

## Decisions

- Errors keep the nested envelope `{error:{code,msg,...}}`: 503 `{error:{code:"ha_unavailable"}}`; 409
  draft conflict `{error:{code:"conflict",msg,rev}}`; 429 `{error:{code:"rate_limited",msg,retry_after_ms}}`
  plus a `Retry-After` header.
- Draft rev is an integer counter. It appears as top-level `rev` in GET config and in the PUT, apply,
  revert and import responses; `If-Match` takes `3`, `"3"` or `W/"3"`. `rev` is separate from `applied.rev`.
- `GET /ha/entities` returns `{entities:[{entity_id,name,domain,state?,device_class?}]}` and
  `GET /ha/services` returns `{services:[{domain,service,name,fields:[...]}]}`. Both reject repeated
  query params.
- Validation: `errors:[{path:[...],msg,line:null}]` (up to 100). `POST validate?all=0` gives the first
  error only. `harmony_core.config_model.validate_all()` / `validate_data()` are new; `parse_model` is
  unchanged and still fail-fast.
- `GET /hubs` returns `{hubs:[{id,has_draft,has_applied,applied_rev,applied_at}]}`. Here `applied_rev`
  and the status endpoint's `applied_rev` are the bundle rev (content hash), the same string the
  integration acks.
- `POST /bundle/ack` takes `{hub,rev,result:"ok"|"error",detail?<=200}`. It is bearer-only with no
  CSRF header; the content type must still be JSON. It returns 404 if the hub has nothing applied and 409
  `stale_rev` if `rev` is not the currently applied one.
- `GET /config/status?hub=` returns `{hub,applied_rev,acked_rev,result,detail,acked_at,linked,last_seen}`.
  `linked` is process-lifetime.
- IR routes: brands `{brands:[{name,slug,count}]}` at most 50; models `{brand,total,offset,limit,
  models:[{file,name,id}]}` limit 1-200; device `{device,forms,notes,skipped:[{name,reason}],
  skipped_total,device_type:{id,name}|null,source}`; candidates `{candidates:[{kind,action,form,count,
  models,codesets}],examined,matched,errors:<count>}`. `brand` is the slug from `/brands` and must be a
  known brand; `model` is the `file` from `/models`, ending in `.json`. Archive failures: 504
  `archive_offline` or `archive_timeout`, 502 `archive_error`, 404 `unknown_brand`. An offline archive
  with a cache still answers.
- `POST /ir/parse` returns `{ok,actions:[{name,action,form}],skipped}` (at most 256 actions). Bad input
  gives 422 `bad_ir` with `error.errors:[{path,msg}]`; a Flipper file with an unsupported protocol
  fails whole with 422.
- `POST /ir/test` takes `{hub,ir,port_mask?}` and returns `{ok,action}`. Invalid IR is 422 `invalid_ir`
  and does not use up the rate budget; a refusal from Home Assistant is 502 `send_failed` with
  `http_status`. The service call is exactly `POST /services/ha_harmony/send_ir` with
  `{config_entry:<hub id>,ir}`.
