"""Bluetooth relay for the editor (docs/addon-api.md "Bluetooth relay").

The add-on never talks to the hub. The editor's requests become commands in a small per-hub queue; the integration
fetches them with its next `POST bundle/bt` exchange (which also reports the hub's `bt_state`, the hub's limits and the
outcome of earlier commands), runs them against the hub and reports back. Everything here is bounded: queue, results,
key-event ring, number of hubs. The integration is the authority on what the hub accepts and re-validates every
command; the checks here only keep obvious garbage from being queued.
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
    MAX_COMMANDS,
    RelayError,
    check_command,
    clean_limits,
    clean_state,
)

from .linked import MAX_LINKED
from .schemas import _hub
from .web_util import CLOCK_KEY, error, jresp, query, read_body

BT_KEY = web.AppKey("bt_relay", object)
STALE_S = 30  # a report older than this: the integration (or Home Assistant) is not there
ACTIVE_S = 10  # the integration polls fast this long after the editor last sent a command
LISTEN_S = 30  # a listen request lapses unless the editor keeps asking (listen / events)
RESULT_WAIT_S = 20.0  # how long an editor request waits for the integration to run its command
PENDING_TTL_S = 60  # a delivered command's slot is freed after this, answered or not
EVENTS_MAX = 64
RESULTS_MAX = 32
PENDING_MAX = 16  # commands queued or in flight per hub
_ID = re.compile(r"^[A-Za-z0-9_-]{1,16}$")
_CODE = re.compile(r"^[a-z_]{1,32}$")


@dataclass
class HubBt:
    state: dict[str, Any] | None = None
    limits: dict[str, Any] | None = None
    supported: bool = False
    online: bool = False
    reported: float | None = None
    queue: collections.deque[dict[str, Any]] = field(default_factory=collections.deque)
    waiting: dict[str, asyncio.Future[dict[str, Any]]] = field(default_factory=dict)
    born: dict[str, float] = field(default_factory=dict)  # command id -> when it was queued
    done: collections.OrderedDict[str, dict[str, Any]] = field(
        default_factory=collections.OrderedDict
    )  # recent results by id
    events: collections.deque[dict[str, Any]] = field(
        default_factory=lambda: collections.deque(maxlen=EVENTS_MAX)
    )
    seq: int = 0
    listen_until: float = 0.0
    active_until: float = 0.0


class BtRelay:
    def __init__(self) -> None:
        self.hubs: dict[str, HubBt] = {}
        self._ids = itertools.count(1)

    def hub(self, hub: str, create: bool = False) -> HubBt | None:
        h = self.hubs.get(hub)
        if h is None and create and len(self.hubs) < MAX_LINKED:
            h = self.hubs[hub] = HubBt()
        return h

    def prune(self, linked: set[str]) -> None:
        """Drop hubs nothing polls for any more (a rename, a removed integration); their waiters are told."""
        for hub in [k for k in self.hubs if k not in linked]:
            _wake(self.hubs.pop(hub))

    @staticmethod
    def status(h: HubBt | None, now: float) -> str:
        if h is None or h.reported is None:
            return "unknown"
        if now - h.reported > STALE_S or not h.online:
            return "offline"
        return "ok" if h.supported else "unsupported"


# ------------------------------------------------------------------ schemas
_RESULT = vol.Schema(
    {
        vol.Required("id"): vol.All(str, vol.Match(_ID)),
        vol.Required("ok"): bool,
        vol.Optional("code"): vol.All(str, vol.Match(_CODE)),
        vol.Optional("msg"): vol.All(str, vol.Length(max=200)),
    },
    extra=vol.PREVENT_EXTRA,
)
_EVENT = vol.Schema(
    {
        vol.Required("slot"): vol.All(int, vol.Range(min=0, max=255)),
        vol.Required("page"): vol.All(int, vol.Range(min=0, max=255)),
        vol.Required("usage"): vol.All(int, vol.Range(min=0, max=65535)),
        vol.Required("down"): bool,
    },
    extra=vol.PREVENT_EXTRA,
)
EXCHANGE = vol.Schema(
    {
        vol.Required("hub"): _hub,
        vol.Required("supported"): bool,
        vol.Required("online"): bool,
        vol.Optional("state"): vol.Any(None, dict),
        vol.Optional("limits"): vol.Any(None, dict),
        vol.Optional("results", default=list): vol.All([_RESULT], vol.Length(max=RESULTS_MAX)),
        vol.Optional("events", default=list): vol.All([_EVENT], vol.Length(max=EVENTS_MAX)),
    },
    extra=vol.PREVENT_EXTRA,
)
_HUB_ONLY = {vol.Optional("hub", default="default"): _hub}
STATUS_QUERY = vol.Schema(_HUB_ONLY, extra=vol.PREVENT_EXTRA)
_CMD = {
    **_HUB_ONLY,
    vol.Optional("cmd_id"): vol.All(str, vol.Match(_ID)),
}  # the caller's id: a re-post is not a second command
EVENTS_QUERY = vol.Schema(
    {**_HUB_ONLY, vol.Optional("after", default="0"): vol.All(str, vol.Match(r"^[0-9]{1,12}$"))},
    extra=vol.PREVENT_EXTRA,
)
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
LISTEN = vol.Schema({**_HUB_ONLY, vol.Required("on"): bool}, extra=vol.PREVENT_EXTRA)


def _now(request: web.Request) -> float:
    return float(request.app[CLOCK_KEY]())


def _relay(request: web.Request) -> BtRelay:
    return request.app[BT_KEY]


def _wake(h: HubBt) -> None:
    for fut in h.waiting.values():
        if not fut.done():
            fut.set_result({"ok": False, "code": "shutdown", "msg": "the add-on is stopping"})


# ------------------------------------------------------------------ integration side
async def exchange(request: web.Request) -> web.Response:
    """Bearer: the integration reports and collects. Body `{hub, supported, online, state?, limits?, results, events}`;
    answer `{commands, listen, fast}`."""
    body = await read_body(request, EXCHANGE)
    relay, hub, now = _relay(request), body["hub"], _now(request)
    from .app import LINKED_KEY  # app.py imports this module

    if hub not in request.app[LINKED_KEY].ids():
        raise error(404, "not_linked", "no Home Assistant integration polls for that hub id")
    relay.prune(set(request.app[LINKED_KEY].ids()))  # a renamed or forgotten hub id must not linger
    h = relay.hub(hub, create=True)
    if h is None:
        raise error(429, "rate_limited", "too many hubs")
    h.supported, h.online, h.reported = body["supported"], body["online"], now
    if body["supported"]:
        h.state = clean_state(body.get("state"))
        h.limits = clean_limits(body.get("limits"))
    else:
        h.state = h.limits = None
    for r in body["results"]:
        fut = h.waiting.pop(r["id"], None)
        if fut is None:
            continue  # not a command this add-on is waiting for
        h.born.pop(r["id"], None)
        h.done[r["id"]] = r
        while len(h.done) > RESULTS_MAX:
            h.done.popitem(last=False)
        if not fut.done():
            fut.set_result(r)
    if now < h.listen_until:
        for ev in body["events"]:
            h.seq += 1
            h.events.append({"seq": h.seq, **ev, "t": int(now)})
    commands = [h.queue.popleft() for _ in range(min(MAX_COMMANDS, len(h.queue)))]
    window = bool(h.state and h.state.get("window"))
    return jresp(
        {
            "commands": commands,
            "listen": now < h.listen_until,
            # fast only for a reason: a pairing window, a listen lease, or a command recently asked for or in flight
            "fast": window
            or now < h.listen_until
            or now < h.active_until
            or bool(h.queue)
            or bool(h.waiting),
        }
    )


# ------------------------------------------------------------------ editor side
def _state_or_refuse(request: web.Request, hub: str) -> HubBt:
    relay, now = _relay(request), _now(request)
    h = relay.hub(hub)
    st = relay.status(h, now)
    if st == "unknown":
        raise error(
            409, "bt_unknown", "the integration has not reported Bluetooth for this hub yet"
        )
    if st == "offline":
        raise error(409, "hub_offline", "the hub is offline")
    if st == "unsupported":
        raise error(409, "bt_unsupported", "this hub has no Bluetooth")
    assert h is not None
    h.active_until = now + ACTIVE_S
    return h


def _answer(h: HubBt, cid: str, res: dict[str, Any]) -> dict[str, Any]:
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
    """Validate, queue for the integration's next exchange, wait for the outcome. `cid` (the caller's command id) makes a
    re-post of the same command safe: it joins the one in flight or gets the stored result, and never runs twice."""
    known = _relay(request).hub(hub)
    if known is not None and cid is not None:
        if cid in known.done:
            return _answer(known, cid, known.done[cid])
        if cid in known.waiting:
            return await _wait(known, cid)
    h = _state_or_refuse(request, hub)
    try:
        cmd = check_command(cmd, h.limits)
    except RelayError as exc:
        raise error(422, "invalid_bt", exc.msg, bt_code=exc.code) from None
    _expire(h, _now(request))
    if len(h.waiting) >= PENDING_MAX:  # every queued command is also waiting for its result
        raise error(429, "bt_busy", "too many Bluetooth requests are waiting")
    cid = cid or f"c{next(_relay(request)._ids) & 0xFFFFFFF:x}"
    cmd["id"] = cid
    h.waiting[cid] = asyncio.get_running_loop().create_future()
    h.born[cid] = _now(request)
    h.queue.append(cmd)
    return await _wait(h, cid)


async def _wait(h: HubBt, cid: str) -> dict[str, Any]:
    fut = h.waiting.get(cid)
    if fut is None:  # the result arrived between the check and here
        return _answer(h, cid, h.done[cid])
    try:
        res = await asyncio.wait_for(asyncio.shield(fut), RESULT_WAIT_S)
    except TimeoutError:
        queued = [c for c in h.queue if c.get("id") == cid]
        for c in (
            queued
        ):  # never delivered: withdraw it. A delivered one stays, so a retry finds its result.
            h.queue.remove(c)
            h.waiting.pop(cid, None)
            h.born.pop(cid, None)
        raise error(504, "hub_timeout", "Home Assistant did not answer in time", id=cid) from None
    return _answer(h, cid, res)


async def status(request: web.Request) -> web.Response:
    q = query(request, STATUS_QUERY)
    relay, now = _relay(request), _now(request)
    h = relay.hub(q["hub"])
    st = relay.status(
        h, now
    )  # a plain status poll does not speed the integration up: it is fresh to within 5 s
    return jresp(
        {
            "hub": q["hub"],
            "status": st,
            "state": h.state if h else None,
            "limits": h.limits if h else None,
            "listening": bool(h and now < h.listen_until),
            "age_s": None if h is None or h.reported is None else int(now - h.reported),
        }
    )


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


async def listen(request: web.Request) -> web.Response:
    """Lease: the integration listens to the hub's remote keys while the editor keeps asking (`listen on`, `events`)."""
    body = await read_body(request, LISTEN)
    h = _state_or_refuse(request, body["hub"])
    now = _now(request)
    h.listen_until = now + LISTEN_S if body["on"] else 0.0
    if body["on"]:
        h.active_until = now + ACTIVE_S
    return jresp({"ok": True, "listening": body["on"]})


async def events(request: web.Request) -> web.Response:
    q = query(request, EVENTS_QUERY)
    relay, now = _relay(request), _now(request)
    h = relay.hub(q["hub"])
    after = int(q["after"])
    if h is None:
        return jresp({"events": [], "last": after, "listening": False, "dropped": False})
    if now < h.listen_until:  # polling keeps the listen alive
        h.listen_until = now + LISTEN_S
    out = [e for e in h.events if e["seq"] > after]
    first = h.events[0]["seq"] if h.events else h.seq + 1
    return jresp(
        {
            "events": out,
            "last": h.seq,
            "listening": now < h.listen_until,
            "dropped": after + 1 < first
            and h.seq > after,  # the ring overwrote events the caller never saw
        }
    )


def add_routes(app: web.Application, api: str) -> None:
    app[BT_KEY] = BtRelay()
    app.router.add_get(f"{api}/bt/status", status)
    app.router.add_get(f"{api}/bt/codes", codes)
    app.router.add_post(f"{api}/bt/pair", pair)
    app.router.add_post(f"{api}/bt/pair/stop", pair_stop)
    app.router.add_post(f"{api}/bt/pick", pick)
    app.router.add_post(f"{api}/bt/forget", forget)
    app.router.add_post(f"{api}/bt/key", key)
    app.router.add_post(f"{api}/bt/listen", listen)
    app.router.add_get(f"{api}/bt/events", events)
    app.router.add_post(f"{api}/bundle/bt", exchange)

    async def _stop(_: web.Application) -> None:
        for h in app[BT_KEY].hubs.values():
            _wake(h)

    app.on_shutdown.append(_stop)
