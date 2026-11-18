"""Add-on -> integration feed: bearer-only bundle routes, long-poll, discovery."""

import asyncio
import json
import logging
import stat
import time

import pytest
from test_addon import CONFIG, H, clock, put_draft

import harmony_core
from harmony_addon.app import create_app
from harmony_addon.auth import BundleAuth, IngressAuth, NoAuth, load_or_create_token
from harmony_addon.options import Options
from harmony_addon.supervisor import Discovery, HttpSupervisor, default_supervisor

TOKEN = "ab" * 32
BEARER = {"Authorization": f"Bearer {TOKEN}"}


@pytest.fixture
async def client(aiohttp_client, tmp_path):
    app = create_app(
        tmp_path,
        options=Options("info"),
        clock=clock,
        auth=NoAuth(),
        bundle_token=TOKEN,
        bundle_networks=None,
        wait_seconds=5.0,
    )
    return await aiohttp_client(app)


async def apply(c, config=CONFIG):
    await put_draft(c, config)
    r = await c.post("/api/v1/config/apply", json={}, headers=H)
    assert r.status == 200, await r.text()


async def test_bundle_without_or_with_wrong_token_is_401_and_opaque(client):
    await apply(client)
    bodies = set()
    for hdr in (
        {},
        {"Authorization": "Bearer nope"},
        {"Authorization": f"Basic {TOKEN}"},
        {"Authorization": TOKEN},
        {"Authorization": f"Bearer {TOKEN[:-1]}0"},
    ):
        r = await client.get("/api/v1/bundle", headers=hdr)
        assert r.status == 401
        bodies.add(await r.text())
        assert (await client.get("/api/v1/bundle/wait", headers=hdr)).status == 401
    assert len(bodies) == 1  # nothing says which part failed


async def test_bundle_contents_and_etag(client):
    await apply(client)
    r = await client.get("/api/v1/bundle?hub=default", headers=BEARER)
    b = await r.json()
    assert r.status == 200 and r.headers["ETag"] == f'"{b["rev"]}"'
    assert set(b) == {"schema", "core_version", "hub_id", "applied_at", "rev", "config"}
    assert b["schema"] == harmony_core.BUNDLE_SCHEMA and b["hub_id"] == "default"
    assert b["core_version"] == harmony_core.__version__ and b["config"] == CONFIG
    r2 = await client.get("/api/v1/bundle", headers={**BEARER, "If-None-Match": r.headers["ETag"]})
    assert r2.status == 304


async def test_bundle_nothing_applied_is_404(client):
    await put_draft(client)  # a draft is not an applied config
    assert (await client.get("/api/v1/bundle", headers=BEARER)).status == 404
    assert (await client.get("/api/v1/bundle?hub=other", headers=BEARER)).status == 404
    assert (await client.get("/api/v1/bundle?hub=BAD!", headers=BEARER)).status == 400


async def test_bundle_rev_changes_only_with_content(client):
    await apply(client)
    a = (await (await client.get("/api/v1/bundle", headers=BEARER)).json())["rev"]
    await apply(client)  # same content again
    assert (await (await client.get("/api/v1/bundle", headers=BEARER)).json())["rev"] == a
    await apply(client, {**CONFIG, "keys": {}})
    assert (await (await client.get("/api/v1/bundle", headers=BEARER)).json())["rev"] != a


async def test_wait_times_out_cleanly_with_204(aiohttp_client, tmp_path):
    app = create_app(
        tmp_path, auth=NoAuth(), bundle_token=TOKEN, bundle_networks=None, wait_seconds=0.2
    )
    c = await aiohttp_client(app)
    await apply(c)
    rev = (await (await c.get("/api/v1/bundle", headers=BEARER)).json())["rev"]
    r = await c.get(f"/api/v1/bundle/wait?rev={rev}", headers=BEARER)
    assert r.status == 204 and await r.read() == b""
    assert (await c.get("/api/v1/bundle/wait?rev=old", headers=BEARER)).status == 200  # at once


async def test_wait_wakes_on_apply(client):
    await apply(client)
    rev = (await (await client.get("/api/v1/bundle", headers=BEARER)).json())["rev"]

    waiter = asyncio.create_task(client.get(f"/api/v1/bundle/wait?rev={rev}", headers=BEARER))
    await asyncio.sleep(0.05)
    assert not waiter.done()
    await apply(client, {**CONFIG, "keys": {}})
    r = await asyncio.wait_for(waiter, 2)
    assert r.status == 200 and (await r.json())["rev"] != rev


async def test_wait_on_hub_with_nothing_applied_wakes_on_first_apply(client):
    waiter = asyncio.create_task(client.get("/api/v1/bundle/wait", headers=BEARER))
    await asyncio.sleep(0.05)
    assert not waiter.done()
    await apply(client)
    assert (await asyncio.wait_for(waiter, 2)).status == 200


async def test_concurrent_waiters_all_wake_and_hubs_are_independent(client):
    await apply(client)
    rev = (await (await client.get("/api/v1/bundle", headers=BEARER)).json())["rev"]
    ws = [
        asyncio.create_task(client.get(f"/api/v1/bundle/wait?rev={rev}", headers=BEARER))
        for _ in range(5)
    ]
    other = asyncio.create_task(client.get("/api/v1/bundle/wait?hub=other", headers=BEARER))
    await asyncio.sleep(0.05)
    r = await client.post("/api/v1/config/revert", json={"target": "applied"}, headers=H)
    assert r.status == 200 and not any(w.done() for w in ws)  # discarding a draft is no apply
    await apply(client, {**CONFIG, "keys": {}})
    assert [(await asyncio.wait_for(w, 2)).status for w in ws] == [200] * 5
    assert not other.done()
    other.cancel()


async def test_revert_to_previous_wakes_waiters(client):
    await apply(client)
    await apply(client, {**CONFIG, "keys": {}})
    rev = (await (await client.get("/api/v1/bundle", headers=BEARER)).json())["rev"]
    w = asyncio.create_task(client.get(f"/api/v1/bundle/wait?rev={rev}", headers=BEARER))
    await asyncio.sleep(0.05)
    await client.post("/api/v1/config/revert", json={"target": "previous"}, headers=H)
    r = await asyncio.wait_for(w, 2)
    assert r.status == 200 and (await r.json())["config"] == CONFIG


# ---------------------------------------------------------------- auth separation
async def test_bundle_routes_are_bearer_only_and_ingress_routes_ingress_only(
    aiohttp_client, tmp_path
):
    app = create_app(
        tmp_path, auth=IngressAuth(peers={"127.0.0.1"}), bundle_token=TOKEN, bundle_networks=None
    )
    c = await aiohttp_client(app)
    ing = {"X-Ingress-Path": "/api/hassio_ingress/t"}
    # an ingress-looking request without the token cannot read the bundle...
    assert (await c.get("/api/v1/bundle", headers=ing)).status == 401
    # ...and even the right token is refused when it arrives through ingress
    assert (await c.get("/api/v1/bundle", headers={**ing, **BEARER})).status == 401
    # the token does not open any ingress-only route for a non-ingress peer
    strict = await aiohttp_client(
        create_app(tmp_path / "s", bundle_token=TOKEN, bundle_networks=None)
    )
    for path in ("/api/v1/health", "/api/v1/config", "/"):
        assert (await strict.get(path, headers={**BEARER, **ing})).status == 403
    assert (await strict.get("/api/v1/bundle", headers=BEARER)).status == 404  # authed, no data


async def test_bundle_peer_must_be_on_the_hassio_network(aiohttp_client, tmp_path):
    c = await aiohttp_client(
        create_app(tmp_path, auth=NoAuth(), bundle_token=TOKEN)  # default: 172.30.32.0/23
    )
    assert (await c.get("/api/v1/bundle", headers=BEARER)).status == 401  # peer is 127.0.0.1


async def test_failed_attempts_are_rate_limited(aiohttp_client, tmp_path):
    t = [0.0]
    auth = BundleAuth(TOKEN, None, clock=lambda: t[0])
    app2 = create_app(tmp_path, auth=NoAuth(), bundle_auth=auth)
    c = await aiohttp_client(app2)
    codes = [(await c.get("/api/v1/bundle")).status for _ in range(BundleAuth.MAX_FAILS)]
    assert set(codes) == {401}
    r = await c.get("/api/v1/bundle")  # a wrong token is now throttled...
    assert r.status == 429 and r.headers.get("Retry-After") == "60"
    # ...but the right one never is (Core and every add-on share one source IP)
    assert (await c.get("/api/v1/bundle", headers=BEARER)).status == 404
    t[0] += BundleAuth.WINDOW + 1
    assert (await c.get("/api/v1/bundle")).status == 401


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
    app = create_app(tmp_path, auth=NoAuth(), bundle_token=token, bundle_networks=None)
    c = await aiohttp_client(app)
    await put_draft(c)
    seen = []
    for method, path in [
        ("GET", "/"),
        ("GET", "/api/v1/health"),
        ("GET", "/api/v1/config?full=1"),
        ("GET", "/api/v1/config/export_yaml"),
        ("POST", "/api/v1/config/apply"),
        ("GET", "/api/v1/config?full=1"),
        ("GET", "/api/v1/nope"),
    ]:
        r = await c.request(method, path, headers=H, json={} if method == "POST" else None)
        seen.append(str(r.headers) + await r.text())
    r = await c.get("/api/v1/bundle", headers={"Authorization": f"Bearer {token}x"})
    seen.append(str(r.headers) + await r.text())
    r = await c.get("/api/v1/bundle", headers={"Authorization": f"Bearer {token}"})
    assert token not in str(r.headers) and token not in json.dumps(await r.json())
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


async def test_shutdown_wakes_long_poll_waiters_with_204(aiohttp_client, tmp_path):
    app = create_app(
        tmp_path, auth=NoAuth(), bundle_token=TOKEN, bundle_networks=None, wait_seconds=30
    )
    c = await aiohttp_client(app)
    ws = [asyncio.ensure_future(c.get("/api/v1/bundle/wait", headers=BEARER)) for _ in range(3)]
    await asyncio.sleep(0.1)
    assert not any(w.done() for w in ws)
    t0 = time.monotonic()
    await app.shutdown()
    rs = await asyncio.wait_for(asyncio.gather(*ws), 2)
    assert [r.status for r in rs] == [204] * 3 and time.monotonic() - t0 < 2
    late = await c.get("/api/v1/bundle/wait", headers=BEARER)  # a poll racing shutdown
    assert late.status == 204


def test_main_passes_a_short_shutdown_timeout(monkeypatch, tmp_path):
    from aiohttp import web

    from harmony_addon import __main__ as m

    seen = {}
    monkeypatch.setattr(web, "run_app", lambda app, **kw: seen.update(kw))
    monkeypatch.setattr("sys.argv", ["x", "--data-dir", str(tmp_path)])
    monkeypatch.delenv("SUPERVISOR_TOKEN", raising=False)
    m.main()
    assert seen["shutdown_timeout"] <= 5


async def test_every_bundle_route_needs_the_token_and_never_ingress(aiohttp_client, tmp_path):
    from test_addon import BUNDLE_ROUTES

    c = await aiohttp_client(
        create_app(tmp_path, auth=NoAuth(), bundle_token=TOKEN, bundle_networks=None)
    )
    ing = {"X-Ingress-Path": "/api/hassio_ingress/t"}
    assert BUNDLE_ROUTES
    for method, path in BUNDLE_ROUTES:
        h = {"Content-Type": "application/json"}
        assert (await c.request(method, path, json={}, headers=h)).status == 401, path
        r = await c.request(method, path, json={}, headers={**h, **BEARER, **ing})
        assert r.status == 401, path
