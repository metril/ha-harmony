"""Add-on -> integration: the bearer-only channel (hello, bundle push, ack, close codes), auth separation, discovery."""

import asyncio
import json
import logging
import stat

import pytest
from addon_ws import BEARER, TOKEN, closed, connect, drain, live, recv
from aiohttp import WSServerHandshakeError
from cfgapi import put_apply
from test_addon import CONFIG, H, clock, put_draft

import harmony_core
from harmony_addon.app import create_app
from harmony_addon.auth import BundleAuth, IngressAuth, NoAuth, load_or_create_token
from harmony_addon.options import Options
from harmony_addon.supervisor import Discovery, HttpSupervisor, default_supervisor


@pytest.fixture
async def client(aiohttp_client, tmp_path):
    app = create_app(
        tmp_path,
        options=Options("info"),
        clock=clock,
        auth=NoAuth(),
        bundle_token=TOKEN,
        bundle_networks=None,
        hello_timeout=0.3,
    )
    return await aiohttp_client(app)


async def apply(c, config=CONFIG, hub="default"):
    r = await put_apply(c, config, hub)
    assert r.status == 200, await r.text()


async def refused(c, path, headers):
    with pytest.raises(WSServerHandshakeError) as e:
        await c.ws_connect(path, headers=headers)
    return e.value.status


async def test_channel_without_or_with_wrong_token_is_401(client):
    await apply(client)
    for hdr in (
        {},
        {"Authorization": "Bearer nope"},
        {"Authorization": f"Basic {TOKEN}"},
        {"Authorization": TOKEN},
        {"Authorization": f"Bearer {TOKEN[:-1]}0"},
    ):
        assert await refused(client, "/api/v1/channel?hub=default", hdr) == 401
    assert await refused(client, "/api/v1/channel?hub=BAD!", BEARER) == 400


async def test_hello_gets_the_bundle(client):
    await apply(client)
    ws = await connect(client, "default", known_rev=None)
    b = await recv(ws, "bundle")
    body = b["body"]
    assert b["rev"] == body["rev"]
    assert set(body) == {"schema", "core_version", "hub_id", "applied_at", "rev", "config"}
    assert body["schema"] == harmony_core.BUNDLE_SCHEMA and body["hub_id"] == "default"
    assert body["core_version"] == harmony_core.__version__ and body["config"] == CONFIG
    await ws.close()


async def test_known_rev_skips_the_bundle_and_nothing_applied_is_said(client):
    await put_draft(client)  # a draft is not an applied config
    ws = await connect(client, "default")
    assert await recv(ws) == {"t": "nothing_applied"}
    await ws.close()
    await apply(client)
    ws = await connect(client, "default")
    rev = (await recv(ws, "bundle"))["rev"]
    await ws.close()
    ws = await connect(client, "default", known_rev=rev)
    assert await drain(ws) == []  # same rev: nothing to send
    await ws.close()
    ws = await connect(client, "default", known_rev="old")
    assert (await recv(ws))["t"] == "bundle"
    await ws.close()


async def test_apply_pushes_one_bundle_and_rev_changes_only_with_content(client):
    await apply(client)
    ws = await connect(client, "default")
    a = (await recv(ws, "bundle"))["rev"]
    await apply(client)  # same content again: pushed, same rev
    assert (await recv(ws, "bundle"))["rev"] == a
    await apply(client, {**CONFIG, "keys": {}})
    assert (await recv(ws, "bundle"))["rev"] != a
    assert await drain(ws) == []
    await ws.close()


async def test_first_apply_after_nothing_applied_is_pushed_and_hubs_are_independent(client):
    ws = await connect(client, "default")
    other = await connect(client, "other")
    assert await recv(ws) == {"t": "nothing_applied"} and await recv(other) == {
        "t": "nothing_applied"
    }
    await apply(client)
    assert (await recv(ws))["t"] == "bundle"
    assert await drain(other) == []
    await ws.close()
    await other.close()


async def test_revert_pushes_only_when_the_applied_config_changes(client):
    await apply(client)
    await apply(client, {**CONFIG, "keys": {}})
    ws = await connect(client, "default")
    await recv(ws, "bundle")
    await client.post("/api/v1/hubs/default/revert", json={}, headers=H)
    assert (await recv(ws, "bundle"))["body"]["config"] == CONFIG
    await ws.close()


async def test_ack_is_stored_and_a_stale_one_is_refused(client):
    await apply(client)
    ws = await connect(client, "default")
    rev = (await recv(ws, "bundle"))["rev"]
    await ws.send_json({"t": "ack", "rev": rev, "result": "ok"})
    assert await drain(ws) == []
    lv = await live(client, "hub:default")
    snap = (await recv(lv, "snapshot"))["topics"]["hub:default"]
    assert snap["acked_rev"] == rev and snap["result"] == "ok" and snap["connected"] is True
    await ws.send_json({"t": "ack", "rev": "0" * 16, "result": "ok"})
    assert (await recv(ws, "error"))["code"] == "stale_rev"
    await ws.send_json({"t": "ack", "rev": rev, "result": "error", "detail": "x" * 300})
    assert (await recv(ws, "error"))["code"] == "bad_frame"
    await ws.send_json({"t": "ack", "rev": rev, "result": "error", "detail": "hub said no"})
    d = await recv(lv, "hub")
    assert d["result"] == "error" and d["detail"] == "hub said no"
    await ws.close()
    await lv.close()


async def test_close_codes(aiohttp_client, tmp_path):
    c = await aiohttp_client(
        create_app(
            tmp_path, auth=NoAuth(), bundle_token=TOKEN, bundle_networks=None, hello_timeout=0.2
        )
    )
    ws = await connect(c, "default", hello=False)  # no hello in time
    assert await closed(ws) == 4000
    for bad in (
        {"t": "hello", "hub": "default"},
        {"t": "nope", "v": 1},
        {"t": "hello", "v": 1, "hub": "x y"},
    ):
        ws = await c.ws_connect("/api/v1/channel?hub=default", headers=BEARER)
        await ws.send_json(bad)
        assert (await recv(ws))["code"] == "bad_hello" and await closed(ws) == 4000
    ws = await c.ws_connect("/api/v1/channel?hub=default", headers=BEARER)
    await ws.send_str("not json")
    assert (await recv(ws))["code"] == "bad_hello" and await closed(ws) == 4000
    ws = await c.ws_connect("/api/v1/channel?hub=default", headers=BEARER)
    await ws.send_json({"t": "hello", "v": 1, "hub": "other"})
    assert (await recv(ws))["code"] == "hub_mismatch" and await closed(ws) == 4000
    ws = await c.ws_connect("/api/v1/channel?hub=default", headers=BEARER)
    await ws.send_json({"t": "hello", "v": 2, "hub": "default"})
    assert (await recv(ws))["code"] == "unsupported_version" and await closed(ws) == 4001
    first = await connect(c, "default")
    await recv(first)
    second = await connect(c, "default")
    await recv(second)
    assert await closed(first) == 4002  # one socket per hub: the old one is replaced
    await second.close()


async def test_unknown_frames_and_fields_are_ignored(client):
    await apply(client)
    ws = await connect(client, "default", future_field=[1, 2], hw=None)
    rev = (await recv(ws, "bundle"))["rev"]
    await ws.send_json({"t": "from_the_future", "x": 1})
    assert await drain(ws) == []
    await ws.send_json({"t": "ack", "rev": rev, "result": "ok", "extra": 1})
    assert await drain(ws) == []
    await ws.close()


# ---------------------------------------------------------------- auth separation
async def test_channel_is_bearer_only_and_ingress_routes_ingress_only(aiohttp_client, tmp_path):
    app = create_app(
        tmp_path, auth=IngressAuth(peers={"127.0.0.1"}), bundle_token=TOKEN, bundle_networks=None
    )
    c = await aiohttp_client(app)
    ing = {"X-Ingress-Path": "/api/hassio_ingress/t"}
    assert await refused(c, "/api/v1/channel", ing) == 401  # ingress without the token...
    assert (
        await refused(c, "/api/v1/channel", {**ing, **BEARER}) == 401
    )  # ...and the token through ingress
    strict = await aiohttp_client(
        create_app(tmp_path / "s", bundle_token=TOKEN, bundle_networks=None)
    )
    for path in ("/api/v1/health", "/api/v1/configs", "/", "/api/v1/live"):
        assert (await strict.get(path, headers={**BEARER, **ing})).status == 403
    ws = await strict.ws_connect(
        "/api/v1/channel", headers=BEARER
    )  # authed: the bearer needs no ingress peer
    await ws.close()


async def test_channel_peer_must_be_on_the_hassio_network(aiohttp_client, tmp_path):
    c = await aiohttp_client(
        create_app(tmp_path, auth=NoAuth(), bundle_token=TOKEN)
    )  # 172.30.32.0/23
    assert await refused(c, "/api/v1/channel", BEARER) == 401  # peer is 127.0.0.1


async def test_failed_attempts_are_rate_limited(aiohttp_client, tmp_path):
    t = [0.0]
    c = await aiohttp_client(
        create_app(tmp_path, auth=NoAuth(), bundle_auth=BundleAuth(TOKEN, None, clock=lambda: t[0]))
    )
    for _ in range(BundleAuth.MAX_FAILS):
        assert await refused(c, "/api/v1/channel", {}) == 401
    r = await c.get("/api/v1/channel")  # a wrong token is now throttled...
    assert r.status == 429 and r.headers.get("Retry-After") == "60"
    ws = await c.ws_connect("/api/v1/channel", headers=BEARER)  # ...but the right one never is
    await ws.close()
    t[0] += BundleAuth.WINDOW + 1
    assert await refused(c, "/api/v1/channel", {}) == 401


async def test_removed_poll_routes_are_gone(client):
    for method, path in [
        ("GET", "/api/v1/bundle"),
        ("GET", "/api/v1/bundle/wait"),
        ("POST", "/api/v1/bundle/ack"),
        ("POST", "/api/v1/bundle/bt"),
    ]:
        assert (
            await client.request(method, path, headers={**BEARER, **H}, json={})
        ).status == 404, path
    for method, path in [
        ("GET", "/api/v1/config/status"),
        ("GET", "/api/v1/bt/status"),
        ("GET", "/api/v1/bt/events"),
        ("POST", "/api/v1/bt/listen"),
    ]:
        r = await client.request(method, path, headers=H, json={})
        assert r.status in (404, 405), path


async def test_health_advertises_the_channel(client):
    assert (await (await client.get("/api/v1/health")).json())["capabilities"]["channel"] == 1


# ---------------------------------------------------------------- token handling
def test_token_file_is_0600_random_and_stable(tmp_path):
    a = load_or_create_token(tmp_path)
    assert len(a) == 64 and int(a, 16) >= 0
    assert stat.S_IMODE((tmp_path / "bundle_token").stat().st_mode) == 0o600
    assert load_or_create_token(tmp_path) == a
    (tmp_path / "bundle_token").write_text("junk")
    assert load_or_create_token(tmp_path) != a  # a damaged file is replaced, not trusted


async def test_token_never_in_logs_or_other_responses(aiohttp_client, tmp_path, caplog):
    caplog.set_level(logging.DEBUG)
    token = load_or_create_token(tmp_path)
    c = await aiohttp_client(
        create_app(tmp_path, auth=NoAuth(), bundle_token=token, bundle_networks=None)
    )
    await put_draft(c)
    seen = []
    for method, path in [
        ("GET", "/"),
        ("GET", "/api/v1/health"),
        ("GET", "/api/v1/configs/main"),
        ("GET", "/api/v1/configs/main/export_yaml"),
        ("POST", "/api/v1/hubs/default/apply"),
        ("GET", "/api/v1/hubs/default"),
        ("GET", "/api/v1/nope"),
    ]:
        r = await c.request(method, path, headers=H, json={} if method == "POST" else None)
        seen.append(str(r.headers) + await r.text())
    r = await c.get("/api/v1/channel", headers={"Authorization": f"Bearer {token}x"})
    seen.append(str(r.headers) + await r.text())
    ws = await c.ws_connect("/api/v1/channel", headers={"Authorization": f"Bearer {token}"})
    await ws.send_json({"t": "hello", "v": 1, "hub": "default"})
    seen.append(json.dumps(await recv(ws)))
    await ws.close()
    assert all(token not in x for x in seen)
    assert token not in caplog.text


# ---------------------------------------------------------------- discovery
class FakeSupervisor:
    def __init__(self, fail=False):
        self.calls, self.fail = [], fail

    async def self_hostname(self):
        self.calls.append("info")
        return "abc-ha-harmony-editor"

    async def announce(self, config):
        self.calls.append(("announce", config))
        if self.fail:
            raise OSError("down")
        return "uuid-1"

    async def withdraw(self, uuid):
        self.calls.append(("withdraw", uuid))


async def test_discovery_announces_at_start_and_withdraws_at_stop(aiohttp_client, tmp_path):
    sup = FakeSupervisor()
    app = create_app(
        tmp_path, auth=NoAuth(), bundle_token=TOKEN, discovery=Discovery(sup, 8099, TOKEN)
    )
    c = await aiohttp_client(app)
    assert sup.calls == [
        "info",
        ("announce", {"host": "abc-ha-harmony-editor", "port": 8099, "token": TOKEN}),
    ]
    await c.close()
    assert sup.calls[-1] == ("withdraw", "uuid-1")


async def test_discovery_failure_is_logged_not_fatal(aiohttp_client, tmp_path, caplog):
    sup = FakeSupervisor(fail=True)
    app = create_app(
        tmp_path, auth=NoAuth(), bundle_token=TOKEN, discovery=Discovery(sup, 8099, TOKEN)
    )
    c = await aiohttp_client(app)
    assert (await c.get("/api/v1/health")).status == 200
    assert "could not announce" in caplog.text and TOKEN not in caplog.text
    await c.close()
    assert not any(isinstance(x, tuple) and x[0] == "withdraw" for x in sup.calls)


def test_no_supervisor_token_logs_once_and_continues(monkeypatch, caplog):
    caplog.set_level(logging.INFO)
    monkeypatch.delenv("SUPERVISOR_TOKEN", raising=False)
    assert default_supervisor() is None
    monkeypatch.setenv("SUPERVISOR_TOKEN", "t")
    assert isinstance(default_supervisor(), HttpSupervisor)
    assert caplog.text.count("SUPERVISOR_TOKEN is not set") == 1


async def test_discovery_without_supervisor_is_a_noop():
    d = Discovery(None, 8099, TOKEN)
    await d.announce()
    await d.withdraw()


async def test_shutdown_closes_every_socket(aiohttp_client, tmp_path):
    app = create_app(tmp_path, auth=NoAuth(), bundle_token=TOKEN, bundle_networks=None)
    c = await aiohttp_client(app)
    chans = [await connect(c, f"h{i}") for i in range(3)]
    lv = await live(c, "hubs")
    for ws in chans:
        await recv(ws)
    await recv(lv, "snapshot")
    waits = [asyncio.ensure_future(closed(w)) for w in (*chans, lv)]
    await app.shutdown()
    codes = await asyncio.wait_for(asyncio.gather(*waits), 5)
    assert [int(x) for x in codes] == [1001] * 4


def test_main_passes_a_short_shutdown_timeout(monkeypatch, tmp_path):
    from aiohttp import web

    from harmony_addon import __main__ as m

    seen = {}
    monkeypatch.setattr(web, "run_app", lambda app, **kw: seen.update(kw))
    monkeypatch.setattr("sys.argv", ["x", "--data-dir", str(tmp_path)])
    monkeypatch.delenv("SUPERVISOR_TOKEN", raising=False)
    m.main()
    assert seen["shutdown_timeout"] <= 5


async def test_every_channel_route_needs_the_token_and_never_ingress(aiohttp_client, tmp_path):
    from test_addon import CHANNEL_ROUTES

    c = await aiohttp_client(
        create_app(tmp_path, auth=NoAuth(), bundle_token=TOKEN, bundle_networks=None)
    )
    ing = {"X-Ingress-Path": "/api/hassio_ingress/t"}
    assert CHANNEL_ROUTES
    for method, path in CHANNEL_ROUTES:
        h = {"Content-Type": "application/json"}
        assert (await c.request(method, path, json={}, headers=h)).status == 401, path
        r = await c.request(method, path, json={}, headers={**h, **BEARER, **ing})
        assert r.status == 401, path


async def test_a_bundle_applied_while_the_hello_is_answered_is_not_lost(client, monkeypatch):
    from harmony_addon import channel

    await apply(client)
    real, calls = channel._bundle_frame, []

    def building(app, hub):
        frame = real(app, hub)
        if not calls:  # an apply lands while the first frame is being built
            channel.on_apply(app, hub)
        calls.append(1)
        return frame

    monkeypatch.setattr(channel, "_bundle_frame", building)
    ws = await connect(client, "default", known_rev=None)
    first = await recv(ws, "bundle")
    second = await recv(ws, "bundle")  # the push that raced the hello is delivered, not dropped
    assert first["rev"] == second["rev"]
    await ws.close()


async def test_shutdown_closes_all_hub_sockets_together(client):
    a, b = await connect(client, "default"), await connect(client, "default2")
    await recv(a)
    await recv(b)
    await client.app.shutdown()
    assert await closed(a) == 1001 and await closed(b) == 1001
