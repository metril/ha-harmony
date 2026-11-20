"""Home Assistant registries over a read-only WebSocket: allow-list, merge, exclusions, fallback,
caching, size. A fake Core WebSocket runs on an ephemeral loopback port (xdist-safe)."""

import inspect
import json
import logging

import pytest
from aiohttp import WSMsgType, web
from test_addon import H

from harmony_addon import ha_client, ha_ws
from harmony_addon.app import create_app
from harmony_addon.auth import NoAuth
from harmony_addon.ha_client import Catalog, HaUnavailable
from harmony_addon.ha_ws import ALLOWED_COMMANDS, HaWsRegistry, RegistryUnavailable

TOKEN = "SECRET-" + "q" * 24
AREAS = [
    {"area_id": "lr", "name": "Living room", "floor_id": "f1"},
    {"area_id": "kt", "name": "Kitchen", "floor_id": None},
]
FLOORS = [{"floor_id": "f1", "name": "Ground"}]
DEVICES = [
    {"id": "d1", "name": "Hue bulb", "name_by_user": "Reading lamp", "area_id": "lr"},
    {"id": "d2", "name": "Plug", "area_id": "kt"},
]
ENTITIES = [
    {"entity_id": "light.lamp", "device_id": "d1", "area_id": None, "platform": "hue"},
    {"entity_id": "switch.plug", "device_id": "d2", "area_id": "lr", "platform": "tplink"},
    {"entity_id": "light.off", "device_id": "d1", "disabled_by": "user", "platform": "hue"},
    {"entity_id": "light.hid", "device_id": "d1", "hidden_by": "integration", "platform": "hue"},
    {"entity_id": "sensor.rssi", "device_id": "d1", "entity_category": "diagnostic", "platform": "hue"},
    {"entity_id": "switch.cfg", "device_id": "d2", "entity_category": "config", "platform": "tplink"},
]
STATES = [
    {"entity_id": e, "state": "on", "attributes": {"friendly_name": n}}
    for e, n in [
        ("light.lamp", "Living room lamp"),
        ("switch.plug", "Plug"),
        ("light.off", "Off light"),
        ("light.hid", "Hidden light"),
        ("sensor.rssi", "Signal"),
        ("switch.cfg", "Setting"),
        ("scene.movie", "Movie"),
    ]
]
LISTS = {
    "config/area_registry/list": AREAS,
    "config/floor_registry/list": FLOORS,
    "config/label_registry/list": [{"label_id": "cozy", "name": "Cozy"}],
    "config/device_registry/list": DEVICES,
    "config/entity_registry/list": ENTITIES,
}


class FakeCore:
    """A Core WebSocket that records every message it receives."""

    def __init__(self, *, auth_ok=True, big=0, fail=()):
        self.received, self.auth_ok, self.big, self.fail = [], auth_ok, big, fail
        self.lists = dict(LISTS)
        self.connections = 0

    async def handler(self, request):
        ws = web.WebSocketResponse()
        await ws.prepare(request)
        self.connections += 1
        await ws.send_json({"type": "auth_required"})
        async for msg in ws:
            if msg.type != WSMsgType.TEXT:
                break
            data = json.loads(msg.data)
            self.received.append(data)
            if data["type"] == "auth":
                await ws.send_json({"type": "auth_ok" if self.auth_ok else "auth_invalid"})
                if not self.auth_ok:
                    await ws.close()
                continue
            cmd = data["type"]
            if cmd in self.fail or cmd not in self.lists:
                await ws.send_json({"id": data["id"], "type": "result", "success": False})
                continue
            result = self.lists[cmd]
            if self.big and cmd == "config/entity_registry/list":
                result = [{"entity_id": "x.y", "pad": "z" * self.big}]
            await ws.send_json({"id": data["id"], "type": "result", "success": True, "result": result})
        return ws


async def serve(aiohttp_server, core):
    app = web.Application()
    app.router.add_get("/websocket", core.handler)
    srv = await aiohttp_server(app)
    return HaWsRegistry(TOKEN, f"ws://127.0.0.1:{srv.port}/websocket")


class FakeApi:
    def __init__(self, states=STATES):
        self.states, self.calls = states, []

    async def get(self, path):
        self.calls.append(path)
        if path == "/states":
            return self.states
        raise HaUnavailable("x")


class FakeRegistry:
    def __init__(self, lists):
        self._lists = lists

    async def lists(self):
        return self._lists


async def test_only_allow_listed_read_commands_are_ever_sent(aiohttp_server):
    core = FakeCore()
    reg = await serve(aiohttp_server, core)
    await reg.lists()
    sent = {m["type"] for m in core.received if m["type"] != "auth"}
    assert sent == ALLOWED_COMMANDS
    assert all(c.endswith("/list") and c.startswith("config/") for c in ALLOWED_COMMANDS)
    assert core.received[0] == {"type": "auth", "access_token": TOKEN}


async def test_a_command_outside_the_allow_list_is_refused_before_sending():
    for bad in ("config/entity_registry/update", "call_service", "subscribe_events", "auth", ""):
        with pytest.raises(RegistryUnavailable, match="command_not_allowed"):
            await HaWsRegistry._send(None, 1, bad)  # type: ignore[arg-type]  # refused before use
    assert ALLOWED_COMMANDS == {
        "config/area_registry/list",
        "config/device_registry/list",
        "config/entity_registry/list",
        "config/floor_registry/list",
        "config/label_registry/list",
    }
    # no public entry point takes a command
    pub = [n for n, _ in inspect.getmembers(HaWsRegistry, inspect.isfunction) if not n.startswith("_")]
    assert pub == ["lists"] and list(inspect.signature(HaWsRegistry.lists).parameters) == ["self"]
    assert inspect.getsource(ha_ws).count("send_json(") == 2  # the auth message and the checked command


async def test_merge_exclusions_and_diagnostic_toggle(aiohttp_server):
    reg = await serve(aiohttp_server, FakeCore())
    cat = Catalog(FakeApi(), lambda: 0.0, reg)
    body = await cat.catalog()
    rows = {r["entity_id"]: r for r in body["entities"]}
    assert set(rows) == {"light.lamp", "switch.plug", "scene.movie"}  # disabled/hidden/diag gone
    assert rows["light.lamp"] == {
        "entity_id": "light.lamp",
        "name": "Living room lamp",
        "domain": "light",
        "area_id": "lr",  # from the device
        "device_id": "d1",
        "platform": "hue",
    }
    assert rows["switch.plug"]["area_id"] == "lr"  # the entity's own area beats its device's
    assert "area_id" not in rows["scene.movie"] and "platform" not in rows["scene.movie"]
    assert body["registries"] is True and body["total"] == 3
    assert body["areas"] == [{"id": "lr", "name": "Living room", "floor_id": "f1", "floor": "Ground"}]
    assert {d["id"]: d["name"] for d in body["devices"]} == {"d1": "Reading lamp", "d2": "Plug"}  # name_by_user wins
    full = await cat.catalog(diagnostic=True)
    ids = {r["entity_id"] for r in full["entities"]}
    assert ids == {"light.lamp", "switch.plug", "scene.movie", "sensor.rssi", "switch.cfg"}
    assert next(r for r in full["entities"] if r["entity_id"] == "sensor.rssi")["diag"] is True


async def test_labels_come_from_the_entity_and_its_device_by_id():
    lists = {
        **LISTS,
        "config/label_registry/list": [
            {"label_id": "cozy", "name": "Cozy"},
            {"label_id": "tv", "name": "Telly"},
        ],
        "config/device_registry/list": [{**DEVICES[0], "labels": ["cozy"]}, DEVICES[1]],
        "config/entity_registry/list": [{**ENTITIES[0], "labels": ["tv", "gone"]}, *ENTITIES[1:]],
    }
    cat = Catalog(FakeApi(), lambda: 0.0, FakeRegistry(lists))
    body = await cat.catalog()
    rows = {r["entity_id"]: r for r in body["entities"]}
    assert rows["light.lamp"]["label_ids"] == ["cozy", "tv"]  # "gone" is not a label
    assert "label_ids" not in rows["switch.plug"]
    assert body["labels"] == [{"id": "cozy", "name": "Cozy"}, {"id": "tv", "name": "Telly"}]
    assert [r["entity_id"] for r in (await cat.catalog(q="telly hue"))["entities"]] == ["light.lamp"]


async def test_area_q_and_domain_filters(aiohttp_server):
    cat = Catalog(FakeApi(), lambda: 0.0, await serve(aiohttp_server, FakeCore()))
    assert [r["entity_id"] for r in (await cat.catalog(area="lr"))["entities"]] == [
        "light.lamp",
        "switch.plug",
    ]
    assert [r["entity_id"] for r in (await cat.catalog(q="reading lamp"))["entities"]] == ["light.lamp"]
    assert [r["entity_id"] for r in (await cat.catalog(q="living plug"))["entities"]] == ["switch.plug"]
    assert [r["entity_id"] for r in (await cat.catalog(domain="scene"))["entities"]] == ["scene.movie"]


@pytest.mark.parametrize(
    "core",
    [FakeCore(auth_ok=False), FakeCore(fail=("config/entity_registry/list",)), None],
    ids=["auth_refused", "command_failed", "unreachable"],
)
async def test_fallback_to_rest_only_when_the_registries_fail(aiohttp_server, core):
    reg = await serve(aiohttp_server, core) if core else HaWsRegistry(TOKEN, "ws://127.0.0.1:9/ws")
    body = await Catalog(FakeApi(), lambda: 0.0, reg).catalog()
    assert body["registries"] is False and body["areas"] == []
    assert len(body["entities"]) == 7  # nothing excluded without registry data
    assert all(set(r) <= {"entity_id", "name", "domain", "device_class"} for r in body["entities"])


async def test_no_registry_configured_is_rest_only():
    body = await Catalog(FakeApi(), lambda: 0.0).catalog()
    assert body["registries"] is False and len(body["entities"]) == 7


async def test_floors_are_optional(aiohttp_server):
    core = FakeCore(fail=("config/floor_registry/list",))
    body = await Catalog(FakeApi(), lambda: 0.0, await serve(aiohttp_server, core)).catalog()
    assert body["registries"] is True and body["areas"] == [{"id": "lr", "name": "Living room"}]


async def test_labels_are_optional_and_listed_when_present(aiohttp_server):
    core = FakeCore(fail=("config/label_registry/list",))
    body = await Catalog(FakeApi(), lambda: 0.0, await serve(aiohttp_server, core)).catalog()
    assert body["registries"] is True and body["labels"] == []
    core = FakeCore()
    body = await Catalog(FakeApi(), lambda: 0.0, await serve(aiohttp_server, core)).catalog()
    assert "config/label_registry/list" in {m["type"] for m in core.received}


async def test_registries_are_cached_for_a_minute_and_a_failed_refresh_keeps_the_last_copy(aiohttp_server):
    t = [0.0]
    core = FakeCore()
    cat = Catalog(FakeApi(), lambda: t[0], await serve(aiohttp_server, core))
    for _ in range(3):
        await cat.catalog()
    assert core.connections == 1
    t[0] = ha_client.REGISTRY_SECONDS + 1
    await cat.catalog()
    assert core.connections == 2
    core.auth_ok = False
    t[0] += ha_client.REGISTRY_SECONDS + 1
    body = await cat.catalog()
    assert core.connections == 3 and body["registries"] is True
    await cat.catalog()
    assert core.connections == 3  # the failure is not retried at once


async def test_a_message_over_the_cap_is_unavailable_not_a_crash(aiohttp_server, monkeypatch):
    monkeypatch.setattr(ha_ws, "MAX_MESSAGE", 4096)
    reg = await serve(aiohttp_server, FakeCore(big=20000))
    with pytest.raises(RegistryUnavailable):
        await reg.lists()
    assert (await Catalog(FakeApi(), lambda: 0.0, reg).catalog())["registries"] is False


async def test_token_never_logged_and_one_warning_per_cause(aiohttp_server, caplog):
    caplog.set_level(logging.DEBUG)
    reg = await serve(aiohttp_server, FakeCore(auth_ok=False))
    assert TOKEN not in repr(reg) and TOKEN not in str(reg)
    t = [0.0]
    cat = Catalog(FakeApi(), lambda: t[0], reg)
    for _ in range(3):
        await cat.catalog()
        t[0] += ha_client.FAILED_RETRY_SECONDS + 1
    warns = [r for r in caplog.records if r.levelno == logging.WARNING]
    assert len(warns) == 1 and "registries cannot be read: auth_failed" in warns[0].getMessage()
    assert TOKEN not in caplog.text


async def test_large_catalog_is_compact_and_etagged(aiohttp_client, aiohttp_server, tmp_path):
    n_area = 25
    areas = [{"area_id": f"a{i}", "name": f"Area {i}"} for i in range(n_area)]
    devs = [{"id": f"d{i}", "name": f"Device {i}", "area_id": f"a{i % n_area}"} for i in range(1000)]
    states, ents = [], []
    doms = ["light", "switch", "sensor", "scene", "script", "media_player"]
    for i in range(3000):
        eid = f"{doms[i % 6]}.thing_{i}"
        states.append({"entity_id": eid, "state": "on", "attributes": {"friendly_name": f"Thing {i}"}})
        ents.append({"entity_id": eid, "device_id": f"d{i % 1000}", "platform": f"integ{i % 40}"})
    core = FakeCore()
    core.lists.update(
        {
            "config/area_registry/list": areas,
            "config/device_registry/list": devs,
            "config/entity_registry/list": ents,
        }
    )
    reg = await serve(aiohttp_server, core)
    c = await aiohttp_client(
        create_app(tmp_path, auth=NoAuth(), ha_api=FakeApi(states), ha_registry=reg, clock=lambda: 0.0)
    )
    r = await c.get("/api/v1/ha/entities")
    raw = await r.read()
    body = json.loads(raw)
    assert len(body["entities"]) == 3000 and len(body["areas"]) == n_area
    assert all("state" not in r for r in body["entities"])  # a state change must not change the ETag
    assert len(raw) < 500_000  # ~150 B per entity, not a multi-MB answer
    etag = r.headers["ETag"]
    r2 = await c.get("/api/v1/ha/entities", headers={"If-None-Match": etag})
    assert r2.status == 304 and await r2.read() == b""
    r3 = await c.get("/api/v1/ha/entities?domain=scene&area=a3")
    assert r3.status == 200 and r3.headers["ETag"] != etag
    assert (await c.get("/api/v1/ha/entities?diagnostic=2")).status == 400
    assert (await c.post("/api/v1/ha/entities", json={}, headers=H)).status == 405


async def test_etag_accepts_weak_and_quoted_validators(aiohttp_client, tmp_path):
    c = await aiohttp_client(create_app(tmp_path, auth=NoAuth(), ha_api=FakeApi(), clock=lambda: 0.0))
    etag = (await c.get("/api/v1/ha/entities")).headers["ETag"]
    for sent in (etag, "W/" + etag, f'"zzz", W/{etag}', "*"):
        assert (await c.get("/api/v1/ha/entities", headers={"If-None-Match": sent})).status == 304
    assert (await c.get("/api/v1/ha/entities", headers={"If-None-Match": '"nope"'})).status == 200


async def test_etag_ignores_state_changes_but_state_is_on_request():
    api = FakeApi([dict(s) for s in STATES])
    t = [0.0]
    cat = Catalog(api, lambda: t[0])
    one = await cat.catalog_body()
    api.states[0]["state"] = "off"
    t[0] += ha_client.CACHE_SECONDS + 1
    assert await cat.catalog_body() == one
    assert '"state"' not in one[0]
    with_state = await cat.catalog_body(state=True)
    assert '"state":"off"' in with_state[0] and with_state[1] != one[1]


async def test_the_body_is_memoised_until_the_sources_change():
    api = FakeApi()
    t = [0.0]
    cat = Catalog(api, lambda: t[0])
    a = await cat.catalog_body(q="plug")
    assert await cat.catalog_body(q="plug") is a  # not rebuilt, not re-serialised
    t[0] += ha_client.CACHE_SECONDS + 1
    api.states = list(api.states)  # a new answer from Home Assistant
    assert await cat.catalog_body(q="plug") is not a
    assert api.calls == ["/states", "/states"]


async def test_concurrent_cache_misses_share_one_fetch(aiohttp_server):
    import asyncio

    core = FakeCore()
    reg = await serve(aiohttp_server, core)

    class Slow(FakeApi):
        async def get(self, path):
            await asyncio.sleep(0.05)
            return await super().get(path)

    api = Slow()
    cat = Catalog(api, lambda: 0.0, reg)
    bodies = await asyncio.gather(*(cat.catalog() for _ in range(8)))
    assert api.calls == ["/states"] and core.connections == 1
    assert all(b["total"] == 3 for b in bodies)


async def test_wrong_typed_registry_fields_do_not_break_the_catalog():
    lists = {
        "config/area_registry/list": [{"area_id": "lr", "name": ["x"], "floor_id": 5}, "junk", {"area_id": []}],
        "config/floor_registry/list": "nope",
        "config/label_registry/list": [{"label_id": "a", "name": None}],
        "config/device_registry/list": [{"id": "d1", "name": 3, "area_id": ["x"], "labels": "oops"}, 7],
        "config/entity_registry/list": [
            {"entity_id": "light.lamp", "device_id": ["d1"], "area_id": {}, "labels": "cozy", "platform": 4},
            {"entity_id": "switch.plug", "device_id": "d1", "labels": ["a", 3, None], "entity_category": []},
            None,
        ],
    }
    body = await Catalog(FakeApi(), lambda: 0.0, FakeRegistry(lists)).catalog()
    assert body["registries"] is True and body["total"] == 7
    rows = {r["entity_id"]: r for r in body["entities"]}
    assert "area_id" not in rows["light.lamp"] and rows["switch.plug"]["label_ids"] == ["a"]


async def test_same_named_areas_stay_apart_because_they_are_keyed_by_id():
    lists = {
        **LISTS,
        "config/area_registry/list": [
            {"area_id": "lr", "name": "Hall", "floor_id": "f1"},
            {"area_id": "kt", "name": "Hall", "floor_id": "f2"},
        ],
        "config/floor_registry/list": [{"floor_id": "f1", "name": "Up"}, {"floor_id": "f2", "name": "Down"}],
        "config/entity_registry/list": [ENTITIES[0], {**ENTITIES[1], "area_id": None}],
    }
    body = await Catalog(FakeApi(), lambda: 0.0, FakeRegistry(lists)).catalog()
    assert {(a["id"], a["floor"]) for a in body["areas"]} == {("lr", "Up"), ("kt", "Down")}
    only = await Catalog(FakeApi(), lambda: 0.0, FakeRegistry(lists)).catalog(area="kt")
    assert [r["entity_id"] for r in only["entities"]] == ["switch.plug"]  # via its device, in the other Hall
