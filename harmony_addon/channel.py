"""Integration WebSocket `GET /api/v1/channel` (docs/addon-api.md "Channel"): bundle push, ack, identity, Bluetooth relay.

Liveness is kernel TCP keepalive (`sockopts.set_keepalive`): no application ping, no aiohttp heartbeat, no timer while
connected. The only timer is the 10 s wait for the first frame."""

from __future__ import annotations

import asyncio
import contextlib
import json
import logging
import re
import time
from dataclasses import dataclass, field
from typing import Any

import voluptuous as vol
from aiohttp import WSMsgType, web

from . import bt_routes, live, schemas
from .linked import clean_name
from .sockopts import set_keepalive
from .store import HUB_ID
from .web_util import CLOCK_KEY, query

log = logging.getLogger(__name__)

VERSION = 1
HELLO_TIMEOUT = 10.0
SHUTDOWN_CLOSE_S = 2.0
_BG: set[asyncio.Task] = set()
MAX_FRAME = 1 << 20
HELLO_KEY = web.AppKey("hello_timeout", float)
CHANNEL_KEY = web.AppKey("channel", object)

# The hub daemon's own log lines (relayed by the integration) go to the add-on's stdout, one logger per hub
# (`hub.<id>`), pinned to INFO so the add-on's log_level option cannot hide them.
logging.getLogger("hub").setLevel(logging.INFO)
# The hub stamps every shipped line "[HH:MM:SS.mmm +<uptime>ms] "; a line without it (older hub, a relayed helper line)
# gets the time it arrived here, so every line in this log can be placed on the timeline.
_HUB_STAMP = re.compile(r"\[\d\d:\d\d:\d\d\.\d{3} \+\d+ms\] ")


def _stamped(msg: str, now: float) -> str:
    if _HUB_STAMP.match(msg):
        return msg
    ms = int(now * 1000) % 1000
    return f"[{time.strftime('%H:%M:%S', time.gmtime(now))}.{ms:03d} rx] {msg}"


_HUB_LOG = vol.Schema(
    {
        vol.Required("level"): vol.In(("info", "warning")),
        vol.Required("msg"): vol.All(str, vol.Length(max=400)),
        vol.Optional("ts"): int,
        vol.Optional("dropped"): int,
    },
    extra=vol.ALLOW_EXTRA,
)
_HUB_FRAME = vol.Schema({vol.Required("online"): bool}, extra=vol.ALLOW_EXTRA)
_BT_STATE = vol.Schema(
    {
        vol.Required("online"): bool,
        vol.Required("supported"): bool,
        vol.Optional("state"): vol.Any(None, dict),
        vol.Optional("limits"): vol.Any(None, dict),
    },
    extra=vol.ALLOW_EXTRA,
)


@dataclass(eq=False)
class Conn:
    hub: str
    ws: web.WebSocketResponse
    at: int
    ready: bool = False  # hello answered: pushes may flow
    lock: asyncio.Lock = field(default_factory=asyncio.Lock)
    tasks: set[asyncio.Task] = field(default_factory=set)

    async def send(self, frame: dict[str, Any]) -> bool:
        if self.ws.closed:
            return False
        try:
            async with self.lock:
                await self.ws.send_json(frame)
        except ConnectionError, RuntimeError:
            return False
        return True

    def spawn(self, coro) -> None:
        t = asyncio.ensure_future(coro)
        self.tasks.add(t)
        t.add_done_callback(self.tasks.discard)


class Channels:
    """The open channel per hub id (one each: a second replaces the first)."""

    def __init__(self) -> None:
        self.conns: dict[str, Conn] = {}


# ------------------------------------------------------------------ bundle push
def _bundle_frame(app: web.Application, hub: str) -> dict[str, Any] | None:
    from .app import STORE_KEY, _bundle, _store_call

    b = _bundle(hub, _store_call(app[STORE_KEY].load, hub))
    return None if b is None else {"t": "bundle", "rev": b["rev"], "body": b}


async def _frame_off_loop(app: web.Application, hub: str) -> dict[str, Any] | None:
    """The bundle frame, read from disk in an executor so a slow volume never stalls the loop (reads see whole files:
    every write is an atomic replace)."""
    return await asyncio.get_running_loop().run_in_executor(None, _bundle_frame, app, hub)


async def _push(app: web.Application, conn: Conn) -> None:
    try:
        frame = await _frame_off_loop(app, conn.hub)
    except web.HTTPException:
        await conn.send({"t": "error", "code": "store_error"})
        return
    if frame is not None:
        await conn.send(frame)


def on_apply(app: web.Application, hub: str) -> None:
    """The apply notifier: a hub's applied config changed. Push it to its channel, tell the editors."""
    conn = app[CHANNEL_KEY].conns.get(hub)
    if conn is not None and conn.ready:
        conn.spawn(_push(app, conn))
    live.touch(app, hub)


# ------------------------------------------------------------------ identity
def _identity(app: web.Application, hub: str, f: dict[str, Any], *, new: bool) -> str | None:
    """hello / identity fields (the former X-Hub-* headers). Returns the `adopt` answer when `prev`/`hw` was sent."""
    from .app import LINKED_KEY, _adopt_renamed, _auto_adopt

    linked = app[LINKED_KEY]
    hw = f.get("hw") if isinstance(f.get("hw"), str) else None
    prev = f.get("prev") if isinstance(f.get("prev"), str) else None
    raw_name = f.get("name")
    adopt = None
    if prev is not None or hw is not None:
        adopt = _adopt_renamed(app, hub, prev, hw)
    linked.note_identity(hub, hw, clean_name(raw_name) if isinstance(raw_name, str) else None)
    if new:
        _auto_adopt(app)
    if isinstance(f.get("remotes"), list) and len(json.dumps(f["remotes"])) <= 4000:
        linked.set_remotes(hub, f["remotes"])
    if isinstance(f.get("limits"), dict) and len(json.dumps(f["limits"])) <= 2000:
        linked.set_limits(hub, f["limits"])
    return adopt


# ------------------------------------------------------------------ frames
async def _on_ack(app: web.Application, conn: Conn, f: dict[str, Any]) -> None:
    from .app import ACKS_KEY, STORE_KEY, _bundle, _store_call

    hub = conn.hub
    try:
        body = schemas.ACK(f)
    except vol.Invalid:
        await conn.send({"t": "error", "code": "bad_frame"})
        return
    try:
        store = app[STORE_KEY]
        state = await asyncio.get_running_loop().run_in_executor(None, _store_call, store.load, hub)
        bundle = _bundle(hub, state)
        if (
            bundle is None or body["rev"] != bundle["rev"]
        ):  # about an older bundle: not what is applied now
            await conn.send(
                {
                    "t": "error",
                    "code": "stale_rev",
                    **({} if bundle is None else {"rev": bundle["rev"]}),
                }
            )
            return
        ack = {"rev": body["rev"], "result": body["result"], "at": int(app[CLOCK_KEY]())}
        if "detail" in body:
            ack["detail"] = body["detail"]
        prev = state["ack"] or {}
        if any(
            prev.get(k) != ack.get(k) for k in ("rev", "result", "detail")
        ):  # no write per repeat
            _store_call(store.set_ack, hub, ack)
            app[ACKS_KEY][hub] = ack
    except web.HTTPException:
        await conn.send({"t": "error", "code": "store_error"})
        return
    live.publish_hub(app, hub)


async def _on_frame(app: web.Application, conn: Conn, f: dict[str, Any]) -> None:
    hub, t = conn.hub, f.get("t")
    if t == "identity":
        adopt = _identity(app, hub, f, new=False)
        if adopt is not None:
            await conn.send({"t": "adopt", "result": adopt})
        live.touch(app, hub)
    elif t == "ack":
        await _on_ack(app, conn, f)
    elif t == "bt_state":
        try:
            bt_routes.on_bt_state(app, hub, _BT_STATE(f))
        except vol.Invalid:
            await conn.send({"t": "error", "code": "bad_frame"})
            return
        live.publish_bt(app, hub)
    elif t == "bt_result":
        try:
            r = bt_routes._RESULT(f)
        except vol.Invalid:
            await conn.send({"t": "error", "code": "bad_frame"})
            return
        if bt_routes.on_bt_result(app, hub, r):
            live.publish_bt(app, hub)
    elif t == "bt_key":
        try:
            ev = bt_routes._EVENT(f)
        except vol.Invalid:
            await conn.send({"t": "error", "code": "bad_frame"})
            return
        h = app[bt_routes.BT_KEY].hub(hub, create=True)
        if h is not None:
            h.key_seq += 1
            key = {k: ev[k] for k in ("slot", "page", "usage", "down")}
            live.publish_key(app, hub, {"seq": h.key_seq, **key, "ts": int(app[CLOCK_KEY]())})
    elif t == "hub_log":
        try:
            ln = _HUB_LOG(f)
        except vol.Invalid:
            await conn.send({"t": "error", "code": "bad_frame"})
            return
        lg = logging.getLogger(f"hub.{hub}")
        if ln.get("dropped"):
            lg.warning(
                "%d hub log line(s) were lost before this one (hub ring full)", ln["dropped"]
            )
        lg.log(
            logging.WARNING if ln["level"] == "warning" else logging.INFO,
            "%s",
            _stamped(ln["msg"], app[CLOCK_KEY]()),
        )
    elif t == "hub":
        try:
            online = _HUB_FRAME(f)["online"]
        except vol.Invalid:
            await conn.send({"t": "error", "code": "bad_frame"})
            return
        h = app[bt_routes.BT_KEY].hub(hub)
        if h is not None and h.online != online:
            h.online = online
            live.publish_bt(app, hub)
    else:
        log.debug("channel %s: ignoring frame %r", hub, t)


def _closed(app: web.Application, conn: Conn) -> None:
    """The socket is gone: the hub is not connected, commands in flight fail `hub_offline`."""
    from .app import LINKED_KEY

    chs = app[CHANNEL_KEY]
    for t in tuple(conn.tasks):
        t.cancel()
    if chs.conns.get(conn.hub) is not conn:
        return  # replaced: the new socket owns the hub
    del chs.conns[conn.hub]
    h = app[bt_routes.BT_KEY].hub(conn.hub)
    if h is not None:
        bt_routes.fail_waiting(h, "hub_offline")
        h.online = False
    app[LINKED_KEY].seen(conn.hub)
    live.touch(app, conn.hub)
    live.publish_bt(app, conn.hub)


async def _fail(ws: web.WebSocketResponse, code: int, err: str) -> None:
    with contextlib.suppress(ConnectionError, RuntimeError):
        await ws.send_json({"t": "error", "code": err})
    await ws.close(code=code, message=err.encode())


def _hello(raw: Any) -> dict[str, Any] | None:
    if isinstance(raw, bytes | str):
        try:
            raw = json.loads(raw)
        except ValueError:
            return None
    return raw if isinstance(raw, dict) else None


async def channel(request: web.Request) -> web.StreamResponse:
    from .app import LINKED_KEY

    app = request.app
    q = query(request, schemas.QUERY)
    hub = q["hub"]
    ws = web.WebSocketResponse(max_msg_size=MAX_FRAME)
    await ws.prepare(request)
    set_keepalive(request.transport.get_extra_info("socket") if request.transport else None)
    try:
        msg = await asyncio.wait_for(ws.receive(), app[HELLO_KEY])
    except TimeoutError:
        await ws.close(code=4000, message=b"no hello")
        return ws
    f = _hello(msg.data) if msg.type == WSMsgType.TEXT else None
    if f is None or f.get("t") != "hello" or type(f.get("v")) is not int:
        await _fail(ws, 4000, "bad_hello")
        return ws
    if f["v"] != VERSION:
        await _fail(ws, 4001, "unsupported_version")
        return ws
    if not isinstance(f.get("hub"), str) or not HUB_ID.fullmatch(f["hub"]):
        await _fail(ws, 4000, "bad_hello")
        return ws
    if f["hub"] != hub:
        await _fail(ws, 4000, "hub_mismatch")
        return ws

    chs = app[CHANNEL_KEY]
    conn = Conn(hub, ws, int(app[CLOCK_KEY]()))
    old = chs.conns.get(hub)
    chs.conns[hub] = conn
    if old is not None:  # one socket per hub: the old one is replaced
        h0 = app[bt_routes.BT_KEY].hub(hub)
        if h0 is not None:
            bt_routes.fail_waiting(h0, "hub_offline")
        for t in tuple(old.tasks):
            t.cancel()
        t = asyncio.ensure_future(
            _close_quiet(old.ws, 4002, b"replaced")
        )  # not awaited: a stuck old peer must not stall
        _BG.add(t)
        t.add_done_callback(_BG.discard)
    try:
        new = app[LINKED_KEY].seen(hub)
        app[bt_routes.BT_KEY].prune(set(app[LINKED_KEY].ids()))
        adopt = _identity(app, hub, f, new=new)
        if adopt is not None:
            await conn.send({"t": "adopt", "result": adopt})
        conn.ready = (
            True  # pushes now queue behind conn.lock, so a bundle applied from here on is not lost
        )
        try:
            frame = await _frame_off_loop(app, hub)
        except web.HTTPException:
            await conn.send({"t": "error", "code": "store_error"})
        else:
            known = f.get("known_rev")
            if frame is None:
                await conn.send({"t": "nothing_applied"})
            elif frame["rev"] != known:
                await conn.send(frame)
        if live._bus(app).subscribers(live.LIVE, f"keys:{hub}"):
            await conn.send({"t": "listen", "on": True})
        live.touch(app, hub)
        live.publish_bt(app, hub)
        async for m in ws:
            if m.type != WSMsgType.TEXT:
                continue
            fr = _hello(m.data)
            if fr is None or not isinstance(fr.get("t"), str):
                await conn.send({"t": "error", "code": "bad_frame"})
                continue
            await _on_frame(app, conn, fr)
    finally:
        _closed(app, conn)
        if not ws.closed:
            with contextlib.suppress(Exception):
                await ws.close()
    return ws


async def _close_quiet(ws: web.WebSocketResponse, code: int, message: bytes) -> None:
    with contextlib.suppress(Exception):
        await ws.close(code=code, message=message)


def add_routes(app: web.Application, api: str) -> None:
    app.router.add_get(f"{api}/channel", channel)

    async def _close(_: web.Application) -> None:
        socks = [c.ws for c in tuple(app[CHANNEL_KEY].conns.values())]
        if socks:
            await asyncio.wait(
                [asyncio.ensure_future(_close_quiet(w, 1001, b"shutdown")) for w in socks],
                timeout=SHUTDOWN_CLOSE_S,
            )

    app.on_shutdown.append(_close)
