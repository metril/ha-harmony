"""Editor WebSocket `GET /api/v1/live` (docs/addon-api.md "Live"): subscribe, snapshot, deltas from the bus.

State changes are published on the bus under the hub key `live` with the topic names of the protocol (`hubs`, `hub:<id>`,
`bt:<id>`, `keys:<id>`); every socket holds one bounded subscription. A `keys:<id>` subscription is the listen lease."""

from __future__ import annotations

import asyncio
import contextlib
import json
import logging
from typing import Any

from aiohttp import WSMsgType, web

from .sockopts import set_keepalive
from .store import HUB_ID
from .web_util import error

log = logging.getLogger(__name__)

LIVE = "live"
SUB_MAX = 16
QUEUE_KEY = web.AppKey("live_queue", int)
SOCKETS_KEY = web.AppKey("live_sockets", set)


# ------------------------------------------------------------------ publishing (called by the app / channel)
def _bus(app: web.Application):
    from .app import BUS_KEY

    return app[BUS_KEY]


def _safe(fn, *args):
    try:
        return fn(*args)
    except web.HTTPException, OSError:  # an unreadable store: the next change republishes
        log.debug("live payload skipped", exc_info=True)
        return None


def publish_hubs(app: web.Application) -> None:
    from .app import hubs_payload

    if _bus(app).subscribers(LIVE, "hubs") and (p := _safe(hubs_payload, app)) is not None:
        _bus(app).publish(LIVE, "hubs", {"t": "hubs", **p})


def publish_hub(app: web.Application, hub: str) -> None:
    from .app import hub_status

    if _bus(app).subscribers(LIVE, f"hub:{hub}") and (p := _safe(hub_status, app, hub)):
        _bus(app).publish(LIVE, f"hub:{hub}", {"t": "hub", **p})


def publish_bt(app: web.Application, hub: str) -> None:
    from .bt_routes import bt_payload

    if _bus(app).subscribers(LIVE, f"bt:{hub}"):
        _bus(app).publish(LIVE, f"bt:{hub}", {"t": "bt", **bt_payload(app, hub)})


def publish_key(app: web.Application, hub: str, msg: dict[str, Any]) -> None:
    _bus(app).publish(LIVE, f"keys:{hub}", {**msg, "t": "key", "hub": hub})


def touch(app: web.Application, hub: str) -> None:
    """The store or link state of `hub` changed."""
    publish_hubs(app)
    publish_hub(app, hub)


# ------------------------------------------------------------------ the socket
def _topic(raw: object) -> str | None:
    if not isinstance(raw, str) or len(raw) > 80:
        return None
    if raw == "hubs":
        return raw
    kind, _, hub = raw.partition(":")
    return raw if kind in ("hub", "bt", "keys") and HUB_ID.fullmatch(hub) else None


def _snapshot(app: web.Application, topic: str) -> Any:
    from .app import hub_status, hubs_payload
    from .bt_routes import bt_payload

    kind, _, hub = topic.partition(":")
    if kind == "hubs":
        return _safe(hubs_payload, app)
    if kind == "hub":
        return _safe(hub_status, app, hub)
    return bt_payload(app, hub)  # kind == "bt"


async def _set_listen(app: web.Application, hub: str, on: bool) -> None:
    from .channel import CHANNEL_KEY

    conn = app[CHANNEL_KEY].conns.get(hub)
    if conn is not None and conn.ready:
        await conn.send({"t": "listen", "on": on})
    publish_bt(app, hub)


async def _retopic(app: web.Application, sub, new: frozenset[str]) -> None:
    """Switch the socket's topics; the first subscriber of `keys:<id>` starts the listen lease, the last one ends it."""
    bus = _bus(app)
    hubs = {t[5:] for t in (sub.topics | new) if t.startswith("keys:")}
    before = {h: bus.subscribers(LIVE, f"keys:{h}") for h in hubs}
    sub.topics = new
    for h in hubs:
        after = bus.subscribers(LIVE, f"keys:{h}")
        if (before[h] == 0) != (after == 0):
            await _set_listen(app, h, after > 0)


async def live(request: web.Request) -> web.StreamResponse:
    from .app import _same_origin

    app = request.app
    if not _same_origin(request):
        raise error(403, "csrf", "forbidden")
    ws = web.WebSocketResponse(max_msg_size=16384)
    await ws.prepare(request)
    set_keepalive(request.transport.get_extra_info("socket") if request.transport else None)
    app[SOCKETS_KEY].add(ws)

    async with _bus(app).subscribe(LIVE, [], app[QUEUE_KEY]) as sub:

        async def sender() -> None:
            while True:
                _, payload = await sub.get()
                if (
                    sub.take_resync()
                ):  # the queue overflowed: what was queued is stale, send fresh state
                    sub._q.clear()
                    for t in sorted(sub.topics):
                        if t.startswith("keys:"):
                            continue
                        await ws.send_json({"t": "resync", "topic": t})
                        await ws.send_json({"t": "snapshot", "topics": {t: _snapshot(app, t)}})
                    continue
                await ws.send_json(payload)

        task = asyncio.ensure_future(sender())
        try:
            async for msg in ws:
                if msg.type != WSMsgType.TEXT:
                    continue
                try:
                    raw = json.loads(msg.data)["sub"]
                    if not isinstance(raw, list) or len(raw) > SUB_MAX:
                        raise ValueError
                except ValueError, KeyError, TypeError:
                    await ws.send_json({"t": "error", "code": "bad_sub"})
                    continue
                new = frozenset(t for t in map(_topic, raw) if t)
                added = sorted(new - sub.topics)
                await _retopic(app, sub, new)  # first: a `bt` snapshot then already shows the lease
                snap = {t: _snapshot(app, t) for t in added if not t.startswith("keys:")}
                if snap:  # through the queue, so no later delta can overtake it
                    sub._put("snapshot", {"t": "snapshot", "topics": snap})
        finally:
            task.cancel()
            with contextlib.suppress(asyncio.CancelledError, Exception):
                await task
            app[SOCKETS_KEY].discard(ws)
            with contextlib.suppress(Exception):
                await _retopic(app, sub, frozenset())
    return ws


async def _close_quiet(ws: web.WebSocketResponse) -> None:
    with contextlib.suppress(Exception):
        await ws.close(code=1001, message=b"shutdown")


def add_routes(app: web.Application, api: str) -> None:
    app[SOCKETS_KEY] = set()
    app.router.add_get(f"{api}/live", live)

    async def _close(_: web.Application) -> None:
        socks = tuple(app[SOCKETS_KEY])
        if socks:
            await asyncio.wait([asyncio.ensure_future(_close_quiet(w)) for w in socks], timeout=2.0)

    app.on_shutdown.append(_close)
