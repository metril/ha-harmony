"""Bluetooth HID messages 35-40 and hello_ack.bt: codec vectors, limits, old-client tolerance, and the sim hub
(which has no Bluetooth: hello_ack without `bt`, every bt_* request refused, unauthenticated sockets get `error`)."""

import json
from pathlib import Path

import aiohttp
import pytest
from conftest import TOKEN

from harmony_core.harmony_proto import MESSAGES, MsgType, ProtoError, decode_ws, decode_ws_full, encode_ws

VEC = json.loads((Path(__file__).parents[1] / "vectors" / "bt_messages.json").read_text())
NEW = ("bt_pair", "bt_state", "bt_forget", "bt_key", "bt_remote_key", "bt_listen")


def test_codes_and_transport():
    assert [MsgType[n.upper()] for n in NEW] == [35, 36, 37, 38, 39, 40]
    assert all(MESSAGES[n][1] == "ws" for n in NEW)
    assert len({m[0] for m in MESSAGES.values()}) == len(MESSAGES)  # codes unique
    assert {30, 31, 32, 33, 34} <= {m[0] for m in MESSAGES.values()}  # remote OTA codes still reserved


@pytest.mark.parametrize("v", VEC["valid"], ids=lambda v: f"{v['env']['t']}-{v['env']['id']}")
def test_valid_round_trip(v):
    env = v["env"]
    mid, mtype, data = decode_ws_full(env)
    assert (mid, mtype.name.lower(), data) == (env["id"], env["t"], env["d"])
    assert encode_ws(mtype, data, mid) == env


@pytest.mark.parametrize("v", VEC["invalid"], ids=lambda v: v["why"])
def test_invalid_refused(v):
    env = v["env"]
    with pytest.raises(ProtoError):
        decode_ws(env)
    with pytest.raises(ProtoError):
        encode_ws(MsgType[env["t"].upper()], env["d"], env["id"])


def test_old_and_new_peers_ignore_unknown_fields():
    # a newer hub adding fields to bt_state / hello_ack.bt: receivers ignore them
    env = {"v": 1, "id": 0, "t": "bt_state", "d": {"state": "ready", "discoverable": False, "hosts": [], "future": 1}}
    assert decode_ws(env)[1] == {"state": "ready", "discoverable": False, "hosts": []}
    ack = {"v": 1, "id": 1, "t": "hello_ack", "d": {"hub_id": "h", "bt": {"hosts_max": 4, "later": [1]}}}
    assert decode_ws(ack)[1]["bt"] == {"hosts_max": 4, "later": [1]}
    # an old hub's hello_ack has no bt; an old hub's bt_state has no remotes / le
    assert "bt" not in decode_ws({"v": 1, "id": 1, "t": "hello_ack", "d": {"hub_id": "h"}})[1]
    old = {"v": 1, "id": 0, "t": "bt_state", "d": {"state": "off", "discoverable": False, "hosts": []}}
    assert decode_ws(old)[1].keys() == {"state", "discoverable", "hosts"}
    # an old client's bt_pair / bt_forget (no mode, kind, transport) is still valid
    assert decode_ws({"v": 1, "id": 1, "t": "bt_pair", "d": {"on": True, "future": 1}})[1] == {"on": True}
    # candidates / addr: an old hub's bt_state has neither, an old client's bt_pair has no addr; unknown fields inside
    # a candidate pass through (json field)
    cand = {"state": "ready", "discoverable": False, "hosts": [], "candidates": [{"addr": "AA:BB:CC:DD:EE:01", "rssi": -50, "x": 1}]}
    assert decode_ws({"v": 1, "id": 0, "t": "bt_state", "d": cand})[1] == cand
    pick = {"on": True, "mode": "remote", "addr": "AA:BB:CC:DD:EE:01"}
    assert decode_ws({"v": 1, "id": 1, "t": "bt_pair", "d": pick})[1] == pick
    assert "addr" not in decode_ws({"v": 1, "id": 1, "t": "bt_pair", "d": {"on": True, "mode": "remote"}})[1]
    # senders stay strict
    with pytest.raises(ProtoError):
        encode_ws(MsgType.BT_FORGET, {"slot": 1, "extra": 1})


async def _session(hub, *, authed):
    s = aiohttp.ClientSession()
    ws = await s.ws_connect(hub.url)
    ack = None
    if authed:
        await ws.send_json({"v": 1, "id": 1, "t": "hello", "d": {"token": TOKEN}})
        ack = await ws.receive_json()
        await ws.receive_json()  # state_update
    return s, ws, ack


BT_REQUESTS = [
    ("bt_pair", {"on": True}),
    ("bt_forget", {"slot": 0}),
    ("bt_key", {"slot": 0, "page": 7, "usage": 4, "action": "press"}),
    ("bt_listen", {"on": True}),
]


async def test_sim_hub_without_bluetooth(start_hub):
    hub = start_hub()
    s, ws, ack = await _session(hub, authed=True)
    async with s, ws:
        assert ack["t"] == "hello_ack" and "bt" not in ack["d"]
        for i, (t, d) in enumerate(BT_REQUESTS, start=2):
            await ws.send_json({"v": 1, "id": i, "t": t, "d": d})
            rep = await ws.receive_json()
            assert (rep["t"], rep["id"], rep["d"]["code"]) == ("error", i, "unsupported")


async def test_bt_requests_need_hello(start_hub):
    hub = start_hub()
    extra = [("bt_state", {"state": "off", "discoverable": False, "hosts": []})]
    for i, (t, d) in enumerate([*BT_REQUESTS, *extra], start=1):
        s, ws, _ = await _session(hub, authed=False)  # the hub closes an unauthenticated socket after its error
        async with s, ws:
            await ws.send_json({"v": 1, "id": i, "t": t, "d": d})
            rep = await ws.receive_json()
            assert (rep["t"], rep["id"], rep["d"]["code"]) == ("error", i, "unauthorized")
