"""The add-on's Bluetooth relay: editor routes (ingress) <-> the integration's exchange (bearer). No hub, no Home Assistant:
a test plays the integration with raw requests."""

import asyncio

import pytest
from test_addon import H

from harmony_addon import bt_routes
from harmony_addon.app import create_app
from harmony_addon.auth import NoAuth

TOKEN = "ab" * 32
BEARER = {"Authorization": f"Bearer {TOKEN}"}
HUB = "h1"
LIMITS = {"hosts_max": 4, "conn_max": 2, "remotes_max": 2, "le_bonds_max": 8, "pages": [7, 12], "le": True}
STATE = {
    "state": "ready",
    "discoverable": False,
    "gen": 3,
    "hosts": [{"slot": 0, "addr": "aa:bb:cc:dd:ee:01", "name": "TV", "transport": "le", "connected": True}],
    "remotes": [{"slot": 0, "addr": "11:22:33:44:55:01", "connected": False, "battery": 80}],
}


class Clock:
    t = 1000.0

    def __call__(self):
        return self.t


@pytest.fixture
def clock():
    return Clock()


@pytest.fixture
async def c(aiohttp_client, tmp_path, clock):
    app = create_app(tmp_path, auth=NoAuth(), bundle_token=TOKEN, bundle_networks=None, clock=clock)
    cl = await aiohttp_client(app)
    await cl.get(f"/api/v1/bundle?hub={HUB}", headers=BEARER)  # a poll: the hub id is now linked
    return cl


async def xchg(c, **over):
    body = {"hub": HUB, "supported": True, "online": True, "state": STATE, "limits": LIMITS, **over}
    r = await c.post("/api/v1/bundle/bt", json=body, headers=BEARER)
    return r.status, await r.json()


async def status(c, hub=HUB):
    r = await c.get(f"/api/v1/bt/status?hub={hub}")
    return await r.json()


async def post(c, path, **body):
    r = await c.post(f"/api/v1/bt/{path}", json={"hub": HUB, **body}, headers=H)
    return r.status, await r.json()


def serve(c, results=None):
    """The integration: collect what is queued, answer every command ok (or with `results[op]`)."""

    async def run():
        for _ in range(100):
            _st, data = await xchg(c, results=[])
            if data["commands"]:
                out = []
                for cmd in data["commands"]:
                    r = (results or {}).get(cmd["op"], {"ok": True})
                    out.append({"id": cmd["id"], **r})
                await xchg(c, results=out)
                return data["commands"]
            await asyncio.sleep(0.02)
        raise AssertionError("no command arrived")

    return asyncio.ensure_future(run())


# ------------------------------------------------------------------ status
async def test_status_unknown_until_the_integration_reports(c):
    s = await status(c)
    assert s["status"] == "unknown" and s["state"] is None and s["limits"] is None
    code, body = await post(c, "pair", mode="host")
    assert code == 409 and body["error"]["code"] == "bt_unknown"


async def test_status_ok_offline_unsupported_and_stale(c, clock):
    assert (await xchg(c))[0] == 200
    s = await status(c)
    assert s["status"] == "ok" and s["limits"]["hosts_max"] == 4 and s["age_s"] == 0
    assert s["state"]["hosts"][0]["addr"] == "AA:BB:CC:DD:EE:01"  # normalised
    await xchg(c, online=False)
    assert (await status(c))["status"] == "offline"
    await xchg(c)
    clock.t += bt_routes.STALE_S + 1
    assert (await status(c))["status"] == "offline"
    await xchg(c, supported=False, state=None, limits=None)
    s = await status(c)
    assert s["status"] == "unsupported" and s["state"] is None
    code, body = await post(c, "pair")
    assert code == 409 and body["error"]["code"] == "bt_unsupported"
    await xchg(c, online=False)
    assert (await post(c, "key", slot=0, page=7, usage=4, action="press"))[1]["error"]["code"] == "hub_offline"


async def test_reported_state_is_cleaned(c):
    junk = {
        "state": "ready",
        "evil": "x",
        "hosts": [{"slot": 999, "addr": "aa:bb:cc:dd:ee:01"}, {"slot": 1, "addr": "nope"}, "x"]
        + [{"slot": 2, "addr": "AA:BB:CC:DD:EE:02", "name": "n\x00l"}],
        "candidates": [{"addr": "AA:BB:CC:DD:EE:03", "rssi": 500}],
    }
    await xchg(c, state=junk)
    st = (await status(c))["state"]
    assert "evil" not in st
    assert [h["slot"] for h in st["hosts"]] == [2] and "name" not in st["hosts"][0]
    assert st["candidates"][0]["rssi"] == -127
    assert (await xchg(c, state="garbage"))[0] == 400  # not an object: refused, the last good state stays
    await xchg(c, state={"state": "weird"})
    assert (await status(c))["state"] is None
    code, _ = await xchg(c, hub="not a hub!")
    assert code == 400


async def test_exchange_refuses_unlinked_hubs_and_bad_bodies(c):
    assert (await xchg(c, hub="other"))[0] == 404
    for bad in (
        {"supported": "yes"},
        {"results": [{"id": "../x", "ok": True}]},
        {"results": [{"id": "a", "ok": True}] * 33},
        {"events": [{"slot": 1, "page": 7, "usage": 70000, "down": True}]},
        {"extra": 1},
    ):
        assert (await xchg(c, **bad))[0] == 400, bad
    r = await c.post("/api/v1/bundle/bt", json={}, headers={})
    assert r.status == 401
    r = await c.post("/api/v1/bundle/bt", data="{}", headers={**BEARER, "Content-Type": "text/plain"})
    assert r.status == 403


# ------------------------------------------------------------------ commands
async def test_pair_round_trip_and_defaults(c):
    await xchg(c)
    t = serve(c)
    code, body = await post(c, "pair", mode="remote", transport="le", secs=60)
    cmds = await t
    assert code == 200 and body["ok"] is True and body["state"]["state"] == "ready"
    assert cmds[0]["op"] == "pair" and cmds[0]["mode"] == "remote" and cmds[0]["secs"] == 60
    t = serve(c)
    assert (await post(c, "pair"))[0] == 200
    assert (await t)[0]["secs"] == 120  # defaults filled in


async def test_every_command_reaches_the_integration_normalised(c):
    await xchg(c)
    sent = {}
    for path, body, op in (
        ("pair/stop", {}, "pair_stop"),
        ("pick", {"addr": "aa:bb:cc:dd:ee:09"}, "pick"),
        ("forget", {"slot": 1, "kind": "remote"}, "forget"),
        ("forget", {"slot": 2}, "forget"),
        ("key", {"slot": 3, "page": 12, "usage": 233, "action": "down"}, "key"),
    ):
        t = serve(c)
        code, _ = await post(c, path, **body)
        assert code == 200
        cmd = (await t)[0]
        assert cmd["op"] == op
        sent.setdefault(op, []).append(cmd)
    assert sent["pick"][0]["addr"] == "AA:BB:CC:DD:EE:09"
    assert sent["forget"][1]["kind"] == "host" and sent["forget"][0]["kind"] == "remote"
    assert {k: sent["key"][0][k] for k in ("slot", "page", "usage", "action")} == {
        "slot": 3, "page": 12, "usage": 233, "action": "down"
    }


async def test_hub_refusal_is_reported_with_its_code(c):
    await xchg(c)
    t = serve(c, {"pick": {"ok": False, "code": "no_candidate", "msg": "gone"}})
    code, body = await post(c, "pick", addr="AA:BB:CC:DD:EE:09")
    await t
    assert code == 502 and body["error"]["code"] == "hub_refused" and body["error"]["hub_code"] == "no_candidate"


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
async def test_invalid_requests_are_refused_before_queueing(c, path, body):
    await xchg(c)
    code, _ = await post(c, path, **body)
    assert code == 400
    assert (await xchg(c))[1]["commands"] == []


async def test_hub_limits_tighten_the_checks(c):
    await xchg(c)
    code, body = await post(c, "forget", slot=4)  # hosts_max is 4
    assert code == 422 and body["error"]["bt_code"] == "bad_slot"
    assert (await post(c, "forget", slot=2, kind="remote"))[0] == 422  # remotes_max is 2
    await xchg(c, limits={**LIMITS, "le": False})
    assert (await post(c, "pair", transport="le"))[1]["error"]["bt_code"] == "no_le"
    await xchg(c, limits={**LIMITS, "remotes_max": 0})
    assert (await post(c, "pair", mode="remote"))[1]["error"]["bt_code"] == "unsupported"
    assert (await xchg(c))[1]["commands"] == []


async def test_a_command_nobody_runs_times_out_and_leaves_no_trace(c, monkeypatch):
    monkeypatch.setattr(bt_routes, "RESULT_WAIT_S", 0.1)
    await xchg(c)
    code, body = await post(c, "pair/stop")
    assert code == 504 and body["error"]["code"] == "hub_timeout"
    assert (await xchg(c))[1]["commands"] == []  # not delivered late
    assert not c.app[bt_routes.BT_KEY].hubs[HUB].waiting


async def test_queue_is_bounded(c, monkeypatch):
    monkeypatch.setattr(bt_routes, "RESULT_WAIT_S", 1.0)
    await xchg(c)
    jobs = [asyncio.ensure_future(post(c, "pair/stop")) for _ in range(bt_routes.PENDING_MAX + 4)]
    async with asyncio.timeout(5):
        while sum(j.done() for j in jobs) < 4:
            await asyncio.sleep(0.02)
    codes = [j.result()[0] for j in jobs if j.done()]
    assert codes.count(429) == 4
    h = c.app[bt_routes.BT_KEY].hubs[HUB]
    assert len(h.queue) <= bt_routes.PENDING_MAX
    _st, data = await xchg(c)
    assert len(data["commands"]) <= 8  # at most MAX_COMMANDS per exchange
    for j in jobs:
        j.cancel()


async def test_fast_polling_only_for_a_reason(c, clock):
    assert (await xchg(c))[1]["fast"] is False
    for _ in range(3):
        await status(c)  # the editor looking does not speed the integration up
    assert (await xchg(c))[1]["fast"] is False
    t = serve(c)
    await post(c, "pair/stop")  # a command does, for a while
    await t
    assert (await xchg(c))[1]["fast"] is True
    clock.t += bt_routes.ACTIVE_S + 1
    assert (await xchg(c))[1]["fast"] is False
    assert (await xchg(c, state={**STATE, "window": "host"}))[1]["fast"] is True  # a pairing window is open


async def test_command_ids_make_a_repost_idempotent(c, monkeypatch):
    assert bt_routes.RESULT_WAIT_S >= 20
    await xchg(c)
    t = serve(c)
    code, body = await post(c, "pair/stop", cmd_id="my-1")
    await t
    assert code == 200 and body["id"] == "my-1"
    code, again = await post(c, "pair/stop", cmd_id="my-1")  # the editor retried: the stored result, nothing queued
    assert code == 200 and again["id"] == "my-1"
    assert (await xchg(c))[1]["commands"] == []
    assert (await post(c, "pair/stop", cmd_id="bad id!"))[0] == 400
    # two posts of one id while it is in flight: one command, both answered
    j1 = asyncio.ensure_future(post(c, "pair/stop", cmd_id="my-2"))
    await asyncio.sleep(0.05)
    j2 = asyncio.ensure_future(post(c, "pair/stop", cmd_id="my-2"))
    await asyncio.sleep(0.05)
    _st, data = await xchg(c)
    assert [x["id"] for x in data["commands"]] == ["my-2"]
    await xchg(c, results=[{"id": "my-2", "ok": True}])
    assert (await j1)[0] == 200 and (await j2)[0] == 200


async def test_a_result_after_the_editor_gave_up_is_kept_for_the_retry(c, monkeypatch):
    monkeypatch.setattr(bt_routes, "RESULT_WAIT_S", 0.1)
    await xchg(c)
    j = asyncio.ensure_future(post(c, "pair/stop", cmd_id="late-1"))
    await asyncio.sleep(0.02)
    assert [x["id"] for x in (await xchg(c))[1]["commands"]] == ["late-1"]  # delivered
    code, body = await j
    assert code == 504 and body["error"]["id"] == "late-1"
    await xchg(c, results=[{"id": "late-1", "ok": False, "code": "bt_busy", "msg": "busy"}])
    code, body = await post(c, "pair/stop", cmd_id="late-1")  # the retry gets the answer, no second command
    assert code == 502 and body["error"]["hub_code"] == "bt_busy"
    assert (await xchg(c))[1]["commands"] == []


async def test_relay_state_of_a_renamed_hub_is_dropped(c):
    from harmony_addon.app import LINKED_KEY

    await xchg(c)
    assert HUB in c.app[bt_routes.BT_KEY].hubs
    linked = c.app[LINKED_KEY]
    linked.rename(HUB, "h2")
    assert (await xchg(c, hub="h2"))[0] == 200
    assert list(c.app[bt_routes.BT_KEY].hubs) == ["h2"]
    assert (await status(c))["status"] == "unknown"


# ------------------------------------------------------------------ listen and events
EV = {"slot": 1, "page": 12, "usage": 233, "down": True}


async def test_listen_lease_and_event_ring(c, clock):
    await xchg(c)
    assert (await xchg(c, events=[EV]))[1]["listen"] is False  # nobody listens: keys are not kept
    assert (await (await c.get(f"/api/v1/bt/events?hub={HUB}")).json())["events"] == []
    code, body = await post(c, "listen", on=True)
    assert code == 200 and body["listening"] is True
    assert (await xchg(c))[1]["listen"] is True
    await xchg(c, events=[EV, {**EV, "down": False}])
    out = await (await c.get(f"/api/v1/bt/events?hub={HUB}")).json()
    assert [e["seq"] for e in out["events"]] == [1, 2] and out["last"] == 2 and out["listening"] is True
    again = await (await c.get(f"/api/v1/bt/events?hub={HUB}&after=2")).json()
    assert again["events"] == [] and again["dropped"] is False
    # polling keeps the lease alive; silence lets it lapse
    clock.t += bt_routes.LISTEN_S - 1
    await c.get(f"/api/v1/bt/events?hub={HUB}")
    clock.t += bt_routes.LISTEN_S - 1
    assert (await xchg(c))[1]["listen"] is True
    clock.t += bt_routes.LISTEN_S + 1
    assert (await xchg(c))[1]["listen"] is False
    await post(c, "listen", on=True)
    await post(c, "listen", on=False)
    assert (await xchg(c))[1]["listen"] is False


async def test_event_ring_is_bounded_and_reports_a_gap(c):
    await xchg(c)
    await post(c, "listen", on=True)
    for _ in range(3):
        await xchg(c, events=[EV] * 64)
    out = await (await c.get(f"/api/v1/bt/events?hub={HUB}")).json()
    assert len(out["events"]) == bt_routes.EVENTS_MAX and out["last"] == 192 and out["dropped"] is True
    assert out["events"][0]["seq"] == 129
    assert (await (await c.get(f"/api/v1/bt/events?hub={HUB}&after=191")).json())["dropped"] is False
    assert (await c.get(f"/api/v1/bt/events?hub={HUB}&after=x")).status == 400
    assert (await c.get(f"/api/v1/bt/events?hub={HUB}&after=1&after=2")).status == 400


async def test_editor_routes_need_the_csrf_header_and_exchange_needs_the_token(c):
    await xchg(c)
    r = await c.post("/api/v1/bt/pair", json={"hub": HUB})
    assert r.status == 403
    r = await c.post("/api/v1/bundle/bt", json={"hub": HUB, "supported": True, "online": True}, headers=H)
    assert r.status == 401  # the editor's header is not the integration's token
