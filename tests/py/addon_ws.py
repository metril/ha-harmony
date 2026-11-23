"""Helpers for tests that play the integration (channel) or the editor (live) over WebSockets. No sleeps: every wait is a
message with a timeout."""

import asyncio

TOKEN = "ab" * 32
BEARER = {"Authorization": f"Bearer {TOKEN}"}
WAIT = 5.0


async def connect(c, hub, *, hello=True, **fields):
    """Open the channel for `hub` and send hello (returns the socket)."""
    ws = await c.ws_connect(f"/api/v1/channel?hub={hub}", headers=BEARER)
    if hello:
        await ws.send_json({"t": "hello", "v": 1, "hub": hub, **fields})
    return ws


async def recv(ws, t=None, timeout=WAIT):
    """The next frame (of type `t`, skipping others when given)."""
    while True:
        msg = await asyncio.wait_for(ws.receive_json(), timeout)
        if t is None or msg.get("t") == t:
            return msg


async def drain(ws):
    """Everything the server sent so far: a junk frame is answered `error bad_frame` after everything queued before it."""
    await ws.send_json({"nope": 1})
    out = []
    while True:
        m = await recv(ws)
        if m == {"t": "error", "code": "bad_frame"}:
            return out
        out.append(m)


async def closed(ws, timeout=WAIT):
    """The close code once the server closes the socket."""
    while True:
        msg = await asyncio.wait_for(ws.receive(), timeout)
        if msg.type.name == "CLOSE":
            return msg.data  # the code in the close frame (ws.close_code is set only after the client's own close finishes)
        if msg.type.name in ("CLOSING", "CLOSED"):
            return ws.close_code


async def live(c, *topics, headers=None):
    ws = await c.ws_connect("/api/v1/live", headers=headers or {})
    if topics:
        await ws.send_json({"sub": list(topics)})
    return ws


BT_LIMITS = {
    "hosts_max": 4,
    "conn_max": 2,
    "remotes_max": 2,
    "le_bonds_max": 8,
    "pages": [7, 12],
    "le": True,
}
BT_STATE = {
    "state": "ready",
    "discoverable": False,
    "gen": 3,
    "hosts": [
        {"slot": 0, "addr": "aa:bb:cc:dd:ee:01", "name": "TV", "transport": "le", "connected": True}
    ],
    "remotes": [{"slot": 0, "addr": "11:22:33:44:55:01", "connected": False, "battery": 80}],
}


async def bt_online(ws, **over):
    await ws.send_json(
        {
            "t": "bt_state",
            "online": True,
            "supported": True,
            "state": BT_STATE,
            "limits": BT_LIMITS,
            **over,
        }
    )


async def get_bundle(c, hub="default"):
    """What the integration is sent on hello: the bundle body, or None when nothing is applied."""
    ws = await connect(c, hub)
    try:
        return (await recv(ws)).get("body")
    finally:
        await ws.close()
