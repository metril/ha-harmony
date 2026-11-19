"""Home Assistant catalog routes: filtering, caps, caching, 503, token hygiene, GET-only client."""

import inspect
import logging

import pytest
from aiohttp import web
from test_addon import H

from harmony_addon import ha_client
from harmony_addon.app import create_app
from harmony_addon.auth import NoAuth
from harmony_addon.ha_client import Catalog, HaUnavailable, HttpHaApi

STATES = [
    {"entity_id": "light.kitchen", "state": "on", "attributes": {"friendly_name": "Kitchen"}},
    {"entity_id": "light.attic", "state": "off", "attributes": {"friendly_name": "attic lamp"}},
    {"entity_id": "switch.fan", "state": "on", "attributes": {}},
    {"entity_id": "sensor.temp", "state": "21", "attributes": {"device_class": "temperature"}},
    {"entity_id": "person.bob", "state": "home", "attributes": {}},  # domain not allow-listed
    {"entity_id": "nodot", "state": "x"},
    "junk",
]
SERVICES = [
    {"domain": "light", "services": {"turn_on": {"name": "Turn on", "fields": {"brightness": {}}}}},
    {"domain": "shell_command", "services": {"rm": {}}},
]


class FakeApi:
    def __init__(self, states=STATES, services=SERVICES, fail=False):
        self.data = {"/states": states, "/services": services}
        self.calls, self.fail = [], fail

    async def get(self, path):
        self.calls.append(path)
        if self.fail:
            raise HaUnavailable("down")
        return self.data[path]


def make(aiohttp_client, tmp_path, api, clock=None):
    return aiohttp_client(
        create_app(tmp_path, auth=NoAuth(), ha_api=api, clock=clock or (lambda: 0.0))
    )


async def test_entities_filter_sort_and_allow_list(aiohttp_client, tmp_path):
    c = await make(aiohttp_client, tmp_path, FakeApi())
    rows = (await (await c.get("/api/v1/ha/entities?state=1")).json())["entities"]
    assert [r["entity_id"] for r in rows] == [
        "light.attic",
        "light.kitchen",
        "sensor.temp",
        "switch.fan",
    ]
    assert rows[0] == {
        "entity_id": "light.attic",
        "name": "attic lamp",
        "domain": "light",
        "state": "off",
    }
    rows = (await (await c.get("/api/v1/ha/entities?domain=light&q=KITCH")).json())["entities"]
    assert [r["entity_id"] for r in rows] == ["light.kitchen"]
    rows = (await (await c.get("/api/v1/ha/entities?q=lamp")).json())["entities"]
    assert [r["entity_id"] for r in rows] == ["light.attic"]


async def test_entities_cap_truncates_after_sorting(aiohttp_client, tmp_path, monkeypatch):
    monkeypatch.setattr(ha_client, "MAX_ENTITIES", 3)
    many = [{"entity_id": f"light.l{i:03}", "state": "on"} for i in range(49, -1, -1)]  # reversed
    c = await make(aiohttp_client, tmp_path, FakeApi(many))
    body = await (await c.get("/api/v1/ha/entities")).json()
    assert [r["entity_id"] for r in body["entities"]] == ["light.l000", "light.l001", "light.l002"]
    assert body["truncated"] is True and body["total"] == 50


async def test_services_only_allow_listed_domains(aiohttp_client, tmp_path):
    c = await make(aiohttp_client, tmp_path, FakeApi())
    rows = (await (await c.get("/api/v1/ha/services")).json())["services"]
    assert rows == [
        {"domain": "light", "service": "turn_on", "name": "Turn on", "fields": ["brightness"]}
    ]
    assert (await c.get("/api/v1/ha/services?domain=shell_command")).status == 400


@pytest.mark.parametrize(
    "qs",
    ["domain=shell_command", "domain=person", "q=" + "x" * 65, "evil=1", "domain=light&domain=x"],
)
async def test_entity_query_is_schema_validated(aiohttp_client, tmp_path, qs):
    c = await make(aiohttp_client, tmp_path, FakeApi())
    assert (await c.get(f"/api/v1/ha/entities?{qs}")).status == 400


async def test_cache_window_uses_the_injected_clock(aiohttp_client, tmp_path):
    t = [100.0]
    api = FakeApi()
    c = await make(aiohttp_client, tmp_path, api, clock=lambda: t[0])
    for _ in range(3):
        await c.get("/api/v1/ha/entities")
    assert api.calls == ["/states"]
    t[0] += ha_client.CACHE_SECONDS + 0.1
    await c.get("/api/v1/ha/entities")
    assert api.calls == ["/states", "/states"]
    await c.get("/api/v1/ha/services")
    assert api.calls[-1] == "/services"


@pytest.mark.parametrize("api", [None, FakeApi(fail=True)])
async def test_unavailable_is_503(aiohttp_client, tmp_path, api):
    c = await make(aiohttp_client, tmp_path, api)
    for path in ("entities", "services"):
        r = await c.get(f"/api/v1/ha/{path}")
        assert r.status == 503 and (await r.json())["error"]["code"] == "ha_unavailable"


async def test_garbage_answer_is_503(aiohttp_client, tmp_path):
    c = await make(aiohttp_client, tmp_path, FakeApi(states={"x": 1}, services="nope"))
    assert (await c.get("/api/v1/ha/entities")).status == 503
    assert (await c.get("/api/v1/ha/services")).status == 503


async def test_catalog_routes_are_get_only(aiohttp_client, tmp_path):
    c = await make(aiohttp_client, tmp_path, FakeApi())
    for path in ("entities", "services"):
        assert (await c.post(f"/api/v1/ha/{path}", json={}, headers=H)).status == 405


async def test_token_never_in_responses_or_logs(aiohttp_client, aiohttp_server, tmp_path, caplog):
    caplog.set_level(logging.DEBUG)
    secret = "SUPERSECRET-" + "z" * 20
    seen = []

    async def states(request):
        seen.append((request.method, request.path, request.headers.get("Authorization")))
        return web.json_response(STATES)

    async def boom(request):
        seen.append((request.method, request.path, request.headers.get("Authorization")))
        return web.Response(status=500, text=f"oops {secret}")

    core = web.Application()
    core.router.add_get("/core/api/states", states)
    core.router.add_get("/core/api/services", boom)
    srv = await aiohttp_server(core)
    api = HttpHaApi(secret, f"http://127.0.0.1:{srv.port}/core/api")
    assert secret not in repr(api) and secret not in str(api)
    c = await make(aiohttp_client, tmp_path, api)
    r1 = await c.get("/api/v1/ha/entities")
    r2 = await c.get("/api/v1/ha/services")
    assert r1.status == 200 and r2.status == 503
    assert secret not in await r1.text() + await r2.text() + caplog.text
    assert seen == [
        ("GET", "/core/api/states", f"Bearer {secret}"),
        ("GET", "/core/api/services", f"Bearer {secret}"),
    ]


def test_the_ha_client_cannot_send_a_body():
    public = {
        n for n, _ in inspect.getmembers(HttpHaApi, inspect.isfunction) if not n.startswith("_")
    }
    assert public == {"get"}
    assert list(inspect.signature(HttpHaApi.get).parameters) == ["self", "path"]
    src = inspect.getsource(ha_client)
    for needle in (".post(", ".put(", ".patch(", ".delete(", "json=", "data=", "request("):
        assert needle not in src
    assert not [n for n in dir(Catalog) if n in ("post", "call", "call_service")]


def test_default_ha_api_needs_the_supervisor_token(monkeypatch):
    monkeypatch.delenv("SUPERVISOR_TOKEN", raising=False)
    assert ha_client.default_ha_api() is None
    monkeypatch.setenv("SUPERVISOR_TOKEN", "t")
    assert isinstance(ha_client.default_ha_api(), HttpHaApi)
