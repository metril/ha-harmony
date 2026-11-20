"""`GET /api/v1/live`: the editor's WebSocket (subscribe, snapshot, deltas, resync, listen lease) and TCP keepalive."""

import socket

import pytest
from addon_ws import TOKEN, bt_online, connect, drain, live, recv
from aiohttp import WSServerHandshakeError
from cfgapi import apply_draft, put_apply, put_draft
from test_addon import CONFIG, H

from harmony_addon import channel as channel_mod
from harmony_addon import live as live_mod
from harmony_addon.app import BUS_KEY, create_app
from harmony_addon.auth import IngressAuth, NoAuth


def make(tmp_path, **kw):
    return create_app(
        tmp_path, auth=NoAuth(), bundle_token=TOKEN, bundle_networks=None, hello_timeout=0.3, **kw
    )


@pytest.fixture
async def c(aiohttp_client, tmp_path):
    return await aiohttp_client(make(tmp_path))


async def apply(c, hub="h1"):
    assert (await put_apply(c, CONFIG, hub)).status == 200


async def test_subscribe_sends_a_snapshot_of_each_new_topic(c):
    await apply(c)
    lv = await live(c, "hubs", "hub:h1", "bt:h1", "keys:h1")
    snap = await recv(lv, "snapshot")
    assert set(snap["topics"]) == {"hubs", "hub:h1", "bt:h1"}  # keys has none
    assert [h["id"] for h in snap["topics"]["hubs"]["hubs"]] == ["h1"]
    assert set(snap["topics"]["hubs"]) >= {"hubs", "linked", "now", "adopt"}
    st = snap["topics"]["hub:h1"]
    assert (
        st["hub"] == "h1"
        and st["applied_rev"]
        and st["connected"] is False
        and st["connected_at"] is None
    )
    assert snap["topics"]["bt:h1"] == {
        "hub": "h1",
        "status": "unknown",
        "state": None,
        "limits": None,
        "listening": True,  # this very socket holds the lease
    }
    await lv.send_json(
        {"sub": ["hubs", "hub:h2"]}
    )  # replaces the set: only the new topic is snapshotted
    snap = await recv(lv, "snapshot")
    assert set(snap["topics"]) == {"hub:h2"}
    assert snap["topics"]["hub:h2"]["applied_rev"] is None
    await lv.close()


async def test_bad_subscriptions_are_answered_and_unknown_topics_ignored(c):
    lv = await live(c)
    for bad in (
        '{"sub": "hubs"}',
        '{"sub": ' + str(["hubs"] * 17).replace("'", '"') + "}",
        "nope",
        '{"x": 1}',
        "[]",
    ):
        await lv.send_str(bad)
        assert await recv(lv) == {"t": "error", "code": "bad_sub"}
    await lv.send_json({"sub": ["nonsense", "hub:", "hub:../x", "bt:a b", 7, "hubs"]})
    snap = await recv(lv, "snapshot")
    assert set(snap["topics"]) == {"hubs"}
    await lv.close()


async def test_changes_are_pushed_as_full_payload_deltas(c):
    lv = await live(c, "hubs", "hub:h1")
    await recv(lv, "snapshot")
    assert (await put_draft(c, CONFIG)).status == 200
    assert (await apply_draft(c, "h1")).status == 200
    d = await recv(lv, "hubs")
    assert d["hubs"][0]["has_applied"] is True
    st = await recv(lv, "hub")
    assert st["applied_rev"] == d["hubs"][0]["applied_rev"] and st["result"] is None
    await lv.close()


async def test_integration_connect_ack_and_disconnect_show_up(c):
    await apply(c)
    lv = await live(c, "hubs", "hub:h1")
    await recv(lv, "snapshot")
    ws = await connect(c, "h1")
    assert [e["id"] for e in (await recv(lv, "hubs"))["linked"]] == ["h1"]
    st = await recv(lv, "hub")
    assert st["connected"] is True and st["connected_at"] is not None
    rev = (await recv(ws, "bundle"))["rev"]
    await ws.send_json({"t": "ack", "rev": rev, "result": "ok"})
    assert (await recv(lv, "hub"))["acked_rev"] == rev
    await ws.close()
    assert (await recv(lv, "hub"))["connected"] is False
    await lv.close()


async def test_other_hubs_topics_are_not_delivered(c):
    lv = await live(c, "hub:h1")
    await recv(lv, "snapshot")
    await apply(c, "h2")
    await apply(c, "h1")
    assert (await recv(lv))["t"] == "hub"  # the first frame is h1's, none for h2 came before it
    await lv.close()


async def test_queue_overflow_resyncs_with_a_fresh_snapshot(aiohttp_client, tmp_path):
    app = make(tmp_path, live_queue=2)
    c = await aiohttp_client(app)
    lv = await live(c, "hub:h1")
    await recv(lv, "snapshot")
    for i in range(5):  # synchronously, so the socket's queue (2) overflows before it can drain
        app[BUS_KEY].publish("live", "hub:h1", {"t": "hub", "hub": "h1", "i": i})
    assert await recv(lv) == {"t": "resync", "topic": "hub:h1"}
    snap = await recv(lv, "snapshot")
    assert snap["topics"]["hub:h1"]["hub"] == "h1" and "i" not in snap["topics"]["hub:h1"]
    app[BUS_KEY].publish(
        "live", "hub:h1", {"t": "hub", "hub": "h1", "i": 99}
    )  # and deltas flow again
    assert (await recv(lv))["i"] == 99
    await lv.close()


# ------------------------------------------------------------------ the listen lease
async def test_keys_subscription_is_the_listen_lease(c):
    ws = await connect(c, "h1")
    await drain(ws)
    a = await live(c, "keys:h1")
    b = await live(c, "keys:h1", "hubs")
    await recv(b, "snapshot")
    on = await recv(ws, "listen")
    assert on == {"t": "listen", "on": True}
    assert await drain(ws) == []  # the second subscriber adds nothing
    await a.send_json({"sub": []})  # one left: still listening
    assert await drain(ws) == []
    await b.send_json({"sub": ["hubs"]})  # the last unsubscribe
    assert await recv(ws, "listen") == {"t": "listen", "on": False}
    c1 = await live(c, "keys:h1")
    assert await recv(ws, "listen") == {"t": "listen", "on": True}
    await c1.close()  # a closed socket releases its lease
    assert await recv(ws, "listen") == {"t": "listen", "on": False}
    await a.close()
    await b.close()
    await ws.close()


async def test_a_reconnecting_channel_is_told_to_listen_again(c):
    ws = await connect(c, "h1")
    await drain(ws)
    lv = await live(c, "keys:h1")
    await recv(ws, "listen")
    await ws.close()
    ws2 = await connect(c, "h1")
    frames = await drain(ws2)
    assert {"t": "listen", "on": True} in frames
    await lv.close()
    await recv(ws2, "listen")
    await ws2.close()
    ws3 = await connect(c, "h1")  # nobody listens: nothing to assert on hello
    assert {"t": "listen", "on": True} not in await drain(ws3)
    await ws3.close()


async def test_remote_keys_flow_from_the_channel_to_subscribers_only(c):
    ws = await connect(c, "h1")
    await drain(ws)
    lv = await live(c, "keys:h1")
    other = await live(c, "keys:h2")
    await recv(ws, "listen")
    await ws.send_json({"t": "bt_key", "slot": 1, "page": 12, "usage": 233, "down": True})
    await ws.send_json({"t": "bt_key", "slot": 1, "page": 12, "usage": 233, "down": False})
    k1, k2 = await recv(lv), await recv(lv)
    assert {k: k1[k] for k in ("t", "hub", "slot", "page", "usage", "down")} == {
        "t": "key",
        "hub": "h1",
        "slot": 1,
        "page": 12,
        "usage": 233,
        "down": True,
    }
    assert isinstance(k1["ts"], int) and k2["down"] is False and k2["seq"] == k1["seq"] + 1
    await other.send_json({"sub": ["hubs"]})
    assert (await recv(other))["t"] == "snapshot"  # nothing for keys:h2 arrived before it
    await lv.close()
    await other.close()
    await ws.close()


async def test_bt_snapshot_reports_listening(c):
    ws = await connect(c, "h1")
    await bt_online(ws)
    await drain(ws)
    lv = await live(c, "bt:h1")
    assert (await recv(lv, "snapshot"))["topics"]["bt:h1"]["listening"] is False
    k = await live(c, "keys:h1")
    assert (await recv(lv, "bt"))["listening"] is True
    await k.close()
    assert (await recv(lv, "bt"))["listening"] is False
    await lv.close()
    await ws.close()


# ------------------------------------------------------------------ admission and keepalive
async def test_live_needs_ingress_auth_and_a_same_origin_upgrade(aiohttp_client, tmp_path):
    c = await aiohttp_client(
        create_app(tmp_path, auth=IngressAuth(peers={"127.0.0.1"}), bundle_token=TOKEN)
    )
    for hdr, code in (({}, 401), ({"X-Remote-User-Id": "bad id!"}, 401)):
        with pytest.raises(WSServerHandshakeError) as e:
            await c.ws_connect("/api/v1/live", headers=hdr)
        assert e.value.status == code
    user = {"X-Remote-User-Id": "u1"}
    host = f"{c.server.host}:{c.server.port}"
    for extra in (
        {"Origin": "http://evil.example"},
        {"Sec-Fetch-Site": "cross-site"},
        {"Origin": "null"},
    ):
        with pytest.raises(WSServerHandshakeError) as e:
            await c.ws_connect("/api/v1/live", headers={**user, **extra})
        assert e.value.status == 403, extra
    ws = await c.ws_connect(
        "/api/v1/live",
        headers={**user, "Origin": f"http://{host}", "Sec-Fetch-Site": "same-origin"},
    )
    await ws.send_json({"sub": ["hubs"]})
    assert (await recv(ws))["t"] == "snapshot"
    await ws.close()
    with pytest.raises(
        WSServerHandshakeError
    ) as e:  # the bearer token is not an ingress credential
        await c.ws_connect("/api/v1/live", headers={"Authorization": f"Bearer {TOKEN}"})
    assert e.value.status == 401


async def test_both_sockets_get_tcp_keepalive_and_no_heartbeat(c, monkeypatch):
    seen = []

    def spy(sock):
        seen.append(sock)
        return True

    monkeypatch.setattr(channel_mod, "set_keepalive", spy)
    monkeypatch.setattr(live_mod, "set_keepalive", spy)
    ws = await connect(c, "h1")
    lv = await live(c, "hubs")
    await drain(ws)
    await recv(lv, "snapshot")
    assert len(seen) == 2 and all(s is not None and hasattr(s, "setsockopt") for s in seen)
    for conn in c.app[channel_mod.CHANNEL_KEY].conns.values():
        assert conn.ws._heartbeat is None  # liveness is the kernel's, not an application timer
    await ws.close()
    await lv.close()


def test_the_real_keepalive_options_are_set():
    a, b = socket.socketpair()
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        from harmony_addon.sockopts import set_keepalive

        assert set_keepalive(s) is True
        assert s.getsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE) == 1
        assert s.getsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPIDLE) == 60
        assert s.getsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPINTVL) == 10
        assert s.getsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPCNT) == 3
        s.close()
    finally:
        a.close()
        b.close()
