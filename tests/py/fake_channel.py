"""A fake Harmony Editor add-on: just the bearer-authenticated `/api/v1/channel` WebSocket (docs/addon-api.md "Channel").

Independent of `harmony_addon`, so it is the spec the integration is tested against. Per hub id it keeps the applied
config, the last ack and the linked identity; it pushes `bundle` on `apply()` and after `hello`, and records every frame
the integration sends. Waiting is on frames (`frame()`), never on sleeps.
"""

from __future__ import annotations

import asyncio
import hashlib
import json
from typing import Any

from aiohttp import WSMsgType, web

from harmony_core import BUNDLE_SCHEMA

CHANNEL_V = 1


class _Store:
    """The bits of the real add-on's store the rename tests look at."""

    def __init__(self, addon: FakeChannelAddon) -> None:
        self._a = addon

    def hubs(self) -> list[str]:
        return list(self._a.applied)

    def load(self, hub: str) -> dict[str, Any]:
        return {"ack": self._a.acks.get(hub), "applied": self._a.applied.get(hub)}


class FakeChannelAddon:
    def __init__(self, token: str) -> None:
        self.token = token
        self.server: Any = None
        self.applied: dict[str, dict[str, Any]] = {}  # hub -> bundle body
        self.acks: dict[str, dict[str, Any]] = {}
        self.linked: dict[str, dict[str, Any]] = {}  # hub -> {"id", "hw", "name"}
        self.closing = False
        self.all_ws: set[web.WebSocketResponse] = set()
        self.sockets: dict[str, web.WebSocketResponse] = {}
        self.frames: list[dict[str, Any]] = []  # everything the integration sent, in order
        self.hellos: list[dict[str, Any]] = []
        self.connects = 0  # channel upgrades accepted
        self.listen_on = False  # sent as `listen` after each hello when True
        self.refuse: int | None = None  # answer the upgrade with this HTTP status
        self.no_adopt = False  # an add-on that ignores hw/prev: never answers `adopt`
        self.silent = False  # after hello, send nothing (not even bundle/nothing_applied)
        self.close_after_hello: int | None = None  # close the socket with this code right after hello
        self.extra_after_hello: list[dict[str, Any]] = []  # frames sent after the hello answer
        self.store = _Store(self)
        self._now = 1_000
        self._cond = asyncio.Condition()
        self._tasks: set[asyncio.Task[None]] = set()

    # ------------------------------------------------------------ test API
    @property
    def port(self) -> int:
        return self.server.port

    def link_data(self) -> dict[str, Any]:
        return {"addon_host": "127.0.0.1", "addon_port": self.port, "addon_token": self.token}

    def linked_entries(self) -> list[dict[str, Any]]:
        return list(self.linked.values())

    def apply(self, config: dict[str, Any], hub: str = "sim-hub", *, push: bool = True) -> str:
        """Store `config` as the applied one (no validation) and push the bundle when a socket is open."""
        self._now += 1
        rev = hashlib.sha1(json.dumps(config, sort_keys=True).encode()).hexdigest()[:16]
        self.applied[hub] = {
            "schema": BUNDLE_SCHEMA,
            "core_version": "0.1.0",
            "hub_id": hub,
            "applied_at": self._now,
            "rev": rev,
            "config": config,
        }
        if push and hub in self.sockets:
            self.send(hub, {"t": "bundle", "rev": rev, "body": self.applied[hub]})
        return rev

    def apply_yaml(self, text: str, hub: str = "sim-hub") -> str:
        from harmony_core.config_model import load_source

        return self.apply(json.loads(json.dumps(load_source(text) or {})), hub)

    def send(self, hub: str, frame: dict[str, Any]) -> None:
        """Queue a frame to the hub's open socket (sync, callable from a test body)."""
        ws = self.sockets.get(hub)
        if ws is None:
            return
        task = asyncio.ensure_future(ws.send_str(json.dumps(frame)))
        self._tasks.add(task)
        task.add_done_callback(self._tasks.discard)

    async def close_socket(self, hub: str, code: int = 1000) -> None:
        ws = self.sockets.get(hub)
        if ws is not None:
            await ws.close(code=code)

    async def frame(self, t: str, pred: Any = None, *, timeout: float = 8.0, start: int = 0) -> dict[str, Any]:
        """The first received frame of type `t` (matching `pred`) at or after index `start`. Waits on arrival."""

        def find() -> dict[str, Any] | None:
            return next((f for f in self.frames[start:] if f.get("t") == t and (pred is None or pred(f))), None)

        async with asyncio.timeout(timeout):
            async with self._cond:
                while (hit := find()) is None:
                    await self._cond.wait()
        return hit

    def of(self, t: str) -> list[dict[str, Any]]:
        return [f for f in self.frames if f.get("t") == t]

    # ------------------------------------------------------------ the server side
    async def _channel(self, request: web.Request) -> web.StreamResponse:
        if request.headers.get("Authorization") != f"Bearer {self.token}":
            return web.Response(status=401)
        if self.refuse is not None or self.closing:
            return web.Response(status=self.refuse or 503)
        hub = request.query.get("hub", "")
        ws = web.WebSocketResponse(heartbeat=None)
        await ws.prepare(request)
        self.connects += 1
        self.all_ws.add(ws)
        old = self.sockets.get(hub)
        self.sockets[hub] = ws
        if old is not None and not old.closed:
            await old.close(code=4002, message=b"replaced")
        try:
            async for msg in ws:
                if msg.type is not WSMsgType.TEXT:
                    continue
                frame = json.loads(msg.data)
                self.frames.append(frame)
                async with self._cond:
                    self._cond.notify_all()
                if frame.get("t") == "hello":
                    await self._hello(ws, hub, frame)
                elif frame.get("t") == "ack":
                    await self._ack(ws, hub, frame)
                elif frame.get("t") == "identity":
                    self._record_identity(hub, frame)
        finally:
            self.all_ws.discard(ws)
            if self.sockets.get(hub) is ws:
                del self.sockets[hub]
        return ws

    def _record_identity(self, hub: str, f: dict[str, Any]) -> None:
        row = self.linked.setdefault(hub, {"id": hub})
        for k in ("hw", "name"):
            if k in f:
                row[k] = f[k]

    async def _hello(self, ws: web.WebSocketResponse, hub: str, f: dict[str, Any]) -> None:
        self.hellos.append(f)
        if f.get("v") != CHANNEL_V:
            await ws.send_str(json.dumps({"t": "error", "code": "unsupported_version"}))
            await ws.close(code=4001)
            return
        adopt = self._adopt(hub, f) if (("prev" in f or "hw" in f) and not self.no_adopt) else None
        self._record_identity(hub, f)
        if self.close_after_hello is not None:
            await ws.close(code=self.close_after_hello)
            return
        if adopt is not None:
            await ws.send_str(json.dumps({"t": "adopt", "result": adopt}))
        if not self.silent:
            body = self.applied.get(hub)
            if body is None:
                await ws.send_str(json.dumps({"t": "nothing_applied"}))
            elif body["rev"] != f.get("known_rev"):
                await ws.send_str(json.dumps({"t": "bundle", "rev": body["rev"], "body": body}))
        if self.listen_on:
            await ws.send_str(json.dumps({"t": "listen", "on": True}))
        for extra in self.extra_after_hello:
            await ws.send_str(json.dumps(extra))

    def _adopt(self, hub: str, f: dict[str, Any]) -> str:
        """The renamed-hub rule: move a store under `prev` (or a linked id with the same hw) to `hub`, never over one."""
        if hub in self.applied:
            return "exists"
        prev = f.get("prev")
        hw = f.get("hw")
        src = prev if prev in self.applied else None
        if src is not None and hw and self.linked.get(src, {}).get("hw") not in (None, hw):
            src = None  # another hub
        if src is None and hw:
            src = next((h for h, row in self.linked.items() if row.get("hw") == hw and h in self.applied and h != hub), None)
        if src is None:
            return "none"
        body = dict(self.applied.pop(src))
        body["hub_id"] = hub
        self.applied[hub] = body
        if src in self.acks:
            self.acks[hub] = self.acks.pop(src)
        self.linked.pop(src, None)
        return "moved"

    async def _ack(self, ws: web.WebSocketResponse, hub: str, f: dict[str, Any]) -> None:
        body = self.applied.get(hub)
        if body is None or f.get("rev") != body["rev"]:
            await ws.send_str(json.dumps({"t": "error", "code": "stale_rev"}))
            return
        self.acks[hub] = {k: f[k] for k in ("rev", "result", "detail") if k in f}

    def app(self) -> web.Application:
        app = web.Application()
        app.router.add_get("/api/v1/channel", self._channel)
        app.on_shutdown.append(self._close_all)  # like the real add-on: a shutdown closes the sockets (1001)
        return app

    async def _close_all(self, _app: web.Application) -> None:
        self.closing = True  # no new channel is accepted while the server shuts down
        for ws in list(self.all_ws):
            await ws.close(code=1001)


async def start(aiohttp_server: Any, token: str) -> FakeChannelAddon:
    fake = FakeChannelAddon(token)
    fake.server = await aiohttp_server(fake.app())
    return fake
