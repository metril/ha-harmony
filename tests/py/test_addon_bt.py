"""The add-on's Bluetooth relay: editor routes (ingress) <-> the integration's channel (bearer WebSocket). No hub, no Home
Assistant: a test plays the integration with raw frames and the editor with HTTP POSTs and /live."""

import asyncio

import pytest
from addon_ws import BT_LIMITS, BT_STATE, TOKEN, bt_online, connect, drain, live, recv
from test_addon import H

from harmony_addon import bt_routes
from harmony_addon.app import create_app
from harmony_addon.auth import NoAuth

HUB = "h1"


class Clock:
    t = 1000.0

    def __call__(self):
        return self.t


@pytest.fixture
def clock():
    return Clock()


@pytest.fixture
async def c(aiohttp_client, tmp_path, clock):
    app = create_app(
        tmp_path,
        auth=NoAuth(),
        bundle_token=TOKEN,
        bundle_networks=None,
        clock=clock,
        hello_timeout=0.3,
        bt_result_wait=2.0,
    )
    return await aiohttp_client(app)


@pytest.fixture
async def ws(c):
    """The integration for HUB, connected, with Bluetooth reported."""
    sock = await connect(c, HUB)
    await recv(sock, "nothing_applied")
    await bt_online(sock)
    await drain(sock)
    yield sock
    await sock.close()


async def post(c, path, **body):
    r = await c.post(f"/api/v1/bt/{path}", json={"hub": HUB, **body}, headers=H)
    return r.status, await r.json()


async def run(c, ws, path, answer=None, **body):
    """Post a command and answer the `bt_cmd` the integration receives. -> (status, body, cmd)."""
    job = asyncio.ensure_future(post(c, path, **body))
    cmd = await recv(ws, "bt_cmd")
    await ws.send_json({"t": "bt_result", "id": cmd["id"], "ok": True, **(answer or {})})
    code, resp = await job
    return code, resp, cmd


async def bt_topic(c, hub=HUB):
    """A /live socket on `bt:<hub>` and its snapshot."""
    lv = await live(c, f"bt:{hub}")
    return lv, (await recv(lv, "snapshot"))["topics"][f"bt:{hub}"]


# ------------------------------------------------------------------ status (pushed on /live)
async def test_status_unknown_until_the_integration_reports(c):
    lv, snap = await bt_topic(c)
    assert snap["status"] == "unknown" and snap["state"] is None and snap["limits"] is None
    assert snap["listening"] is False and "age_s" not in snap
    code, body = await post(c, "pair", mode="host")
    assert code == 409 and body["error"]["code"] == "bt_unknown"
    sock = await connect(c, HUB)  # connected, but Bluetooth not reported yet
    await drain(sock)
    assert (await post(c, "pair"))[1]["error"]["code"] == "bt_unknown"
    await sock.close()
    await lv.close()


async def test_status_ok_offline_unsupported_and_channel_closed(c):
    lv, _ = await bt_topic(c)
    sock = await connect(c, HUB)
    assert (await recv(lv, "bt"))["status"] == "unknown"  # connected, Bluetooth not reported yet
    await bt_online(sock)
    d = await recv(lv, "bt")
    assert d["status"] == "ok" and d["limits"]["hosts_max"] == 4 and d["hub"] == HUB
    assert d["state"]["hosts"][0]["addr"] == "AA:BB:CC:DD:EE:01"  # normalised
    await sock.send_json({"t": "bt_state", "online": False, "supported": True})
    assert (await recv(lv, "bt"))["status"] == "offline"
    assert (await post(c, "key", slot=0, page=7, usage=4, action="press"))[1]["error"][
        "code"
    ] == "hub_offline"
    await sock.send_json({"t": "hub", "online": True})  # the hub link came back
    assert (await recv(lv, "bt"))["status"] == "ok"
    await sock.send_json({"t": "hub", "online": False})
    assert (await recv(lv, "bt"))["status"] == "offline"
    await bt_online(sock, supported=False, state=None, limits=None)
    d = await recv(lv, "bt")
    assert d["status"] == "unsupported" and d["state"] is None
    code, body = await post(c, "pair")
    assert code == 409 and body["error"]["code"] == "bt_unsupported"
    await bt_online(sock)
    assert (await recv(lv, "bt"))["status"] == "ok"
    await sock.close()  # the channel closes: offline, pushed
    assert (await recv(lv, "bt"))["status"] == "offline"
    assert (await post(c, "pair"))[1]["error"]["code"] == "hub_offline"
    await lv.close()


async def test_reported_state_is_cleaned(c):
    lv, _ = await bt_topic(c)
    sock = await connect(c, HUB)
    await recv(lv, "bt")  # the hello
    junk = {
        "state": "ready",
        "evil": "x",
        "hosts": [{"slot": 999, "addr": "aa:bb:cc:dd:ee:01"}, {"slot": 1, "addr": "nope"}, "x"]
        + [{"slot": 2, "addr": "AA:BB:CC:DD:EE:02", "name": "n\x00l"}],
        "candidates": [{"addr": "AA:BB:CC:DD:EE:03", "rssi": 500}],
    }
    await bt_online(sock, state=junk)
    st = (await recv(lv, "bt"))["state"]
    assert "evil" not in st
    assert [h["slot"] for h in st["hosts"]] == [2] and "name" not in st["hosts"][0]
    assert st["candidates"][0]["rssi"] == -127
    await bt_online(sock, state={"state": "weird"})
    assert (await recv(lv, "bt"))["state"] is None
    await sock.close()
    await lv.close()


async def test_bad_bluetooth_frames_are_refused_and_change_nothing(c, ws):
    for bad in (
        {"t": "bt_state", "supported": "yes", "online": True},
        {"t": "bt_state", "online": True},
        {"t": "bt_result", "id": "../x", "ok": True},
        {"t": "bt_result", "id": "a", "ok": "yes"},
        {"t": "bt_key", "slot": 1, "page": 7, "usage": 70000, "down": True},
        {"t": "hub", "online": "no"},
    ):
        await ws.send_json(bad)
        assert (await recv(ws))["code"] == "bad_frame", bad
    lv, snap = await bt_topic(c)
    assert snap["status"] == "ok"
    await lv.close()


async def test_a_result_for_nothing_in_flight_is_ignored(c, ws):
    await ws.send_json({"t": "bt_result", "id": "nobody", "ok": True})
    assert await drain(ws) == []


async def test_a_result_carrying_state_updates_the_editor(c, ws):
    lv, _ = await bt_topic(c)
    job = asyncio.ensure_future(post(c, "pair", mode="remote"))
    cmd = await recv(ws, "bt_cmd")
    await ws.send_json(
        {"t": "bt_result", "id": cmd["id"], "ok": True, "state": {**BT_STATE, "window": "remote"}}
    )
    code, body = await job
    assert code == 200 and body["state"]["window"] == "remote"
    assert (await recv(lv, "bt"))["state"]["window"] == "remote"
    await lv.close()


# ------------------------------------------------------------------ commands
async def test_pair_round_trip_and_defaults(c, ws):
    code, body, cmd = await run(c, ws, "pair", mode="remote", transport="le", secs=60)
    assert code == 200 and body["ok"] is True and body["state"]["state"] == "ready"
    assert (
        cmd["op"] == "pair"
        and cmd["mode"] == "remote"
        and cmd["secs"] == 60
        and cmd["id"] == body["id"]
    )
    assert (await run(c, ws, "pair"))[2]["secs"] == 120  # defaults filled in


async def test_every_command_reaches_the_integration_normalised(c, ws):
    sent = {}
    for path, body, op in (
        ("pair/stop", {}, "pair_stop"),
        ("pick", {"addr": "aa:bb:cc:dd:ee:09"}, "pick"),
        ("forget", {"slot": 1, "kind": "remote"}, "forget"),
        ("forget", {"slot": 2}, "forget"),
        ("key", {"slot": 3, "page": 12, "usage": 233, "action": "down"}, "key"),
    ):
        code, _, cmd = await run(c, ws, path, **body)
        assert code == 200 and cmd["op"] == op
        sent.setdefault(op, []).append(cmd)
    assert sent["pick"][0]["addr"] == "AA:BB:CC:DD:EE:09"
    assert sent["forget"][1]["kind"] == "host" and sent["forget"][0]["kind"] == "remote"
    assert {k: sent["key"][0][k] for k in ("slot", "page", "usage", "action")} == {
        "slot": 3,
        "page": 12,
        "usage": 233,
        "action": "down",
    }


async def test_hub_refusal_is_reported_with_its_code(c, ws):
    job = asyncio.ensure_future(post(c, "pick", addr="AA:BB:CC:DD:EE:09"))
    cmd = await recv(ws, "bt_cmd")
    await ws.send_json(
        {"t": "bt_result", "id": cmd["id"], "ok": False, "code": "no_candidate", "msg": "gone"}
    )
    code, body = await job
    assert (
        code == 502
        and body["error"]["code"] == "hub_refused"
        and body["error"]["hub_code"] == "no_candidate"
    )


@pytest.mark.parametrize(
    ("path", "body"),
    [
        ("pair", {"mode": "both"}),
        ("pair", {"secs": 301}),
        ("pair", {"secs": 0}),
        ("pair", {"transport": "wifi"}),
        ("pair", {"extra": 1}),
        ("pick", {"addr": "AA:BB"}),
        ("pick", {}),
        ("forget", {"slot": 255}),
        ("forget", {"slot": -1}),
        ("forget", {"slot": 1, "kind": "x"}),
        ("key", {"slot": 0, "page": 9, "usage": 4, "action": "press"}),
        ("key", {"slot": 0, "page": 7, "usage": 70000, "action": "press"}),
        ("key", {"slot": 0, "page": 7, "usage": 4, "action": "tap"}),
        ("key", {"slot": 0, "page": True, "usage": 4, "action": "press"}),
        ("listen", {}),
    ],
)
async def test_invalid_requests_are_refused_before_anything_is_sent(c, ws, path, body):
    code, _ = await post(c, path, **body)
    assert code in (400, 404)  # `listen` is gone altogether
    assert await drain(ws) == []


async def test_hub_limits_tighten_the_checks(c, ws):
    code, body = await post(c, "forget", slot=4)  # hosts_max is 4
    assert code == 422 and body["error"]["bt_code"] == "bad_slot"
    assert (await post(c, "forget", slot=2, kind="remote"))[0] == 422  # remotes_max is 2
    await bt_online(ws, limits={**BT_LIMITS, "le": False})
    await drain(ws)
    assert (await post(c, "pair", transport="le"))[1]["error"]["bt_code"] == "no_le"
    await bt_online(ws, limits={**BT_LIMITS, "remotes_max": 0})
    await drain(ws)
    assert (await post(c, "pair", mode="remote"))[1]["error"]["bt_code"] == "unsupported"
    assert await drain(ws) == []


async def test_a_command_nobody_answers_times_out_and_the_retry_finds_the_late_result(c, ws):
    c.app[bt_routes.BT_KEY].result_wait = 0.1
    job = asyncio.ensure_future(post(c, "pair/stop", cmd_id="late-1"))
    cmd = await recv(ws, "bt_cmd")  # delivered once
    assert cmd["id"] == "late-1"
    code, body = await job
    assert (
        code == 504 and body["error"]["code"] == "hub_timeout" and body["error"]["id"] == "late-1"
    )
    await ws.send_json(
        {"t": "bt_result", "id": "late-1", "ok": False, "code": "bt_busy", "msg": "busy"}
    )
    await drain(ws)
    code, body = await post(
        c, "pair/stop", cmd_id="late-1"
    )  # the retry gets the answer, no second command
    assert code == 502 and body["error"]["hub_code"] == "bt_busy"
    assert await drain(ws) == []


async def test_in_flight_commands_are_bounded(c, ws):
    jobs = [asyncio.ensure_future(post(c, "pair/stop")) for _ in range(bt_routes.PENDING_MAX + 4)]
    async with asyncio.timeout(5):
        for _ in range(bt_routes.PENDING_MAX):
            await recv(ws, "bt_cmd")
        done, pending = await asyncio.wait(jobs, return_when=asyncio.FIRST_COMPLETED)
        while len(done) < 4:
            done, pending = await asyncio.wait(jobs, return_when=asyncio.FIRST_COMPLETED)
    assert sorted(j.result()[0] for j in done) == [429] * 4
    await ws.close()  # the channel closes: every command in flight fails hub_offline
    results = await asyncio.wait_for(asyncio.gather(*pending), 5)
    assert {code for code, _ in results} == {409} and {b["error"]["code"] for _, b in results} == {
        "hub_offline"
    }


async def test_a_command_that_never_got_an_answer_frees_its_slot(c, ws, clock):
    c.app[bt_routes.BT_KEY].result_wait = 5.0
    jobs = [
        asyncio.ensure_future(post(c, "pair/stop", cmd_id=f"x{i}"))
        for i in range(bt_routes.PENDING_MAX)
    ]
    for _ in jobs:
        await recv(ws, "bt_cmd")
    clock.t += bt_routes.PENDING_TTL_S + 1
    code, _, _ = await run(c, ws, "pair/stop")  # the slots of the silent ones are given back
    assert code == 200
    done = await asyncio.wait_for(asyncio.gather(*jobs), 5)
    assert {(code, b["error"]["hub_code"]) for code, b in done} == {(502, "expired")}


async def test_command_ids_make_a_repost_idempotent(c, ws):
    assert bt_routes.RESULT_WAIT_S >= 20
    code, body, _ = await run(c, ws, "pair/stop", cmd_id="my-1")
    assert code == 200 and body["id"] == "my-1"
    code, again = await post(
        c, "pair/stop", cmd_id="my-1"
    )  # the editor retried: the stored result, nothing sent
    assert code == 200 and again["id"] == "my-1"
    assert await drain(ws) == []
    assert (await post(c, "pair/stop", cmd_id="bad id!"))[0] == 400
    # two posts of one id while it is in flight: one command, both answered
    j1 = asyncio.ensure_future(post(c, "pair/stop", cmd_id="my-2"))
    cmd = await recv(ws, "bt_cmd")
    j2 = asyncio.ensure_future(post(c, "pair/stop", cmd_id="my-2"))
    await ws.send_json({"t": "bt_result", "id": cmd["id"], "ok": True})
    assert (await j1)[0] == 200 and (await j2)[0] == 200
    assert await drain(ws) == []  # still exactly one bt_cmd for my-2


async def test_a_second_channel_for_the_hub_fails_commands_in_flight(c, ws):
    job = asyncio.ensure_future(post(c, "pair/stop"))
    await recv(ws, "bt_cmd")
    other = await connect(c, HUB)
    await drain(other)
    code, body = await job
    assert code == 409 and body["error"]["code"] == "hub_offline"
    await other.close()


async def test_relay_state_of_a_renamed_hub_is_dropped(c, ws):
    from harmony_addon.app import LINKED_KEY

    assert HUB in c.app[bt_routes.BT_KEY].hubs
    c.app[LINKED_KEY].rename(HUB, "h2")
    other = await connect(c, "h2")
    await bt_online(other)
    await drain(other)
    assert list(c.app[bt_routes.BT_KEY].hubs) == ["h2"]
    await other.close()


async def test_editor_routes_need_the_csrf_header_and_the_poll_routes_are_gone(c):
    r = await c.post("/api/v1/bt/pair", json={"hub": HUB})
    assert r.status == 403
    for method, path in (
        ("GET", "/api/v1/bt/status"),
        ("GET", "/api/v1/bt/events"),
        ("POST", "/api/v1/bt/listen"),
    ):
        r = await c.request(method, path, json={"hub": HUB, "on": True}, headers=H)
        assert r.status in (404, 405), path


async def test_codes_tables_match_the_core_and_need_no_hub(c):
    from harmony_core.bt_codes import BUILTINS, DEFAULT_COMMANDS, DEFAULT_REMOTE_KEYS

    r = await c.get("/api/v1/bt/codes")
    assert r.status == 200
    got = await r.json()
    assert {b["name"] for b in got["builtins"]} == set(BUILTINS)
    assert {b["name"] for b in got["builtins"] if b["default"]} == set(DEFAULT_COMMANDS)
    assert [(k["name"], k["key"]) for k in got["remote_keys"]] == list(DEFAULT_REMOTE_KEYS)
    assert {"volume_up", "key_100", "key_109"} <= set(got["keys"])
