"""Page format v2 over the simulated RF link: remote_info (cache, ws broadcast, resend after hello_ack, cleared on link
loss), tile_state vs tile_update, and v2 pages through the hub to `remote_sim` (which shares the C decoder)."""

import base64
import json

import aiohttp
from conftest import RemoteDump  # noqa: F401  (fixture types)
from test_hub_engine import TOKEN, call, env, hello, recv
from test_page_e2e import finish, start_remote  # noqa: F401  (fixture)

from custom_components.ha_harmony.harmony_core.page_codec import Page, Widget, encode

PAGE = Page(
    2,
    2,
    3,
    (
        Widget("button", 1, 0, 0, 2, 1, "Watch", role="primary", icon=1, bind=0),
        Widget("tile", 2, 0, 1, 2, 1, "Lamp", icon=38),
        Widget("button", 3, 0, 2, 1, 1, "Next", role="nav", icon=8, bind=0),
    ),
    version=2,
    gen=7,
    total=4,
    kind="favorites",
    seq_idx=1,
    seq_cnt=2,
    act=1,
    title="Living room",
)
RAW = encode(PAGE, 2)


async def push(ws, data: bytes, page=2, id=5):
    r = await call(
        ws, "page_push", {"remote": 1, "page": page, "data": base64.b64encode(data).decode()}, id=id
    )
    assert r["t"] == "command_result" and r["d"]["ok"] is True


async def tile(ws, text, state=None, id=6):
    args = {"remote": 1, "page": 2, "tile": 2, "text": text}
    if state is not None:
        args["state"] = state
    r = await call(ws, "command", {"device": "hub", "command": "tile_update", "args": args}, id=id)
    assert r["t"] == "command_result" and r["d"]["ok"] is True


async def hello_collect(ws):
    """hello, then everything up to and including the state_update: the messages in between."""
    ack = await call(ws, "hello", {"token": TOKEN})
    assert ack["t"] == "hello_ack"
    between = []
    while True:
        m = await recv(ws, keep_attached=True)
        if m["t"] == "state_update":
            return between
        between.append(m)


async def test_remote_info_is_broadcast_cached_and_resent(hub_rf, start_remote):
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_rf.url) as ws1:
        await hello(ws1)
        r = start_remote(hub_rf.rf, "--info", "3", "--dump-layout", "--settle-ms", "300")
        r.wait_attached()
        msgs = [await recv(ws1, keep_attached=True) for _ in range(2)]
        assert [m["t"] for m in msgs] == ["remote_attached", "remote_info"]
        assert msgs[1]["id"] == 0
        assert msgs[1]["d"] == {"remote": 1, "page_fmt": 2, "caps": 3, "icons": 50}
        # a client connecting later gets it right after hello_ack
        async with s.ws_connect(hub_rf.url) as ws2:
            between = await hello_collect(ws2)
            assert [m["t"] for m in between] == ["remote_info"]
            assert between[0]["d"] == {"remote": 1, "page_fmt": 2, "caps": 3, "icons": 50}
        await push(ws1, RAW)
        code, out, _ = finish(r)
        assert code == 0
    # the remote has gone: its remote_info is stale and no new client sees it
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_rf.url) as ws3:
        for _ in range(50):
            between = await hello_collect(ws3)
            if not between:
                break
            await ws3.close()
            ws3 = await s.ws_connect(hub_rf.url)
        assert between == []


async def test_v2_page_and_tile_state_reach_a_capable_remote(hub_rf, start_remote):
    r = start_remote(hub_rf.rf, "--info", "3", "--dump-layout", "--settle-ms", "600")
    r.wait_attached()
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_rf.url) as ws:
        await ws.send_str(env("hello", {"token": TOKEN}, 1))
        while (await recv(ws, keep_attached=True))["t"] != "state_update":
            pass
        await push(ws, RAW)
        await tile(ws, "On", state=2)
    code, out, err = finish(r)
    assert code == 0, err
    lay = json.loads(out)
    assert lay["version"] == 2 and lay["gen"] == 7 and lay["total"] == 4
    assert lay["pkind"] == "favorites" and lay["seq"] == {"idx": 1, "cnt": 2} and lay["act"] == 1
    assert lay["title"] == "Living room" and lay["back"] is False
    w = {x["id"]: x for x in lay["widgets"]}
    assert w[1]["role"] == "primary" and w[1]["icon"] == 1 and w[1]["bind"] == 0
    assert w[3]["role"] == "nav" and w[3]["bind"] == 0 and w[2]["bind"] is None
    assert w[2]["text"] == "On" and w[2]["state"] == 2  # tile_state: text and state arrived


async def test_state_arg_is_ignored_for_a_remote_without_the_cap(hub_rf, start_remote):
    r = start_remote(hub_rf.rf, "--info", "2", "--dump-layout", "--settle-ms", "600")  # page store only
    r.wait_attached()
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_rf.url) as ws:
        await ws.send_str(env("hello", {"token": TOKEN}, 1))
        while (await recv(ws, keep_attached=True))["t"] != "state_update":
            pass
        await push(ws, RAW)
        await tile(ws, "21 C", state=2)
    code, out, _ = finish(r)
    assert code == 0
    t = {x["id"]: x for x in json.loads(out)["widgets"]}[2]
    assert t["text"] == "21 C" and "state" not in t  # legacy tile_update: text only


async def test_without_remote_info_everything_stays_version_one(hub_rf, start_remote):
    r = start_remote(hub_rf.rf, "--dump-layout", "--settle-ms", "400")
    r.wait_attached()
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_rf.url) as ws:
        between = await hello_collect(ws)
        assert between == []  # nothing announced: no remote_info
        await push(ws, RAW)
        await tile(ws, "5 C", state=1)
    code, out, _ = finish(r)
    t = {x["id"]: x for x in json.loads(out)["widgets"]}[2]
    assert code == 0 and t["text"] == "5 C"


async def test_bad_state_arg_is_refused(hub_rf, start_remote):
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_rf.url) as ws:
        await hello(ws)
        r = await call(
            ws,
            "command",
            {
                "device": "hub",
                "command": "tile_update",
                "args": {"remote": 1, "page": 2, "tile": 2, "text": "x", "state": 9},
            },
            id=8,
        )
        assert r["d"]["ok"] is False
