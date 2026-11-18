"""Small HTTP helpers shared by the route modules."""

from __future__ import annotations

import json
import re
from collections.abc import Callable
from typing import Any

import voluptuous as vol
from aiohttp import web

CLOCK_KEY = web.AppKey("clock", Callable)


def jresp(data: Any, status: int = 200) -> web.Response:
    return web.json_response(data, status=status)


def error(
    status: int, code: str, msg: str, *, headers: dict[str, str] | None = None, **extra: Any
) -> web.HTTPException:
    kw = {"headers": headers} if headers else {}
    text = json.dumps({"error": {"code": code, "msg": msg, **extra}})
    cls = {
        400: web.HTTPBadRequest,
        401: web.HTTPUnauthorized,
        403: web.HTTPForbidden,
        404: web.HTTPNotFound,
        409: web.HTTPConflict,
        422: web.HTTPUnprocessableEntity,
        429: web.HTTPTooManyRequests,
        502: web.HTTPBadGateway,
        503: web.HTTPServiceUnavailable,
        504: web.HTTPGatewayTimeout,
    }[status]
    return cls(text=text, content_type="application/json", **kw)


async def read_body(request: web.Request, schema: vol.Schema) -> dict[str, Any]:
    raw = await request.read()  # 413 beyond client_max_size
    try:
        data = json.loads(raw)
    except ValueError, RecursionError:
        raise error(400, "bad_json", "body is not valid JSON") from None
    return validate(schema, data)


def validate(schema: vol.Schema, data: Any) -> dict[str, Any]:
    try:
        return schema(data)
    except vol.Invalid as exc:
        path = ".".join(str(p) for p in exc.path)
        if not re.fullmatch(r"[A-Za-z0-9_.]{1,40}", path):
            path = "body"  # never echo raw input
        raise error(400, "bad_request", f"invalid request: {path}") from None


def query(request: web.Request, schema: vol.Schema) -> dict[str, Any]:
    if len(set(request.query.keys())) != len(request.query):
        raise error(400, "bad_request", "invalid request: repeated parameter")
    return validate(schema, dict(request.query))
