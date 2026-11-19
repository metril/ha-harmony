"""aiohttp app factory and routes."""

from __future__ import annotations

import asyncio
import hashlib
import html
import json
import logging
import re
import time
from collections.abc import Callable, Collection
from pathlib import Path
from typing import Any
from urllib.parse import unquote, urlsplit

import aiohttp
import voluptuous as vol
import yaml
from aiohttp import web

import harmony_core
from harmony_core.compiler import (
    CompileError,
    compile_config,
)
from harmony_core.config_model import (
    ConfigError,
    check_yaml_depth,
    load_source,
    parse_data,
    validate_data,
    validate_ir,
    validate_pages,
)
from harmony_core.hub_limits import LEGACY, LIMITS_HEADER, HubLimits
from harmony_core.keymap import resolve_keymaps
from harmony_core.page_codec import MAX_BYTES, MAX_WIDGETS, page_to_dict
from harmony_irdb.archive import IrdbArchive

from . import __version__, ir_routes, schemas
from .auth import (
    BUNDLE_PREFIX,
    HASSIO_NETWORK,
    Auth,
    BundleAuth,
    IngressAuth,
    RouteAuth,
    load_or_create_token,
)
from .ha_client import Catalog, HaApi, HaUnavailable
from .ir_tester import IrSender, SendLimiter, SendRefused
from .linked import HW_ID, LinkedHubs, clean_name
from .options import Options, load_options
from .store import HISTORY_MAX, HUB_ID, RevConflict, Store, StoreError
from .supervisor import Discovery
from .web_util import CLOCK_KEY, error, jresp, query, read_body

log = logging.getLogger(__name__)

MAX_BODY = 4 * 1024 * 1024  # a config as large as a hub takes (its memory budget) with headroom
MUTATING = {"POST", "PUT", "PATCH", "DELETE"}
JSON_TYPES = {"application/json"}
YAML_TYPES = {"text/yaml", "application/yaml"}
CSP = (
    "default-src 'none'; base-uri 'none'; form-action 'none'; "
    "frame-ancestors 'self'; img-src 'self' data:; style-src 'self'; script-src 'self'; "
    "connect-src 'self'"
)
CSRF_HEADER = "X-HH-CSRF"

OPTIONS_KEY = web.AppKey("options", Options)
STORE_KEY = web.AppKey("store", Store)
NOTIFIER_KEY = web.AppKey("notifier", object)
WAIT_KEY = web.AppKey("wait_s", float)
CATALOG_KEY = web.AppKey("catalog", Catalog)
SEEN_KEY = web.AppKey("bundle_seen", dict)
LINKED_KEY = web.AppKey("linked_hubs", LinkedHubs)
HW_HEADER = "X-Hub-Hw"  # a poll names which hub it is: stable hardware id, previous id, display name (docs/addon-api.md)
PREV_HEADER = "X-Hub-Prev"
NAME_HEADER = "X-Hub-Name"
ACKS_KEY = web.AppKey("acks", dict)
MEMO_KEY = web.AppKey("preview_memo", dict)
SENDER_KEY = web.AppKey("ir_sender", object)
LIMITER_KEY = web.AppKey("ir_limiter", SendLimiter)
WAIT_SECONDS = 55.0


# ------------------------------------------------------------------ middlewares
def _same_origin(request: web.Request) -> bool:
    if request.headers.get("Sec-Fetch-Site", "same-origin") not in ("same-origin", "none"):
        return False
    origin = request.headers.get("Origin")
    if origin is None:
        return True
    host = urlsplit(origin).netloc
    allowed = {request.headers.get("Host"), request.headers.get("X-Forwarded-Host")}
    return bool(host) and host in allowed


@web.middleware
async def headers_mw(request: web.Request, handler):
    try:
        resp = await handler(request)
    except web.HTTPException as exc:
        if exc.status < 400 or exc.content_type == "application/json":
            _harden(request, exc)
            raise
        # aiohttp's own 404/405/413...: replace the text body with a fixed JSON one
        resp = web.json_response(
            {"error": {"code": "http_error", "msg": exc.reason}}, status=exc.status
        )
    _harden(request, resp)
    return resp


def _harden(request: web.Request, resp: web.StreamResponse) -> None:
    resp.headers["Content-Security-Policy"] = CSP
    resp.headers["X-Content-Type-Options"] = "nosniff"
    resp.headers["Referrer-Policy"] = "no-referrer"
    if (adopt := request.get("adopt")) is not None:  # the answer to a poll that named a renamed hub
        resp.headers["X-HH-Adopt"] = adopt
    if request.path.startswith("/api/"):
        resp.headers["Cache-Control"] = "no-store"
    elif request.path.startswith("/assets/") and resp.status == 200:
        # build output is content-hashed (vite), so it never changes under one name
        resp.headers["Cache-Control"] = "public, max-age=31536000, immutable"
    elif request.path == "/" and resp.status == 200:
        resp.headers["Cache-Control"] = "no-cache"


def make_auth_mw(auth: Auth):
    @web.middleware
    async def auth_mw(request: web.Request, handler):
        try:
            auth.check(request)
        except web.HTTPForbidden:
            raise error(403, "forbidden", "forbidden") from None
        except web.HTTPUnauthorized:
            raise error(401, "unauthorized", "unauthorized") from None
        except web.HTTPTooManyRequests:
            raise error(
                429, "rate_limited", "too many attempts", headers={"Retry-After": "60"}
            ) from None
        bundle = request.path == BUNDLE_PREFIX or request.path.startswith(BUNDLE_PREFIX + "/")
        if bundle:  # authenticated (auth.check passed): remember that the integration is linked
            request.app[SEEN_KEY]["last_seen"] = int(request.app[CLOCK_KEY]())
            # only a poll names its hub in the query (an ack carries it in the body): no fallback
            hub = request.query.get("hub")
            polling = request.method == "GET" and request.path in (BUNDLE_PREFIX, BUNDLE_PREFIX + "/wait")
            if polling and hub:
                linked = request.app[LINKED_KEY]
                hw = request.headers.get(HW_HEADER)
                prev = request.headers.get(PREV_HEADER)
                raw_name = request.headers.get(NAME_HEADER)
                new = linked.seen(hub)
                if prev is not None or hw is not None:
                    request["adopt"] = _adopt_renamed(request.app, hub, prev, hw)
                linked.note_identity(hub, hw, None if raw_name is None else clean_name(unquote(raw_name)))
                if new:
                    _auto_adopt(request.app)
                raw_limits = request.headers.get(LIMITS_HEADER)
                if raw_limits is not None and len(raw_limits) <= 2000:  # the hub's own limits, as its integration knows them
                    try:
                        linked.set_limits(hub, json.loads(raw_limits))
                    except ValueError:
                        pass
        if request.method in MUTATING:
            if bundle:  # bearer-authenticated machine client: no ambient credential, so no CSRF
                if request.content_type not in JSON_TYPES:
                    raise error(403, "csrf", "forbidden")
            else:
                allowed = YAML_TYPES if request.path == "/api/v1/config/import_yaml" else JSON_TYPES
                if (
                    request.content_type not in allowed
                    or request.headers.get(CSRF_HEADER) != "1"
                    or not _same_origin(request)
                ):
                    raise error(403, "csrf", "forbidden")
        return await handler(request)

    return auth_mw


# ------------------------------------------------------------------ helpers
def _adopt_renamed(app: web.Application, hub: str, prev: str | None, hw: str | None) -> str:
    """The integration polls for `hub` and says which hub that is: its previous id (`prev`) and/or its stable
    hardware id (`hw`). When that hub is one the add-on knows under another id, its store (draft, applied
    revisions, history, ack) moves to `hub` in one atomic rename. Returns the answer for X-HH-Adopt:
    `moved`, `exists` (a store under `hub` already exists: nothing is overwritten) or `none`."""
    linked, store = app[LINKED_KEY], app[STORE_KEY]
    if hw is not None and not HW_ID.fullmatch(hw):
        hw = None
    sources: list[str] = []
    if prev and prev != hub and HUB_ID.fullmatch(prev) and prev != "default":
        sources.append(prev)
    if hw:
        sources += [h for h in linked.ids() if h != hub and h != "default" and linked.hw(h) == hw and h not in sources]
    try:
        for src in sources:
            known = linked.hw(src)
            if hw and known and known != hw:
                continue  # that id is another hub
            if src not in store.hubs():
                continue
            if hub in store.hubs():
                log.warning("hub %s was renamed to %s but %s already has a config: nothing moved", src, hub, hub)
                return "exists"
            if not store.adopt(src, hub):
                continue
            acks = app[ACKS_KEY]
            if src in acks:
                acks[hub] = acks.pop(src)
            linked.rename(src, hub)
            app[NOTIFIER_KEY].notify(hub)
            log.info("moved the config of hub %s to %s (the hub was renamed)", src, hub)
            return "moved"
    except OSError, StoreError:
        log.exception("could not move the config of a renamed hub to %s", hub)
        return "none"
    if prev and prev != hub and prev in linked.ids() and (not hw or linked.hw(prev) in (None, hw)):
        linked.rename(prev, hub)  # no config under the old id: just stop listing it as a hub that is polled
    return "none"


def _auto_adopt(app: web.Application) -> None:
    """A store that exists only under `default` moves to the one hub id an integration polls for."""
    linked = app[LINKED_KEY].ids()
    ids = [h for h in linked if h != "default"]
    if len(ids) != 1 or "default" in linked:  # something polls for `default` itself: it is in use
        return
    try:
        moved = app[STORE_KEY].adopt("default", ids[0])
    except OSError, StoreError:
        log.exception("could not move the config of hub default to %s", ids[0])
        return
    if moved:
        log.info("moved the config of hub default to %s (the id Home Assistant polls for)", ids[0])
        app[NOTIFIER_KEY].notify(ids[0])


MAX_ERRORS = 100


def _check(config: dict[str, Any], *, all_errors: bool = True) -> tuple[Any, list[dict[str, Any]]]:
    """(model, []) or (None, [{path, msg, line}]). `all_errors=False` keeps the integration's
    fail-fast behaviour (one error). Works on the loaded dict: no YAML pass (about 100x faster)."""
    if not all_errors:
        try:
            return parse_data(config), []
        except ConfigError as exc:
            return None, [_err(exc)]
    errors = validate_data(config)
    if errors:
        return None, [_err(e) for e in errors[:MAX_ERRORS]]
    return parse_data(config), []


def _audit(request: web.Request, action: str, hub: str) -> None:
    """One line per state-changing action: who (the Supervisor-vouched ingress user id) and what.
    No config content, no tokens."""
    log.info("audit user=%s action=%s hub=%s", request.get("user_id", "-"), action, hub)


def _err(exc: ConfigError) -> dict[str, Any]:
    return {"path": list(exc.path), "msg": exc.msg[:300], "line": None}


def _pick_config(state: dict[str, Any], given: dict[str, Any] | None) -> dict[str, Any]:
    if given is not None:
        return given
    if state["draft"] is None:
        raise error(409, "no_draft", "there is no draft; send a config")
    return state["draft"]["config"]


def _meta(state: dict[str, Any]) -> dict[str, Any]:
    a = state["applied"]
    return {
        "applied": None if a is None else {k: a[k] for k in ("rev", "at", "hash")},
        "history": [{k: h[k] for k in ("rev", "at", "hash")} for h in state["history"]],
        "history_max": HISTORY_MAX,
        "rev": state["draft_rev"],
    }


def _if_match(request: web.Request) -> int | None:
    """`If-Match: <rev>` (bare, quoted or weak). Absent = unconditional write."""
    raw = request.headers.get("If-Match")
    if raw is None:
        return None
    m = re.fullmatch(r'(?:W/)?"?(\d{1,12})"?', raw.strip())
    if not m:
        raise error(400, "bad_request", "invalid request: If-Match")
    return int(m.group(1))


def _store_call(fn, *args):
    try:
        return fn(*args)
    except RevConflict as exc:
        raise error(409, "conflict", str(exc), rev=exc.rev) from None
    except StoreError as exc:
        raise error(409, "store", str(exc)) from None
    except OSError:
        log.exception("storage failure")
        raise error(409, "store", "storage failure") from None


# ------------------------------------------------------------------ routes
_HA_ACTION_PROBE = {
    "keys": {"up": {"ha": {"service": "light.toggle", "target": {"entity_id": "light.probe"}}}}
}


def _capabilities() -> dict[str, Any]:
    """What this build can store, probed from the schema itself so it cannot drift: the editor
    disables the Home Assistant action picker when `ha_actions` is false."""
    from harmony_core.config_model import validate_data

    return {
        "ha_actions": not validate_data(_HA_ACTION_PROBE),
        "page_format": 2,
    }


async def health(request: web.Request) -> web.Response:
    return jresp(
        {
            "ok": True,
            "addon_version": __version__,
            "core_version": harmony_core.__version__,
            "bundle_schema": harmony_core.BUNDLE_SCHEMA,
            "capabilities": _capabilities(),
        }
    )


async def get_config(request: web.Request) -> web.Response:
    q = query(request, schemas.GET_CONFIG_QUERY)
    state = _store_call(request.app[STORE_KEY].load, q["hub"])
    out = {"hub": q["hub"], "draft": state["draft"], **_meta(state)}
    if q["full"] == "1" and state["applied"]:
        out["applied"] = state["applied"]
    return jresp(out)


async def get_history_entry(request: web.Request) -> web.Response:
    """The source config of one applied version (the applied one or a history row), by its rev."""
    q = query(request, schemas.HISTORY_QUERY)
    state = _store_call(request.app[STORE_KEY].load, q["hub"])
    rev = int(q["rev"])
    rows = [state["applied"], *state["history"]]
    row = next((r for r in rows if r is not None and r["rev"] == rev), None)
    if row is None:
        raise error(404, "unknown_rev", "no applied or history version has that rev")
    return jresp({"hub": q["hub"], **{k: row[k] for k in ("rev", "at", "hash", "config")}})


async def put_draft(request: web.Request) -> web.Response:
    q = query(request, schemas.QUERY)
    body = await read_body(request, schemas.PUT_DRAFT)
    now = int(request.app[CLOCK_KEY]())
    state = _store_call(
        request.app[STORE_KEY].set_draft, q["hub"], body["config"], now, _if_match(request)
    )
    return jresp({"hub": q["hub"], "draft_updated": now, **_meta(state)})


async def validate(request: web.Request) -> web.Response:
    q = query(request, schemas.VALIDATE_QUERY)
    body = await read_body(request, schemas.OPT_CONFIG)
    state = _store_call(request.app[STORE_KEY].load, q["hub"])
    _, errors = _check(_pick_config(state, body.get("config")), all_errors=q["all"] == "1")
    return jresp({"ok": not errors, "errors": errors})


def _compile(config: dict[str, Any], limits: HubLimits = LEGACY):
    model, errors = _check(config)
    if errors:
        return None, None, errors
    try:
        return model, compile_config(model, limits=limits), []
    except CompileError as exc:
        return None, None, [{"path": [], "msg": str(exc)[:300], "line": None}]


PREVIEW_MEMO_MAX = 8


def _page_error(errors: list[dict[str, Any]], i: int) -> str | None:
    for e in errors:
        if e["path"][:2] == ["pages", i]:
            return e["msg"]
    return None


def _blank(page: dict[str, Any], i: int) -> dict[str, Any]:
    """A page stand-in that always compiles, keeping what other pages may point at."""
    out: dict[str, Any] = {"id": page.get("id", f"_{i}"), "cols": 1, "rows": 1, "widgets": []}
    for k in ("kind", "activity"):
        if k in page:
            out[k] = page[k]
    return out


def _compile_tolerant(
    model: dict[str, Any], problems: list[tuple[list[Any], str]], limits: HubLimits = LEGACY
) -> tuple[Any, set[int]]:
    """Compile in collect mode; pages that fail to build are blanked and the rest recompiled, so
    one bad page never hides the others. Every problem found lands in `problems` (deduplicated)."""
    pages = list(model["pages"])
    blanked: set[int] = set()
    seen: set[tuple[str, str]] = set()
    for _ in range(len(pages) + 1):
        trial: list[tuple[list[Any], str]] = []
        compiled = compile_config({**model, "pages": pages}, collect=trial, limits=limits)
        for path, msg in trial:
            if (json.dumps(path), msg) not in seen:
                seen.add((json.dumps(path), msg))
                problems.append((path, msg))
        new = {
            path[1]
            for path, _ in trial
            if path[:1] == ["pages"] and len(path) > 1 and isinstance(path[1], int)
        } - blanked - set(compiled.page_of)  # pages that built keep their (over-budget) model
        new = {i for i in new if i < len(pages)}
        if not new:
            return compiled, blanked
        for i in new:
            pages[i] = _blank(pages[i], i)
        blanked |= new
    return compiled, blanked


def _wire_entry(m: Any, u: dict[str, Any]) -> dict[str, Any]:
    return {
        "page_id": m.page_id,
        "seq": {"idx": m.seq_idx, "cnt": m.seq_cnt} if m.seq_cnt else None,
        "bytes": u["bytes"],
        "bytes_v1": u["bytes_v1"],
        "widgets": u["widgets"],
        "page": page_to_dict(m),
    }


def _preview(config: dict[str, Any], limits: HubLimits = LEGACY) -> dict[str, Any]:
    """Blocking (CPU). Always answers: pages that compile are returned even when the draft has
    errors elsewhere; apply stays strict because it uses `_compile`, not this."""
    raw_pages = config.get("pages") if isinstance(config.get("pages"), list) else []
    _, errors = _check(config)
    problems: list[tuple[list[Any], str]] = []
    if not errors:
        model = parse_data(config)
        compiled, blanked = _compile_tolerant(model, problems, limits)
        errors = [{"path": p, "msg": m[:300], "line": None} for p, m in problems]
        total_ok = not problems
    else:  # compile what can be compiled: valid pages only, by shape (references unchecked)
        shaped = validate_pages(config)
        for i, (_, errs) in enumerate(shaped):
            have = any(e["path"][:2] == ["pages", i] for e in errors)
            if errs and not have:  # the whole-config pass stopped before reaching this page
                errors.extend(_err(e) for e in errs[: MAX_ERRORS - len(errors)])
        partial = {
            "devices": [],
            "activities": [],
            "keys": {},
            "pages": [
                m if m is not None else _blank(raw_pages[i] if isinstance(raw_pages[i], dict) else {}, i)
                for i, (m, _) in enumerate(shaped)
            ],
        }
        compiled, blanked = _compile_tolerant(partial, problems, limits)
        for path, msg in problems:
            if path[:1] == ["pages"]:
                errors.append({"path": path, "msg": msg[:300], "line": None})
        model, total_ok = None, False
    by_cfg: dict[int, list[dict[str, Any]]] = {}
    for m, cfg, u in zip(compiled.models, compiled.page_of, compiled.usage["page_detail"], strict=True):
        by_cfg.setdefault(cfg, []).append(_wire_entry(m, u))
    pages = []
    for i in range(len(raw_pages)):
        raw = raw_pages[i] if isinstance(raw_pages[i], dict) else {}
        wires = by_cfg.get(i, [])
        err = _page_error(errors, i)
        ok = bool(wires) and err is None
        pid = raw.get("id") if isinstance(raw.get("id"), str) else None
        sized = wires and i not in blanked and (model is not None or ok)  # an over-budget page still reports its size
        pages.append(
            {
                "index": i,
                "id": pid,
                "ok": ok,
                "error": None if ok else (err or "page does not compile"),
                "page": wires[0]["page"] if ok else None,
                "wire_pages": wires if ok else [],
                "bytes": max(w["bytes"] for w in wires) if sized else None,
                "bytes_v1": max(w["bytes_v1"] for w in wires) if sized else None,
                "limit": MAX_BYTES,
                "widgets": len(raw["widgets"]) if isinstance(raw.get("widgets"), list) else None,
                "widget_limit": MAX_WIDGETS,
            }
        )
    usage = {k: v for k, v in compiled.usage.items() if k != "page_detail"} if model is not None else None
    return {
        "ok": not errors,
        "hash": compiled.config_hash if total_ok else None,
        "errors": errors,
        "missing_pages": [{"index": p["index"], "id": p["id"]} for p in pages if not p["ok"]],
        "pages": pages,
        "usage": usage,
    }


async def preview(request: web.Request) -> web.Response:
    q = query(request, schemas.QUERY)
    body = await read_body(request, schemas.OPT_CONFIG)
    state = _store_call(request.app[STORE_KEY].load, q["hub"])
    config = _pick_config(state, body.get("config"))
    # editor metadata cannot change a preview: leave it out of the memo key
    limits = request.app[LINKED_KEY].limits(q["hub"])
    key = hashlib.sha256(
        json.dumps([_public(config), limits.to_wire()], sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest()
    memo: dict[str, dict[str, Any]] = request.app[MEMO_KEY]
    out = memo.pop(key, None)
    if out is None:
        out = await asyncio.get_running_loop().run_in_executor(None, _preview, _public(config), limits)
    memo[key] = out  # most recently used last
    while len(memo) > PREVIEW_MEMO_MAX:
        memo.pop(next(iter(memo)))
    return jresp(out)


def _keymap_sync(config: dict[str, Any]):
    model, errors = _check(config)
    return None if errors else resolve_keymaps(model), errors


async def keymap(request: web.Request) -> web.Response:
    """Per layer, every hard key's effective action, from the one lookup rule in harmony_core."""
    q = query(request, schemas.QUERY)
    body = await read_body(request, schemas.OPT_CONFIG)
    state = _store_call(request.app[STORE_KEY].load, q["hub"])
    config = _pick_config(state, body.get("config"))
    maps, errors = await asyncio.get_running_loop().run_in_executor(None, _keymap_sync, config)
    if errors:
        return jresp({"ok": False, "errors": errors}, 422)
    return jresp({"ok": True, **maps})


async def apply(request: web.Request) -> web.Response:
    q = query(request, schemas.QUERY)
    await read_body(request, schemas.EMPTY)
    store = request.app[STORE_KEY]
    state = _store_call(store.load, q["hub"])
    config = _pick_config(state, None)
    _, compiled, errors = _compile(config, request.app[LINKED_KEY].limits(q["hub"]))
    if errors:
        return jresp({"ok": False, "errors": errors}, 422)
    now = int(request.app[CLOCK_KEY]())
    state = _store_call(store.apply, q["hub"], config, compiled.config_hash, now)
    request.app[NOTIFIER_KEY].notify(q["hub"])
    _audit(request, "apply", q["hub"])
    return jresp({"ok": True, **_meta(state)})


async def revert(request: web.Request) -> web.Response:
    q = query(request, schemas.QUERY)
    body = await read_body(request, schemas.REVERT)
    state = _store_call(request.app[STORE_KEY].revert, q["hub"], body["target"])
    if body["target"] == "previous":  # discarding a draft changes nothing the integration sees
        request.app[NOTIFIER_KEY].notify(q["hub"])
    _audit(request, f"revert:{body['target']}", q["hub"])
    return jresp({"ok": True, **_meta(state)})


def _ir_file_refs(data: Any) -> list[str]:
    found: list[str] = []
    stack = [data]
    while stack and len(found) < 20:
        cur = stack.pop()
        if isinstance(cur, dict):
            for k, v in cur.items():
                if k in ("flipper", "smartir") and isinstance(v, dict) and "file" in v:
                    found.append(f"{k}:{str(v['file'])[:80]}")
                stack.append(v)
        elif isinstance(cur, list):
            stack.extend(cur)
    return found


def _yaml_to_config(raw: bytes) -> dict[str, Any]:
    """YAML upload -> the same plain-JSON config a PUT /draft would carry, or an HTTP error."""
    if len(raw) > schemas.YAML_MAX:
        raise error(400, "too_large", "YAML is too large")
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError:
        raise error(400, "bad_yaml", "YAML must be UTF-8") from None
    try:
        check_yaml_depth(text)
    except ConfigError as exc:
        raise error(400, "bad_yaml", exc.msg[:200]) from None
    try:
        if any(isinstance(e, yaml.AliasEvent) for e in yaml.parse(text, Loader=yaml.SafeLoader)):
            raise error(400, "yaml_alias", "YAML anchors and aliases are not accepted")
    except yaml.YAMLError, RecursionError:
        pass  # load_source reports the syntax / nesting error below
    try:
        loaded = load_source(text)
    except ConfigError as exc:
        raise error(400, "bad_yaml", exc.msg[:200]) from None
    try:  # non-JSON YAML types (timestamps, sets, ...) and NaN are refused, int keys become str
        data = json.loads(json.dumps({} if loaded is None else loaded, allow_nan=False))
    except TypeError, ValueError, RecursionError:
        raise error(400, "bad_yaml", "YAML holds values a config cannot contain") from None
    try:
        return schemas.bounded_config(data)
    except vol.Invalid as exc:
        raise error(400, "bad_request", f"YAML rejected: {exc.error_message}"[:100]) from None


async def import_yaml(request: web.Request) -> web.Response:
    q = query(request, schemas.QUERY)
    if_rev = _if_match(request)
    config = _yaml_to_config(await request.read())
    refs = _ir_file_refs(config)
    if refs:
        raise error(
            422,
            "ir_files_unresolvable",
            "IR file references cannot be resolved here; inline the codes or import them in the "
            "editor",
            files=refs,
        )
    _, errors = _check(config)
    if errors:
        raise error(422, "invalid_config", "the YAML is not a valid config", errors=errors)
    now = int(request.app[CLOCK_KEY]())
    state = _store_call(request.app[STORE_KEY].set_draft, q["hub"], config, now, if_rev)
    _audit(request, "import_yaml", q["hub"])
    return jresp({"ok": True, "hub": q["hub"], "draft_updated": now, **_meta(state)})


async def export_yaml(request: web.Request) -> web.Response:
    q = query(request, schemas.EXPORT_QUERY)
    state = _store_call(request.app[STORE_KEY].load, q["hub"])
    src = q.get("source") or ("draft" if state["draft"] else "applied")
    entry = state[src]
    if entry is None:
        raise error(404, "empty", f"there is no {src} config")
    text = yaml.safe_dump(entry["config"], sort_keys=False, allow_unicode=True)
    return web.Response(text=text, content_type="text/yaml", charset="utf-8")


# ------------------------------------------------------------------ Home Assistant catalog (GET only)
def _ha_unavailable() -> web.HTTPException:
    return error(503, "ha_unavailable", "Home Assistant is not reachable from the add-on")


async def ha_entities(request: web.Request) -> web.Response:
    q = query(request, schemas.HA_ENTITIES_QUERY)
    try:
        rows = await request.app[CATALOG_KEY].entities(q.get("domain"), q["q"])
    except HaUnavailable:
        raise _ha_unavailable() from None
    return jresp({"entities": rows})


async def ha_services(request: web.Request) -> web.Response:
    q = query(request, schemas.HA_SERVICES_QUERY)
    try:
        rows = await request.app[CATALOG_KEY].services(q.get("domain"))
    except HaUnavailable:
        raise _ha_unavailable() from None
    return jresp({"services": rows})


# ------------------------------------------------------------------ IR test-fire
async def ir_test(request: web.Request) -> web.Response:
    """Fire one inline IR code via Home Assistant's own `ha_harmony.send_ir` service."""
    body = await read_body(request, schemas.TEST_IR)
    ir = dict(body["ir"])
    if "port_mask" in body:
        if "port_mask" in ir:
            raise error(400, "bad_request", "invalid request: port_mask")
        ir["port_mask"] = body["port_mask"]
    try:  # no base dir: flipper/smartir file references are refused, sizes are checked
        action = validate_ir(ir)
    except vol.Invalid as exc:
        msg = exc.error_message[:200]
        raise error(422, "invalid_ir", msg, errors=[{"path": [], "msg": msg}]) from None
    sender = request.app[SENDER_KEY]
    if sender is None:
        raise _ha_unavailable()
    wait_ms = request.app[LIMITER_KEY].acquire()
    if wait_ms is not None:
        raise error(
            429,
            "rate_limited",
            "slow down: test sends are limited",
            headers={"Retry-After": str(-(-wait_ms // 1000))},
            retry_after_ms=wait_ms,
        )
    try:
        await sender.send(body["hub"], action)
    except SendRefused as exc:
        raise error(
            502, "send_failed", "Home Assistant refused the test send", http_status=exc.status
        ) from None
    except HaUnavailable:
        raise _ha_unavailable() from None
    _audit(request, "ir_test", body["hub"])
    return jresp({"ok": True, "action": action})


# ------------------------------------------------------------------ hubs and apply status
async def list_hubs(request: web.Request) -> web.Response:
    store = request.app[STORE_KEY]
    out = []
    for hub in store.hubs():
        state = _store_call(store.load, hub)
        a = state["applied"]
        out.append(
            {
                "id": hub,
                "has_draft": state["draft"] is not None,
                "has_applied": a is not None,
                "applied_rev": None if a is None else _rev(a["config"]),
                "applied_at": None if a is None else a["at"],
            }
        )
    linked = request.app[LINKED_KEY]
    have = set(store.hubs())
    targets = [h for h in linked.ids() if h != "default" and h not in have]
    adopt = {"from": "default", "to": targets} if "default" in have and targets else None
    return jresp(
        {"hubs": out, "linked": linked.entries(), "now": int(request.app[CLOCK_KEY]()), "adopt": adopt}
    )


async def adopt_hub(request: web.Request) -> web.Response:
    """Explicit move of a store (default: `default`) to a hub id an integration polls for."""
    body = await read_body(request, schemas.ADOPT)
    store, src, dst = request.app[STORE_KEY], body["from"], body["to"]
    if dst not in request.app[LINKED_KEY].ids():
        raise error(409, "not_linked", "no Home Assistant integration polls for that hub id")
    if dst in store.hubs():
        raise error(409, "hub_exists", "that hub already has a config")
    if not _store_call(store.adopt, src, dst):
        raise error(404, "nothing_to_adopt", "that hub has no config to move")
    request.app[NOTIFIER_KEY].notify(dst)
    _audit(request, f"adopt:{src}", dst)
    return jresp({"ok": True, "hub": dst})


async def config_status(request: web.Request) -> web.Response:
    q = query(request, schemas.QUERY)
    state = _store_call(request.app[STORE_KEY].load, q["hub"])
    bundle = _bundle(q["hub"], state)
    ack = request.app[ACKS_KEY].get(q["hub"]) or state["ack"] or {}
    last_seen = request.app[SEEN_KEY].get("last_seen")
    return jresp(
        {
            "hub": q["hub"],
            "applied_rev": None if bundle is None else bundle["rev"],
            "acked_rev": ack.get("rev"),
            "result": ack.get("result"),
            "detail": ack.get("detail"),
            "acked_at": ack.get("at"),
            "linked": last_seen is not None,
            "last_seen": last_seen,
            "hub_last_seen": request.app[LINKED_KEY].last_seen(q["hub"]),
            "linked_hubs": request.app[LINKED_KEY].entries(),
            "now": int(request.app[CLOCK_KEY]()),
        }
    )


async def post_ack(request: web.Request) -> web.Response:
    """The integration reports what it did with a pulled bundle (bearer auth, like the feed)."""
    body = await read_body(request, schemas.ACK)
    store, hub = request.app[STORE_KEY], body["hub"]
    state = _store_call(store.load, hub)
    bundle = _bundle(hub, state)
    if bundle is None:
        raise _nothing_applied()
    if body["rev"] != bundle["rev"]:  # an answer about an older bundle: not what is applied now
        raise error(409, "stale_rev", "that rev is not the applied one", rev=bundle["rev"])
    ack = {"rev": body["rev"], "result": body["result"], "at": int(request.app[CLOCK_KEY]())}
    if "detail" in body:
        ack["detail"] = body["detail"]
    prev = state["ack"] or {}
    if any(prev.get(k) != ack.get(k) for k in ("rev", "result", "detail")):  # no write per repeat
        _store_call(store.set_ack, hub, ack)
        request.app[ACKS_KEY][hub] = ack
    return jresp({"ok": True})


# ------------------------------------------------------------------ bundle (integration feed)
class ApplyNotifier:
    """Wakes long-poll waiters when a hub's applied config changes."""

    def __init__(self) -> None:
        self._events: dict[str, asyncio.Event] = {}
        self.closing = False

    def event(self, hub: str) -> asyncio.Event:
        if self.closing:
            ev = asyncio.Event()
            ev.set()
            return ev
        return self._events.setdefault(hub, asyncio.Event())

    def close(self) -> None:
        """Shutdown: wake every waiter now (they answer 204) so stop never waits out a poll."""
        self.closing = True
        for hub in list(self._events):
            self.notify(hub)

    def notify(self, hub: str) -> None:
        ev = self._events.pop(hub, None)
        if ev is not None:
            ev.set()


def _public(config: dict[str, Any]) -> dict[str, Any]:
    """What the integration may see: editor-only metadata never leaves the add-on, so editing
    it can neither change the bundle rev nor trigger a hub push (nor break an older integration)."""
    return {k: v for k, v in config.items() if k != "editor"}


def _rev(config: dict[str, Any]) -> str:
    config = _public(config)
    canon = json.dumps(config, sort_keys=True, separators=(",", ":"), ensure_ascii=True)
    return hashlib.sha256(canon.encode()).hexdigest()[:16]


def _bundle(hub: str, state: dict[str, Any]) -> dict[str, Any] | None:
    a = state["applied"]
    if a is None:
        return None
    return {
        "schema": harmony_core.BUNDLE_SCHEMA,
        "core_version": harmony_core.__version__,
        "hub_id": hub,
        "applied_at": a["at"],
        "rev": _rev(a["config"]),
        "config": _public(a["config"]),
    }


def _bundle_response(bundle: dict[str, Any]) -> web.Response:
    return web.json_response(bundle, headers={"ETag": f'"{bundle["rev"]}"'})


def _nothing_applied() -> web.HTTPException:
    return error(404, "nothing_applied", "no config has been applied for this hub")


async def get_bundle(request: web.Request) -> web.Response:
    q = query(request, schemas.QUERY)
    bundle = _bundle(q["hub"], _store_call(request.app[STORE_KEY].load, q["hub"]))
    if bundle is None:
        raise _nothing_applied()
    if request.headers.get("If-None-Match") == f'"{bundle["rev"]}"':
        return web.Response(status=304, headers={"ETag": f'"{bundle["rev"]}"'})
    return _bundle_response(bundle)


async def wait_bundle(request: web.Request) -> web.Response:
    """Long-poll: answer when the applied rev differs from `rev`; 204 after the bounded wait."""
    q = query(request, schemas.WAIT_QUERY)
    app, hub, known = request.app, q["hub"], q["rev"]
    loop = asyncio.get_running_loop()
    deadline = loop.time() + app[WAIT_KEY]
    while True:
        ev = app[NOTIFIER_KEY].event(hub)  # taken before the read: no lost wake-up
        if app[NOTIFIER_KEY].closing:
            return web.Response(status=204)
        bundle = _bundle(hub, _store_call(app[STORE_KEY].load, hub))
        if bundle is not None and bundle["rev"] != known:
            return _bundle_response(bundle)
        if bundle is None and known:
            raise _nothing_applied()
        left = deadline - loop.time()
        if left <= 0:
            return web.Response(status=204)
        try:
            await asyncio.wait_for(ev.wait(), left)
        except TimeoutError:
            return web.Response(status=204)


STATIC_KEY = web.AppKey("static_dir", Path)


async def spa_index(request: web.Request) -> web.StreamResponse:
    # Served as-is: every URL inside is relative, so it works under any ingress prefix.
    return web.FileResponse(request.app[STATIC_KEY] / "index.html")


async def index(request: web.Request) -> web.Response:
    o = request.app[OPTIONS_KEY]
    e = html.escape
    body = (
        '<!doctype html><html lang="en"><meta charset="utf-8">'
        "<title>Harmony editor</title><body><h1>Harmony editor</h1>"
        "<p>The editor UI is not built yet. The API is up.</p>"
        f"<ul><li>add-on {e(__version__)}</li>"
        f"<li>core {e(harmony_core.__version__)}, bundle schema {harmony_core.BUNDLE_SCHEMA}</li>"
        f"<li>log level: {e(o.log_level)}</li></ul></body></html>"
    )
    return web.Response(text=body, content_type="text/html")


def create_app(
    data_dir: Path | str = "/data",
    *,
    options_path: Path | str | None = None,
    options: Options | None = None,
    clock: Callable[[], float] = time.time,
    auth: Auth | None = None,
    bundle_token: str | None = None,
    bundle_networks: Collection[str] | None = (HASSIO_NETWORK,),
    bundle_auth: BundleAuth | None = None,
    discovery: Discovery | None = None,
    wait_seconds: float = WAIT_SECONDS,
    ha_api: HaApi | None = None,
    irdb: IrdbArchive | None = None,
    ir_sender: IrSender | None = None,
    send_clock: Callable[[], float] = time.monotonic,
    irdb_dir: Path | str | None = None,
    static_dir: Path | str | None = None,
) -> web.Application:
    """`auth` guards the ingress routes; the bundle routes always need `bundle_token` (default:
    the 0600 token file under data_dir) from a peer in `bundle_networks` (None: any, for tests)."""
    data_dir = Path(data_dir)
    route_auth = RouteAuth(
        auth or IngressAuth(),
        bundle_auth or BundleAuth(bundle_token or load_or_create_token(data_dir), bundle_networks),
    )
    app = web.Application(
        client_max_size=MAX_BODY,
        middlewares=[headers_mw, make_auth_mw(route_auth)],
    )
    app[OPTIONS_KEY] = options or load_options(
        Path(options_path) if options_path else data_dir / "options.json"
    )
    app[STORE_KEY] = Store(data_dir)
    app[CLOCK_KEY] = clock
    app[NOTIFIER_KEY] = ApplyNotifier()
    app[WAIT_KEY] = wait_seconds
    app[CATALOG_KEY] = Catalog(ha_api, clock)
    app[SEEN_KEY] = {}
    app[LINKED_KEY] = LinkedHubs(data_dir, clock)
    _auto_adopt(app)
    app[MEMO_KEY] = {}
    app[SENDER_KEY] = ir_sender
    app[LIMITER_KEY] = SendLimiter(send_clock)
    if irdb is not None:
        app[ir_routes.IRDB_KEY] = irdb
    else:  # one archive for the process: its own aiohttp session, cache under /data/irdb

        async def _irdb_ctx(app: web.Application):
            session = aiohttp.ClientSession(raise_for_status=False)
            app[ir_routes.IRDB_KEY] = IrdbArchive(session, irdb_dir or data_dir / "irdb")
            yield
            await session.close()

        app.cleanup_ctx.append(_irdb_ctx)
    app[ACKS_KEY] = {}

    async def _wake_waiters(_: web.Application) -> None:
        app[NOTIFIER_KEY].close()

    app.on_shutdown.append(_wake_waiters)
    if discovery is not None:

        async def _announce(_: web.Application) -> None:
            await discovery.announce()

        async def _withdraw(_: web.Application) -> None:
            await discovery.withdraw()

        app.on_startup.append(_announce)
        app.on_shutdown.append(_withdraw)
    api = "/api/v1"
    if static_dir is not None and (Path(static_dir) / "index.html").is_file():
        app[STATIC_KEY] = Path(static_dir)
        app.router.add_get("/", spa_index)
        assets = Path(static_dir) / "assets"
        if assets.is_dir():
            app.router.add_static("/assets", assets, follow_symlinks=False)
    else:
        app.router.add_get("/", index)
    app.router.add_get(f"{api}/health", health)
    app.router.add_get(f"{api}/config", get_config)
    app.router.add_get(f"{api}/config/history", get_history_entry)
    app.router.add_put(f"{api}/config/draft", put_draft)
    app.router.add_post(f"{api}/config/validate", validate)
    app.router.add_post(f"{api}/config/preview", preview)
    app.router.add_post(f"{api}/config/keymap", keymap)
    app.router.add_post(f"{api}/config/apply", apply)
    app.router.add_post(f"{api}/config/revert", revert)
    app.router.add_post(f"{api}/config/import_yaml", import_yaml)
    app.router.add_get(f"{api}/config/export_yaml", export_yaml)
    app.router.add_get(f"{api}/ha/entities", ha_entities)
    app.router.add_get(f"{api}/ha/services", ha_services)
    ir_routes.add_routes(app, api)
    app.router.add_post(f"{api}/ir/test", ir_test)
    app.router.add_get(f"{api}/hubs", list_hubs)
    app.router.add_post(f"{api}/hubs/adopt", adopt_hub)
    app.router.add_get(f"{api}/config/status", config_status)
    app.router.add_post(f"{api}/bundle/ack", post_ack)
    app.router.add_get(f"{api}/bundle", get_bundle)
    app.router.add_get(f"{api}/bundle/wait", wait_bundle)
    return app
