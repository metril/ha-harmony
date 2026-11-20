"""Bluetooth relay for the editor (docs/addon-api.md "Bluetooth relay").

The add-on never talks to the hub. An editor command POST becomes a `bt_cmd` frame on the hub's channel; the integration
runs it and answers `bt_result`, which completes the waiting POST. `bt_state` / `bt_result` / `hub` frames keep the
per-hub state, pushed on to the editor's /live. Everything is bounded: commands in flight, results, number of hubs.
The integration re-validates every command; the checks here only keep obvious garbage from being sent.
"""

from __future__ import annotations

import asyncio
import collections
import itertools
import re
from dataclasses import dataclass, field
from typing import Any

import voluptuous as vol
from aiohttp import web

from harmony_core.bt_relay import (
    ADDR,
    RelayError,
    check_command,
    clean_limits,
    clean_state,
)

from .linked import MAX_LINKED
from .schemas import _hub
from .web_util import CLOCK_KEY, error, jresp, read_body

BT_KEY = web.AppKey("bt_relay", object)
RESULT_WAIT_S = 20.0  # how long an editor request waits for the integration to run its command
PENDING_TTL_S = 60  # a command whose result never came frees its slot after this (checked lazily)
RESULTS_MAX = 32
PENDING_MAX = 16  # commands in flight per hub
_ID = re.compile(r"^[A-Za-z0-9_-]{1,16}$")
_CODE = re.compile(r"^[a-z_]{1,32}$")


@dataclass
class HubBt:
    state: dict[str, Any] | None = None
    limits: dict[str, Any] | None = None
    supported: bool = False
    online: bool = False
    reported: bool = False  # a `bt_state` frame has arrived
    key_seq: int = 0
    waiting: dict[str, asyncio.Future[dict[str, Any]]] = field(default_factory=dict)
    born: dict[str, float] = field(default_factory=dict)  # command id -> when it was sent
    done: collections.OrderedDict[str, dict[str, Any]] = field(
        default_factory=collections.OrderedDict
    )  # recent results by id


class BtRelay:
    def __init__(self, result_wait: float = RESULT_WAIT_S) -> None:
        self.hubs: dict[str, HubBt] = {}
        self.result_wait = result_wait
        self._ids = itertools.count(1)

    def hub(self, hub: str, create: bool = False) -> HubBt | None:
        h = self.hubs.get(hub)
        if h is None and create and len(self.hubs) < MAX_LINKED:
            h = self.hubs[hub] = HubBt()
        return h

    def prune(self, linked: set[str]) -> None:
        """Drop hubs no integration is linked for any more (a rename, a removed integration); their waiters are told."""
        for hub in [k for k in self.hubs if k not in linked]:
            fail_waiting(self.hubs.pop(hub), "hub_offline")


def fail_waiting(h: HubBt, code: str) -> None:
    """The channel closed (or the add-on stops) with commands in flight: the waiting editor requests fail."""
    msg = "the add-on is stopping" if code == "shutdown" else "the hub is offline"
    for fut in h.waiting.values():
        if not fut.done():
            fut.set_result({"ok": False, "code": code, "msg": msg})
    h.waiting.clear()
    h.born.clear()


def bt_status(h: HubBt | None, connected: bool) -> str:
    if h is None or not h.reported:
        return "unknown"
    if not connected or not h.online:
        return "offline"
    return "ok" if h.supported else "unsupported"


def bt_payload(app: web.Application, hub: str) -> dict[str, Any]:
    """The `bt:<id>` topic of /live."""
    from .app import BUS_KEY
    from .channel import CHANNEL_KEY

    h = app[BT_KEY].hub(hub)
    return {
        "hub": hub,
        "status": bt_status(h, hub in app[CHANNEL_KEY].conns),
        "state": h.state if h else None,
        "limits": h.limits if h else None,
        "listening": app[BUS_KEY].subscribers("live", f"keys:{hub}") > 0,
    }


# ------------------------------------------------------------------ schemas
_RESULT = vol.Schema(
    {
        vol.Required("id"): vol.All(str, vol.Match(_ID)),
        vol.Required("ok"): bool,
        vol.Optional("code"): vol.All(str, vol.Match(_CODE)),
        vol.Optional("msg"): vol.All(str, vol.Length(max=200)),
        vol.Optional("state"): vol.Any(None, dict),
    },
    extra=vol.ALLOW_EXTRA,  # a channel frame: unknown fields are ignored
)
_EVENT = vol.Schema(
    {
        vol.Required("slot"): vol.All(int, vol.Range(min=0, max=255)),
        vol.Required("page"): vol.All(int, vol.Range(min=0, max=255)),
        vol.Required("usage"): vol.All(int, vol.Range(min=0, max=65535)),
        vol.Required("down"): bool,
    },
    extra=vol.ALLOW_EXTRA,
)
_HUB_ONLY = {vol.Optional("hub", default="default"): _hub}
_CMD = {
    **_HUB_ONLY,
    vol.Optional("cmd_id"): vol.All(str, vol.Match(_ID)),
}  # the caller's id: a re-post is not a second command
PAIR = vol.Schema(
    {
        **_CMD,
        vol.Optional("mode"): vol.In(["host", "remote"]),
        vol.Optional("transport"): vol.In(["any", "classic", "le"]),
        vol.Optional("secs"): vol.All(int, vol.Range(min=1, max=300)),
    },
    extra=vol.PREVENT_EXTRA,
)
STOP = vol.Schema(_CMD, extra=vol.PREVENT_EXTRA)
PICK = vol.Schema(
    {**_CMD, vol.Required("addr"): vol.All(str, vol.Match(ADDR))}, extra=vol.PREVENT_EXTRA
)
FORGET = vol.Schema(
    {
        **_CMD,
        vol.Required("slot"): vol.All(int, vol.Range(min=0, max=254)),
        vol.Optional("kind"): vol.In(["host", "remote"]),
    },
    extra=vol.PREVENT_EXTRA,
)
KEY = vol.Schema(
    {
        **_CMD,
        vol.Required("slot"): vol.All(int, vol.Range(min=0, max=254)),
        vol.Required("page"): vol.In([7, 12]),
        vol.Required("usage"): vol.All(int, vol.Range(min=0, max=65535)),
        vol.Required("action"): vol.In(["down", "up", "press"]),
    },
    extra=vol.PREVENT_EXTRA,
)


def _now(request: web.Request) -> float:
    return float(request.app[CLOCK_KEY]())


def _relay(request: web.Request) -> BtRelay:
    return request.app[BT_KEY]


# ------------------------------------------------------------------ channel side
def on_bt_state(app: web.Application, hub: str, f: dict[str, Any]) -> None:
    """`bt_state` frame: the hub's Bluetooth state changed (or was reported after hello)."""
    h = app[BT_KEY].hub(hub, create=True)
    if h is None:
        return
    h.supported, h.online, h.reported = (
        f["online"] is not None and f["supported"],
        f["online"],
        True,
    )
    if h.supported:
        h.state = clean_state(f.get("state"))
        h.limits = clean_limits(f.get("limits"))
    else:
        h.state = h.limits = None


def on_bt_result(app: web.Application, hub: str, r: dict[str, Any]) -> bool:
    """`bt_result` frame: complete the waiting command. True when the state changed."""
    h = app[BT_KEY].hub(hub)
    if h is None:
        return False
    changed = False
    if r.get("state") is not None and h.supported:
        st = clean_state(r["state"])
        changed, h.state = st != h.state, st
    fut = h.waiting.pop(r["id"], None)
    if fut is None:
        return changed  # not a command this add-on is waiting for
    h.born.pop(r["id"], None)
    res = {k: v for k, v in r.items() if k != "state"}
    h.done[r["id"]] = res
    while len(h.done) > RESULTS_MAX:
        h.done.popitem(last=False)
    if not fut.done():
        fut.set_result(res)
    return changed


# ------------------------------------------------------------------ editor side
def _state_or_refuse(request: web.Request, hub: str) -> HubBt:
    from .channel import CHANNEL_KEY

    h = _relay(request).hub(hub)
    st = bt_status(h, hub in request.app[CHANNEL_KEY].conns)
    if st == "unknown":
        raise error(
            409, "bt_unknown", "the integration has not reported Bluetooth for this hub yet"
        )
    if st == "offline":
        raise error(409, "hub_offline", "the hub is offline")
    if st == "unsupported":
        raise error(409, "bt_unsupported", "this hub has no Bluetooth")
    assert h is not None
    return h


def _answer(h: HubBt, cid: str, res: dict[str, Any]) -> dict[str, Any]:
    if res.get("code") == "hub_offline":
        raise error(409, "hub_offline", "the hub is offline", id=cid)
    if not res["ok"]:
        raise error(
            502,
            "hub_refused",
            str(res.get("msg") or "the hub refused")[:200],
            hub_code=res.get("code", "failed"),
            id=cid,
        )
    return {"ok": True, "id": cid, "state": h.state}


def _expire(h: HubBt, now: float) -> None:
    """A delivered command whose result never came: forget it, so it cannot hold a pending slot for ever."""
    for cid in [c for c, t in h.born.items() if now - t > PENDING_TTL_S]:
        fut = h.waiting.pop(cid, None)
        h.born.pop(cid, None)
        if fut is not None and not fut.done():
            fut.set_result(
                {"ok": False, "code": "expired", "msg": "the integration never answered"}
            )


async def _run(
    request: web.Request, hub: str, cmd: dict[str, Any], cid: str | None = None
) -> dict[str, Any]:
    """Validate, send a `bt_cmd` on the hub's channel, wait for its `bt_result`. `cid` (the caller's command id) makes a
    re-post of the same command safe: it joins the one in flight or gets the stored result, and never runs twice."""
    from .channel import CHANNEL_KEY

    relay = _relay(request)
    known = relay.hub(hub)
    if known is not None and cid is not None:
        if cid in known.done:
            return _answer(known, cid, known.done[cid])
        if cid in known.waiting:
            return await _wait(known, cid, relay.result_wait)
    h = _state_or_refuse(request, hub)
    try:
        cmd = check_command(cmd, h.limits)
    except RelayError as exc:
        raise error(422, "invalid_bt", exc.msg, bt_code=exc.code) from None
    _expire(h, _now(request))
    if len(h.waiting) >= PENDING_MAX:
        raise error(429, "bt_busy", "too many Bluetooth requests are waiting")
    cid = cid or f"c{next(relay._ids) & 0xFFFFFFF:x}"
    cmd["id"] = cid
    conn = request.app[CHANNEL_KEY].conns.get(hub)
    h.waiting[cid] = asyncio.get_running_loop().create_future()
    h.born[cid] = _now(request)
    if conn is None or not await conn.send({"t": "bt_cmd", **cmd}):
        h.waiting.pop(cid, None)
        h.born.pop(cid, None)
        raise error(409, "hub_offline", "the hub is offline", id=cid)
    return await _wait(h, cid, relay.result_wait)


async def _wait(h: HubBt, cid: str, wait: float) -> dict[str, Any]:
    fut = h.waiting.get(cid)
    if fut is None:  # the result arrived between the check and here
        if cid in h.done:
            return _answer(h, cid, h.done[cid])
        raise error(409, "hub_offline", "the hub is offline", id=cid)
    try:
        res = await asyncio.wait_for(asyncio.shield(fut), wait)
    except (
        TimeoutError
    ):  # the command was delivered and stays: a retry with the same cmd_id finds its result
        raise error(504, "hub_timeout", "Home Assistant did not answer in time", id=cid) from None
    return _answer(h, cid, res)


_CODES: dict[str, Any] = {}


async def codes(request: web.Request) -> web.Response:
    """Read-only tables the editor needs (so it never copies them): built-in command names with their HID code, the
    default command set, the default remote usage -> key map and the key names a remote's `keys` may use."""
    if not _CODES:
        from harmony_core.bt_codes import BUILTINS, DEFAULT_COMMANDS, DEFAULT_REMOTE_KEYS
        from harmony_core.keys import KEY_CODES

        _CODES["builtins"] = [
            {"name": n, "page": p, "usage": u, "default": n in DEFAULT_COMMANDS}
            for n, (p, u) in BUILTINS.items()
        ]
        _CODES["remote_keys"] = [
            {"name": n, "page": BUILTINS[n][0], "usage": BUILTINS[n][1], "key": k}
            for n, k in DEFAULT_REMOTE_KEYS
        ]
        _CODES["keys"] = [*KEY_CODES, *(f"key_{n}" for n in range(100, 110))]
    return jresp(_CODES)


async def pair(request: web.Request) -> web.Response:
    body = await read_body(request, PAIR)
    cmd = {"op": "pair", **{k: v for k, v in body.items() if k not in ("hub", "cmd_id")}}
    return jresp(await _run(request, body["hub"], cmd, body.get("cmd_id")))


async def pair_stop(request: web.Request) -> web.Response:
    body = await read_body(request, STOP)
    return jresp(await _run(request, body["hub"], {"op": "pair_stop"}, body.get("cmd_id")))


async def pick(request: web.Request) -> web.Response:
    body = await read_body(request, PICK)
    return jresp(
        await _run(request, body["hub"], {"op": "pick", "addr": body["addr"]}, body.get("cmd_id"))
    )


async def forget(request: web.Request) -> web.Response:
    body = await read_body(request, FORGET)
    cmd = {"op": "forget", "slot": body["slot"], "kind": body.get("kind", "host")}
    return jresp(await _run(request, body["hub"], cmd, body.get("cmd_id")))


async def key(request: web.Request) -> web.Response:
    body = await read_body(request, KEY)
    cmd = {"op": "key", **{k: v for k, v in body.items() if k not in ("hub", "cmd_id")}}
    return jresp(await _run(request, body["hub"], cmd, body.get("cmd_id")))


def add_routes(app: web.Application, api: str, result_wait: float = RESULT_WAIT_S) -> None:
    app[BT_KEY] = BtRelay(result_wait)
    app.router.add_get(f"{api}/bt/codes", codes)
    app.router.add_post(f"{api}/bt/pair", pair)
    app.router.add_post(f"{api}/bt/pair/stop", pair_stop)
    app.router.add_post(f"{api}/bt/pick", pick)
    app.router.add_post(f"{api}/bt/forget", forget)
    app.router.add_post(f"{api}/bt/key", key)

    async def _stop(_: web.Application) -> None:
        for h in app[BT_KEY].hubs.values():
            fail_waiting(h, "shutdown")

    app.on_shutdown.append(_stop)
