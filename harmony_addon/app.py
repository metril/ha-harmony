"""aiohttp app factory and routes."""

from __future__ import annotations

import asyncio
import dataclasses
import hashlib
import html
import json
import logging
import re
import time
from collections.abc import Callable, Collection
from pathlib import Path
from typing import Any
from urllib.parse import urlsplit

import voluptuous as vol
import yaml
from aiohttp import web

import harmony_core
from harmony_core.compiler import (
    LIMITS,
    CompileError,
    _pool_words,
    compact_json,
    compile_config,
)
from harmony_core.config_model import ConfigError, check_yaml_depth, load_source, parse_model
from harmony_core.page_codec import decode_page

from . import __version__, schemas
from .auth import (
    HASSIO_NETWORK,
    Auth,
    BundleAuth,
    IngressAuth,
    RouteAuth,
    load_or_create_token,
)
from .options import Options, load_options
from .store import HISTORY_MAX, Store, StoreError
from .supervisor import Discovery

log = logging.getLogger(__name__)

MAX_BODY = 1024 * 1024
MUTATING = {"POST", "PUT", "PATCH", "DELETE"}
JSON_TYPES = {"application/json"}
YAML_TYPES = {"text/yaml", "application/yaml"}
CSP = (
    "default-src 'none'; base-uri 'none'; form-action 'none'; "
    "frame-ancestors 'self'; img-src 'self' data:; style-src 'self'; script-src 'self'"
)
CSRF_HEADER = "X-HH-CSRF"

OPTIONS_KEY = web.AppKey("options", Options)
STORE_KEY = web.AppKey("store", Store)
CLOCK_KEY = web.AppKey("clock", Callable)
NOTIFIER_KEY = web.AppKey("notifier", object)
WAIT_KEY = web.AppKey("wait_s", float)
WAIT_SECONDS = 55.0


def _json(data: Any, status: int = 200) -> web.Response:
    return web.json_response(data, status=status)


def _error(
    status: int, code: str, msg: str, *, headers: dict[str, str] | None = None, **extra: Any
) -> web.HTTPException:
    kw = {"headers": headers} if headers else {}
    body = json.dumps({"error": {"code": code, "msg": msg, **extra}})
    cls = {
        400: web.HTTPBadRequest,
        401: web.HTTPUnauthorized,
        403: web.HTTPForbidden,
        404: web.HTTPNotFound,
        409: web.HTTPConflict,
        422: web.HTTPUnprocessableEntity,
        429: web.HTTPTooManyRequests,
    }[status]
    return cls(text=body, content_type="application/json", **kw)


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
    if request.path.startswith("/api/"):
        resp.headers["Cache-Control"] = "no-store"


def make_auth_mw(auth: Auth):
    @web.middleware
    async def auth_mw(request: web.Request, handler):
        try:
            auth.check(request)
        except web.HTTPForbidden:
            raise _error(403, "forbidden", "forbidden") from None
        except web.HTTPUnauthorized:
            raise _error(401, "unauthorized", "unauthorized") from None
        except web.HTTPTooManyRequests:
            raise _error(
                429, "rate_limited", "too many attempts", headers={"Retry-After": "60"}
            ) from None
        if request.method in MUTATING:
            allowed = YAML_TYPES if request.path == "/api/v1/config/import_yaml" else JSON_TYPES
            if (
                request.content_type not in allowed
                or request.headers.get(CSRF_HEADER) != "1"
                or not _same_origin(request)
            ):
                raise _error(403, "csrf", "forbidden")
        return await handler(request)

    return auth_mw


# ------------------------------------------------------------------ helpers
async def _body(request: web.Request, schema: vol.Schema) -> dict[str, Any]:
    raw = await request.read()  # 413 beyond client_max_size
    try:
        data = json.loads(raw)
    except ValueError, RecursionError:
        raise _error(400, "bad_json", "body is not valid JSON") from None
    return _validate(schema, data)


def _validate(schema: vol.Schema, data: Any) -> dict[str, Any]:
    try:
        return schema(data)
    except vol.Invalid as exc:
        path = ".".join(str(p) for p in exc.path)
        if not re.fullmatch(r"[A-Za-z0-9_.]{1,40}", path):
            path = "body"  # never echo raw input
        raise _error(400, "bad_request", f"invalid request: {path}") from None


def _query(request: web.Request, schema: vol.Schema) -> dict[str, Any]:
    return _validate(schema, dict(request.query))


def _check(config: dict[str, Any]) -> tuple[dict[str, Any] | None, list[dict[str, Any]]]:
    try:
        return parse_model(json.dumps(config), "config"), []
    except ConfigError as exc:
        return None, [{"path": list(exc.path), "msg": exc.msg[:300], "line": None}]


def _pick_config(state: dict[str, Any], given: dict[str, Any] | None) -> dict[str, Any]:
    if given is not None:
        return given
    if state["draft"] is None:
        raise _error(409, "no_draft", "there is no draft; send a config")
    return state["draft"]["config"]


def _meta(state: dict[str, Any]) -> dict[str, Any]:
    a = state["applied"]
    return {
        "applied": None if a is None else {k: a[k] for k in ("rev", "at", "hash")},
        "history": [{k: h[k] for k in ("rev", "at", "hash")} for h in state["history"]],
        "history_max": HISTORY_MAX,
    }


def _store_call(fn, *args):
    try:
        return fn(*args)
    except StoreError as exc:
        raise _error(409, "store", str(exc)) from None
    except OSError:
        log.exception("storage failure")
        raise _error(409, "store", "storage failure") from None


# ------------------------------------------------------------------ routes
async def health(request: web.Request) -> web.Response:
    return _json(
        {
            "ok": True,
            "addon_version": __version__,
            "core_version": harmony_core.__version__,
            "bundle_schema": harmony_core.BUNDLE_SCHEMA,
        }
    )


async def get_config(request: web.Request) -> web.Response:
    q = _query(request, schemas.GET_CONFIG_QUERY)
    state = _store_call(request.app[STORE_KEY].load, q["hub"])
    out = {"hub": q["hub"], "draft": state["draft"], **_meta(state)}
    if q["full"] == "1" and state["applied"]:
        out["applied"] = state["applied"]
    return _json(out)


async def put_draft(request: web.Request) -> web.Response:
    q = _query(request, schemas.QUERY)
    body = await _body(request, schemas.PUT_DRAFT)
    now = int(request.app[CLOCK_KEY]())
    state = _store_call(request.app[STORE_KEY].set_draft, q["hub"], body["config"], now)
    return _json({"hub": q["hub"], "draft_updated": now, **_meta(state)})


async def validate(request: web.Request) -> web.Response:
    q = _query(request, schemas.QUERY)
    body = await _body(request, schemas.OPT_CONFIG)
    state = _store_call(request.app[STORE_KEY].load, q["hub"])
    _, errors = _check(_pick_config(state, body.get("config")))
    return _json({"ok": not errors, "errors": errors})


def _compile(config: dict[str, Any]):
    model, errors = _check(config)
    if errors:
        return None, None, errors
    try:
        return model, compile_config(model), []
    except CompileError as exc:
        return None, None, [{"path": [], "msg": str(exc)[:300], "line": None}]


async def preview(request: web.Request) -> web.Response:
    q = _query(request, schemas.QUERY)
    body = await _body(request, schemas.OPT_CONFIG)
    state = _store_call(request.app[STORE_KEY].load, q["hub"])
    _, compiled, errors = _compile(_pick_config(state, body.get("config")))
    if errors:
        return _json({"ok": False, "errors": errors}, 422)
    hub = compiled.hub_json
    return _json(
        {
            "ok": True,
            "hash": compiled.config_hash,
            "pages": [dataclasses.asdict(decode_page(p)) for p in compiled.pages],
            "usage": {
                "json_bytes": len(compact_json(hub).encode()),
                "json_max": LIMITS["MAX_JSON"],
                "pool_words": _pool_words(hub),
                "pool_max": LIMITS["POOL_WORDS"],
                "devices": len(compiled.devices),
                "devices_max": LIMITS["MAX_DEVICES"],
                "activities": len(compiled.activities),
                "activities_max": LIMITS["MAX_ACTIVITIES"],
            },
        }
    )


async def apply(request: web.Request) -> web.Response:
    q = _query(request, schemas.QUERY)
    await _body(request, schemas.EMPTY)
    store = request.app[STORE_KEY]
    state = _store_call(store.load, q["hub"])
    config = _pick_config(state, None)
    _, compiled, errors = _compile(config)
    if errors:
        return _json({"ok": False, "errors": errors}, 422)
    now = int(request.app[CLOCK_KEY]())
    state = _store_call(store.apply, q["hub"], config, compiled.config_hash, now)
    request.app[NOTIFIER_KEY].notify(q["hub"])
    return _json({"ok": True, **_meta(state)})


async def revert(request: web.Request) -> web.Response:
    q = _query(request, schemas.QUERY)
    body = await _body(request, schemas.REVERT)
    state = _store_call(request.app[STORE_KEY].revert, q["hub"], body["target"])
    if body["target"] == "previous":  # discarding a draft changes nothing the integration sees
        request.app[NOTIFIER_KEY].notify(q["hub"])
    return _json({"ok": True, **_meta(state)})


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
        raise _error(400, "too_large", "YAML is too large")
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError:
        raise _error(400, "bad_yaml", "YAML must be UTF-8") from None
    try:
        check_yaml_depth(text)
    except ConfigError as exc:
        raise _error(400, "bad_yaml", exc.msg[:200]) from None
    try:
        if any(isinstance(e, yaml.AliasEvent) for e in yaml.parse(text, Loader=yaml.SafeLoader)):
            raise _error(400, "yaml_alias", "YAML anchors and aliases are not accepted")
    except yaml.YAMLError, RecursionError:
        pass  # load_source reports the syntax / nesting error below
    try:
        loaded = load_source(text)
    except ConfigError as exc:
        raise _error(400, "bad_yaml", exc.msg[:200]) from None
    try:  # non-JSON YAML types (timestamps, sets, ...) and NaN are refused, int keys become str
        data = json.loads(json.dumps({} if loaded is None else loaded, allow_nan=False))
    except TypeError, ValueError, RecursionError:
        raise _error(400, "bad_yaml", "YAML holds values a config cannot contain") from None
    try:
        return schemas.bounded_config(data)
    except vol.Invalid as exc:
        raise _error(400, "bad_request", f"YAML rejected: {exc.error_message}"[:100]) from None


async def import_yaml(request: web.Request) -> web.Response:
    q = _query(request, schemas.QUERY)
    config = _yaml_to_config(await request.read())
    refs = _ir_file_refs(config)
    if refs:
        raise _error(
            422,
            "ir_files_unresolvable",
            "IR file references cannot be resolved here; inline the codes or import them in the "
            "editor",
            files=refs,
        )
    _, errors = _check(config)
    if errors:
        raise _error(422, "invalid_config", "the YAML is not a valid config", errors=errors)
    now = int(request.app[CLOCK_KEY]())
    state = _store_call(request.app[STORE_KEY].set_draft, q["hub"], config, now)
    return _json({"ok": True, "hub": q["hub"], "draft_updated": now, **_meta(state)})


async def export_yaml(request: web.Request) -> web.Response:
    q = _query(request, schemas.EXPORT_QUERY)
    state = _store_call(request.app[STORE_KEY].load, q["hub"])
    src = q.get("source") or ("draft" if state["draft"] else "applied")
    entry = state[src]
    if entry is None:
        raise _error(404, "empty", f"there is no {src} config")
    text = yaml.safe_dump(entry["config"], sort_keys=False, allow_unicode=True)
    return web.Response(text=text, content_type="text/yaml", charset="utf-8")


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


def _rev(config: dict[str, Any]) -> str:
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
        "config": a["config"],
    }


def _bundle_response(bundle: dict[str, Any]) -> web.Response:
    return web.json_response(bundle, headers={"ETag": f'"{bundle["rev"]}"'})


def _nothing_applied() -> web.HTTPException:
    return _error(404, "nothing_applied", "no config has been applied for this hub")


async def get_bundle(request: web.Request) -> web.Response:
    q = _query(request, schemas.QUERY)
    bundle = _bundle(q["hub"], _store_call(request.app[STORE_KEY].load, q["hub"]))
    if bundle is None:
        raise _nothing_applied()
    if request.headers.get("If-None-Match") == f'"{bundle["rev"]}"':
        return web.Response(status=304, headers={"ETag": f'"{bundle["rev"]}"'})
    return _bundle_response(bundle)


async def wait_bundle(request: web.Request) -> web.Response:
    """Long-poll: answer when the applied rev differs from `rev`; 204 after the bounded wait."""
    q = _query(request, schemas.WAIT_QUERY)
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
    app.router.add_get("/", index)
    app.router.add_get(f"{api}/health", health)
    app.router.add_get(f"{api}/config", get_config)
    app.router.add_put(f"{api}/config/draft", put_draft)
    app.router.add_post(f"{api}/config/validate", validate)
    app.router.add_post(f"{api}/config/preview", preview)
    app.router.add_post(f"{api}/config/apply", apply)
    app.router.add_post(f"{api}/config/revert", revert)
    app.router.add_post(f"{api}/config/import_yaml", import_yaml)
    app.router.add_get(f"{api}/config/export_yaml", export_yaml)
    app.router.add_get(f"{api}/bundle", get_bundle)
    app.router.add_get(f"{api}/bundle/wait", wait_bundle)
    return app
