"""Audit fixes: restart, desync, waste, held repeats, errors, reconnects, performance, startup."""
# ruff: noqa: F811  (imported fixtures are used by name)

import asyncio

import pytest
from homeassistant.helpers import entity_registry as er
from pytest_homeassistant_custom_component.common import async_mock_service as async_mock_service_
from test_ha_runtime import (  # noqa: F401  (fixtures)
    THREE_PAGES,
    V1_INFO,
    V2_INFO,
    YAML,
    Clock,
    ScriptHub,
    _fast_tiles,
    clock,
    press,
    rig,
    settle,
)

from custom_components.ha_harmony import const
from custom_components.ha_harmony.harmony_core.harmony_proto import MsgType

pytestmark = pytest.mark.usefixtures("enable_custom_integrations", "fast_client")


async def _until(pred, timeout=3.0):
    """Poll without hass.async_block_till_done (a service blocked on the manual clock holds it)."""
    async with asyncio.timeout(timeout):
        while not pred():
            await asyncio.sleep(0.005)


# 1: a restart with cache + link never removes the add-on link sensor's registry entry
async def test_restart_keeps_the_addon_link_registry_entry(hass, rig, clock):
    _hub, c = await rig()
    ereg = er.async_get(hass)
    ent_id = ereg.async_get_entity_id("sensor", "ha_harmony", f"{c.hub_id}_addon_link")
    assert ent_id
    removed = []
    hass.bus.async_listen(
        er.EVENT_ENTITY_REGISTRY_UPDATED,
        lambda ev: removed.append(ev.data["entity_id"]) if ev.data["action"] == "remove" else None,
    )
    await hass.config_entries.async_reload(c.config_entry.entry_id)
    await settle(hass)
    assert removed == []


# 2: pages only for a config the hub accepted
async def test_pages_follow_only_a_hub_accepted_config(hass, rig, clock, fake_addon):
    hub, c = await rig()
    fake_addon.apply_yaml(YAML.replace("name: Stream", "name: Stream2"))
    hub.reject_config = True
    with pytest.raises(Exception, match="rejected"):
        await c.async_refresh_from_addon()
    hub.got.clear()
    await hub.push("remote_attached", {})
    await hub.push("remote_info", V2_INFO)
    await hub.push("battery", {"remote": 1, "percent": 50, "millivolts": 3000})
    await settle(hass)
    await asyncio.sleep(0.2)
    await settle(hass)
    assert hub.pages() == []  # the rejected config never reaches the remote
    hub.reject_config = False
    await c.async_sync_config()
    n = len(c.compiled.pages_v2)
    await settle(hass, lambda: len(hub.pages()) >= n)
    await asyncio.sleep(0.2)
    await settle(hass)
    assert len(hub.pages()) == n
    assert {p.gen for p in hub.pages()} == {c.compiled.models[0].gen}


# 3: a service reload while a push runs does not restart it
async def test_reload_config_with_the_same_rev_sends_the_set_once(hass, rig, clock):
    hub, c = await rig(THREE_PAGES)
    await hub.push("remote_info", V2_INFO)
    await settle(hass, lambda: c.links[1].pushed and c.links[1].pushed[1] == 2)
    hub.gate = asyncio.Event()
    hub.got.clear()
    await hub.push("remote_attached", {})
    await hub.push("remote_info", V2_INFO)
    await settle(hass, lambda: len(hub.pages()) == 1)
    await c.async_refresh_from_addon()  # the add-on's same-rev answer, as the long-poll delivers it
    await c.async_refresh_from_addon()
    hub.gate.set()
    await settle(
        hass, lambda: c.links[1].pushed == c.links[1].inflight is None or c.links[1].pushed
    )
    await asyncio.sleep(0.2)
    await settle(hass)
    assert len(hub.pages()) == len(c.compiled.pages_v2)


# 4: held repeats never pile up on a slow service; a release stops them
async def test_hold_repeats_do_not_queue_on_a_slow_service(hass, rig, clock):
    hass.states.async_set("remote.shield", "on")
    hub, _c = await rig()
    running = peak = starts = 0

    async def slow(call):
        nonlocal running, peak, starts
        starts += 1
        running += 1
        peak = max(peak, running)
        await asyncio.sleep(0.25)
        running -= 1

    hass.services.async_register("remote", "send_command", slow)
    DOWN = 2
    await press(hub, DOWN, 0)
    for _ in range(12):  # a long hold against a service slower than the hold cadence
        await press(hub, DOWN, 2)
        await asyncio.sleep(0.04)
    await press(hub, DOWN, 1)
    await press(hub, DOWN, 2)  # a stray hold after the release
    await settle(hass, lambda: running == 0)
    done = starts
    await asyncio.sleep(0.4)
    assert starts == done and peak == 1 and done <= 4, (starts, peak)


# 5: remote.send_command honours the standard arguments; user-initiated failures raise
async def test_send_command_honours_repeats_delay_and_hold(hass, rig, clock):
    from homeassistant.components.remote import DOMAIN as REMOTE

    hub, _c = await rig()
    hub.got.clear()
    ent = "remote.harmony_hub_sim_hub"
    assert hass.states.get(ent)
    task = asyncio.ensure_future(
        hass.services.async_call(
            REMOTE,
            "send_command",
            {"entity_id": ent, "command": ["a", "b"], "num_repeats": 2, "delay_secs": 0.5},
            blocking=True,
        )
    )
    sent = lambda: [
        d["command"] for t, d in hub.got if t == "command" and d["command"] != "tile_update"
    ]
    waits = 0
    while not task.done():
        await _until(lambda: task.done() or clock.active())
        if clock.active():
            waits += 1
            clock.elapse()
    await task
    assert sent() == ["a", "b", "a", "b"] and waits == 3  # 4 sends, 3 delays between them
    assert clock.waits[-3:] == [0.5] * 3
    # hold_secs resends at the hold cadence until the time has passed
    hub.got.clear()
    clock.waits.clear()
    task = asyncio.ensure_future(
        hass.services.async_call(
            REMOTE,
            "send_command",
            {"entity_id": ent, "command": ["vol"], "hold_secs": 1.0},
            blocking=True,
        )
    )
    while not task.done():
        await _until(lambda: task.done() or clock.active())
        clock.elapse()
    await task
    assert len(sent()) == 3 and clock.waits == [const.HOLD_REPEAT] * 2


async def test_failed_user_calls_raise_translated_errors_key_calls_only_log(
    hass, rig, clock, caplog
):
    from homeassistant.exceptions import HomeAssistantError, ServiceValidationError

    hass.states.async_set("remote.shield", "on")
    _hub, c = await rig()

    async def boom(call):
        raise RuntimeError("tv on fire")

    hass.services.async_register("remote", "send_command", boom)
    with pytest.raises(HomeAssistantError) as exc:
        await c.async_device_command("shield", "ok")  # a button / remote.send_command
    assert exc.value.translation_key == "service_failed"
    assert "tv on fire" in exc.value.translation_placeholders["error"]
    with pytest.raises(ServiceValidationError) as exc:
        await c.async_device_command("shield", "nope")
    assert exc.value.translation_key == "unknown_command"
    hass.services.async_remove("remote", "send_command")
    with pytest.raises(ServiceValidationError) as exc:
        await c.async_device_command("shield", "ok")
    assert exc.value.translation_key == "service_missing"
    # a key event never raises (nothing awaits it): it is a log line
    hass.services.async_register("remote", "send_command", boom)
    caplog.set_level("WARNING")
    c._on_push(MsgType.KEY_EVENT, {"remote": 1, "key": 2, "action": 0})
    await settle(hass)
    assert "tv on fire" in caplog.text


# 6: an activity change while HA and the hub were disconnected still runs the HA-side steps
async def test_transition_during_a_disconnect_runs_the_ha_steps(hass, rig, clock):
    from pytest_homeassistant_custom_component.common import async_mock_service

    for e in ("media_player.shield", "light.room"):
        hass.states.async_set(e, "off" if e.startswith("media") else "on")
    async_mock_service(hass, "light", "turn_off")
    hub, c = await rig()
    async_mock_service(hass, "remote", "turn_on")
    async_mock_service(hass, "remote", "send_command")
    await hub.push("state_update", {"activity": 1, "power": True})  # Stream is running
    await settle(hass, lambda: c.current_activity == "Stream")
    clock.elapse()
    await settle(hass)
    hass.states.async_set("media_player.shield", "on")  # the shield is on for Stream
    bye = async_mock_service(hass, "scene", "turn_on")
    off = async_mock_service(hass, "remote", "turn_off")
    hub.state = {"activity": 0, "power": False}  # the hub went off while we were disconnected
    await hub.ws.close()
    await settle(hass, lambda: c.available is False)
    await settle(hass, lambda: c.available and c.current_activity is None)
    await settle(hass, lambda: bye and off)
    assert len(bye) == 1 and len(off) == 1


async def test_first_report_after_ha_starts_is_only_a_baseline(hass, rig, clock):
    hass.states.async_set("media_player.shield", "off")
    on = async_mock_service_(hass, "remote", "turn_on")
    async_mock_service_(hass, "remote", "send_command")
    async_mock_service_(hass, "light", "turn_off")
    _hub, c = await rig(state={"activity": 1, "power": True})  # already running at HA start
    await settle(hass)
    assert c.current_activity == "Stream" and on == []


# 7: a flapping hub (accepts, then drops) is met with growing, jittered waits; a stable link resets
async def test_flapping_hub_backs_off_exponentially_with_jitter(fake_hub):
    import aiohttp

    from custom_components.ha_harmony.api import HarmonyHubClient

    async def handler(ws, env):
        if env["t"] == "hello":
            await ws.send_json(
                {"v": 1, "id": env["id"], "t": "hello_ack", "d": {"hub_id": "h", "activities": []}}
            )
            await ws.close()  # accepted, then dropped at once

    hub = await fake_hub(handler)
    now = 0.0
    waits: list[float] = []

    async def sleep(seconds):
        nonlocal now
        waits.append(seconds)
        now += seconds
        await asyncio.sleep(0.001)

    session = aiohttp.ClientSession()
    c = HarmonyHubClient(
        "127.0.0.1",
        hub.port,
        "t",
        session,
        backoff_min=1.0,
        backoff_max=30.0,
        clock=lambda: now,
        rand=lambda: 1.0,  # the top of the jitter range
        sleep=sleep,
    )
    await c.async_connect()
    try:
        async with asyncio.timeout(10):
            while len(waits) < 8:
                await asyncio.sleep(0.01)
    finally:
        await c.async_close()
        await session.close()
    base = [1, 2, 4, 8, 16, 30, 30, 30]  # grows although every handshake succeeded; capped
    assert [round(w / 1.2, 3) for w in waits[:8]] == base
    assert hub.connections >= 8


async def test_stable_connection_resets_the_backoff(fake_hub):
    import aiohttp

    from custom_components.ha_harmony.api import HarmonyHubClient

    conns = 0
    now = 0.0

    async def handler(ws, env):
        nonlocal conns, now
        if env["t"] == "hello":
            await ws.send_json(
                {"v": 1, "id": env["id"], "t": "hello_ack", "d": {"hub_id": "h", "activities": []}}
            )
            conns += 1
            if conns == 3:
                await asyncio.sleep(0.05)  # the client has noted the link as up
                now += 40  # this link stayed up for 40 s
            await ws.close()

    hub = await fake_hub(handler)
    waits: list[float] = []

    async def sleep(seconds):
        nonlocal now
        waits.append(seconds)
        now += seconds
        await asyncio.sleep(0.001)

    session = aiohttp.ClientSession()
    c = HarmonyHubClient(
        "127.0.0.1", hub.port, "t", session, backoff_min=1.0, backoff_max=30.0,
        clock=lambda: now, rand=lambda: 0.5, sleep=sleep,
    )  # fmt: skip
    await c.async_connect()
    try:
        async with asyncio.timeout(10):
            while len(waits) < 4:
                await asyncio.sleep(0.01)
    finally:
        await c.async_close()
        await session.close()
    assert waits[:4] == [1.0, 2.0, 1.0, 2.0]  # rand 0.5 = the middle of the jitter range


# 8: entities are written only for what they show
@pytest.fixture
def writes(monkeypatch):
    from homeassistant.helpers.entity import Entity

    seen: list[str] = []
    real = Entity.async_write_ha_state

    def counting(self):
        seen.append(self.entity_id)
        return real(self)

    monkeypatch.setattr(Entity, "async_write_ha_state", counting)
    return seen


async def test_attribute_churn_of_a_tracked_device_writes_no_entity(hass, rig, clock, writes):
    hass.states.async_set("media_player.shield", "playing", {"volume_level": 0.1})
    _hub, _c = await rig()
    await settle(hass)
    writes.clear()
    for i in range(30):  # a playing media_player: new position/volume attributes all the time
        hass.states.async_set("media_player.shield", "playing", {"volume_level": i / 100})
    await settle(hass)
    assert writes == []
    hass.states.async_set("media_player.shield", "off")  # a real change: that device only
    await settle(hass)
    assert sorted(writes) == ["binary_sensor.shield_power", "sensor.shield_input"]


async def test_battery_update_writes_only_the_battery_entities(hass, rig, clock, writes):
    hub, _c = await rig()
    await settle(hass)
    writes.clear()
    await hub.push("battery", {"remote": 1, "percent": 80, "millivolts": 3100})
    await settle(hass, lambda: writes)
    assert sorted(writes) == ["sensor.remote_1_battery", "sensor.remote_1_voltage"]
    writes.clear()
    await hub.push("battery", {"remote": 1, "percent": 80, "millivolts": 3100})  # nothing changed
    await settle(hass)
    assert writes == []


async def test_activity_change_writes_the_activity_entities_only(hass, rig, clock, writes):
    hub, _c = await rig()
    await settle(hass)
    writes.clear()
    await hub.push("state_update", {"activity": 1, "power": True})
    await settle(hass, lambda: writes)
    assert sorted(writes) == ["remote.harmony_hub_sim_hub", "select.harmony_hub_sim_hub_activity"]


# 9: the remote's reported store decides whether pages are sent
def _held(yaml=YAML, **over):
    """remote_info of a v2 remote that holds exactly the set this config compiles to."""
    import json

    from custom_components.ha_harmony.addon_link import compile_source
    from harmony_core.config_model import load_source

    c = compile_source(json.loads(json.dumps(load_source(yaml))), "sim-hub")
    return {**V2_INFO, "gen": c.gen, "pages": len(c.pages_v2), **over}


async def test_a_remote_that_holds_the_set_gets_no_pages_on_setup_or_reconnect(hass, rig, clock):
    hub, c = await rig(info=_held, pages=False)
    await settle(hass, lambda: c.links[1].pushed is not None)
    await asyncio.sleep(0.2)
    await settle(hass)
    assert hub.pages() == [] and c.config_pushes == 1
    # the HA<->hub websocket drops and comes back: the hub resends the remote's info, nothing more
    await hub.ws.close()
    await settle(hass, lambda: c.available is False)
    await settle(hass, lambda: c.available and c.links[1].info is not None)
    await asyncio.sleep(0.3)
    await settle(hass)
    assert hub.pages() == [] and c.config_pushes == 1
    assert c.links[1].pushed is not None


async def test_a_remote_with_another_set_or_fewer_pages_is_pushed_to(hass, rig, clock):
    hub, c = await rig(info=lambda y: _held(y, gen=(_held(y)["gen"] + 1) % 256), pages=False)
    await settle(hass, lambda: c.config_state == "accepted")  # the ack promotes the compiled set
    n = len(c.compiled.pages_v2)
    assert n > 0
    await settle(hass, lambda: len(hub.pages()) >= n)  # another generation: the whole set
    assert len(hub.pages()) == n
    hub.got.clear()
    await hub.push("remote_attached", {})
    await hub.push("remote_info", {**_held(), "pages": n - 1})  # incomplete store
    await settle(hass, lambda: len(hub.pages()) >= n)
    await asyncio.sleep(0.1)
    assert len(hub.pages()) == n
    hub.got.clear()
    await hub.push("remote_attached", {})
    await hub.push("remote_info", _held())  # complete and current: nothing
    await settle(hass)
    await asyncio.sleep(0.2)
    await settle(hass)
    assert hub.pages() == []


async def test_flapping_ws_link_pushes_nothing_per_reconnect(hass, rig, clock):
    hub, c = await rig(info=_held, pages=False)
    await settle(hass, lambda: c.links[1].pushed is not None)
    for _ in range(4):
        await hub.ws.close()
        await settle(hass, lambda: c.available is False)
        await settle(hass, lambda: c.available)
    await asyncio.sleep(0.3)
    await settle(hass)
    assert hub.pages() == [] and c.config_pushes == 1  # bounded work per reconnect: a hello and a resync


# 10: a hold is not an event unless asked for; event_types is built once per compile
async def test_hold_repeats_write_no_event_by_default(hass, rig, clock, writes):
    hub, c = await rig()
    await settle(hass)
    writes.clear()
    for action in (0, 2, 2, 2, 1):  # press, three hold repeats, release
        await press(hub, 4, action)
    await settle(hass, lambda: len(writes) >= 2)
    await asyncio.sleep(0.1)
    assert [e for e in writes if e.startswith("event.")] == ["event.harmony_hub_sim_hub_remote_1_keys"] * 2
    st = hass.states.get("event.harmony_hub_sim_hub_remote_1_keys")
    assert st.attributes["action"] == "release" and "remote" not in st.attributes
    hass.config_entries.async_update_entry(c.config_entry, options={"hold_events": True})
    writes.clear()
    for action in (0, 2, 2, 2, 1):
        await press(hub, 4, action)
    await settle(hass, lambda: len(writes) >= 5)
    assert [e for e in writes if e.startswith("event.")] == ["event.harmony_hub_sim_hub_remote_1_keys"] * 5


async def test_event_types_and_config_hash_are_computed_once_per_compile(hass, rig, clock):
    _hub, c = await rig()
    assert c.event_types is c.compiled.event_types is c.event_types
    assert "volume_up" in c.event_types and "unknown" in c.event_types
    assert c.compiled.config_hash is c.compiled.config_hash


# 11: first setup with nothing cached: one pull, a bounded wait, never the hub sync
async def test_first_setup_with_a_slow_addon_returns_within_the_bound(
    hass, fake_hub, fake_addon, clock, monkeypatch
):
    import time

    from conftest import make_entry

    from custom_components.ha_harmony.addon_link import AddonClient
    from custom_components.ha_harmony.const import FIRST_APPLY_WAIT  # noqa: F401

    monkeypatch.setattr(const, "FIRST_APPLY_WAIT", 0.3)
    fetches = []
    real = AddonClient.fetch

    async def slow_fetch(self, hub_id):
        fetches.append(hub_id)
        await asyncio.sleep(1.5)  # the add-on takes much longer than setup may wait
        return await real(self, hub_id)

    monkeypatch.setattr(AddonClient, "fetch", slow_fetch)
    hub = ScriptHub()
    server = await fake_hub(hub.handler)
    fake_addon.apply_yaml(YAML)
    entry = make_entry("127.0.0.1", server.port, addon=fake_addon)
    entry.add_to_hass(hass)
    t0 = time.monotonic()
    assert await hass.config_entries.async_setup(entry.entry_id)
    assert time.monotonic() - t0 < 1.0  # the bound, not the add-on's 1.5 s (let alone the 20 s timeout)
    c = entry.runtime_data
    assert c.compiled.devices == {} and hass.states.get("sensor.tv_input") is None
    await settle(hass, lambda: c.managed, timeout=5)  # the same single pull fills everything in
    await settle(hass, lambda: hass.states.get("sensor.tv_input") is not None)
    assert len(fetches) == 1  # not one from setup and one from the link loop
    await settle(hass, lambda: any(t == "set_config" for t, _ in hub.got))
    assert [t for t, _ in hub.got].count("set_config") == 1
    await hass.config_entries.async_unload(entry.entry_id)


# (d) an outgoing page is not base64-decoded just to be validated
def test_encode_ws_does_not_decode_outgoing_pages(monkeypatch):
    import base64

    from custom_components.ha_harmony.harmony_core import harmony_proto as hp

    payload = {"remote": 1, "page": 0, "data": base64.b64encode(bytes(range(251))).decode()}
    calls = []
    real = base64.b64decode
    monkeypatch.setattr(hp.base64, "b64decode", lambda *a, **k: (calls.append(1), real(*a, **k))[1])
    hp.encode_ws(hp.MsgType.PAGE_PUSH, payload, 7)
    hp._validate("page_push", payload)
    assert calls == []  # outgoing pages are not base64-decoded just to be validated
    hp._validate("page_push", payload, incoming=True)
    assert len(calls) == 1  # untrusted input is decoded once to prove it
    bad = {**payload, "data": "not base64!!"}
    with pytest.raises(hp.ProtoError):  # untrusted input is still checked in full
        hp.decode_ws(hp.encode_ws(hp.MsgType.PAGE_PUSH, bad, 7))


# (c) a burst of tile updates is pipelined, not one round trip per tile
TILES20 = """
pages:
  - id: main
    cols: 4
    rows: 6
    widgets:
""" + "".join(
    f"      - {{type: tile, id: t{i}, x: {i % 4}, y: {i // 4}, label: T{i}, entity: sensor.s{i}}}\n"
    for i in range(20)
)


async def test_twenty_tiles_flush_pipelined_within_the_window(hass, rig, clock):
    import time

    for i in range(20):
        hass.states.async_set(f"sensor.s{i}", str(i))
    hub, c = await rig(TILES20)
    await hub.push("remote_info", V2_INFO)
    await settle(hass, lambda: c.links[1].pushed and c.links[1].pushed[1] == 2)
    await settle(hass, lambda: len(hub.tiles()) >= 20)
    await asyncio.sleep(0.3)  # the start-up flushes are over
    hub.tile_delay, hub.tiles_peak = 0.05, 0
    hub.got.clear()
    for i in range(20):
        hass.states.async_set(f"sensor.s{i}", str(100 + i))
    t0 = time.monotonic()
    await _until(lambda: len(hub.tiles()) >= 20)
    await _until(lambda: hub.tiles_inflight == 0)
    elapsed = time.monotonic() - t0
    assert {t["text"] for t in hub.tiles()} == {str(100 + i) for i in range(20)}
    assert 1 < hub.tiles_peak <= const.TILE_WINDOW  # pipelined, and bounded
    assert elapsed < 20 * 0.05 * 0.6, elapsed  # 20 serial round trips would take >= 1 s


# 12: HA device on/off per domain
@pytest.mark.parametrize(
    "entity,state,want",
    [
        ("cover.garage", "open", True),
        ("cover.garage", "opening", True),
        ("cover.garage", "closed", False),
        ("cover.garage", "closing", False),
        ("cover.garage", "stopped", None),
        ("media_player.tv", "playing", True),
        ("media_player.tv", "idle", True),
        ("media_player.tv", "on", True),
        ("media_player.tv", "off", False),
        ("media_player.tv", "standby", False),
        ("media_player.tv", "unavailable", None),
        ("media_player.tv", "unknown", None),
        ("climate.hvac", "heat", True),
        ("climate.hvac", "off", False),
        ("lock.door", "unlocked", True),
        ("lock.door", "locked", False),
        ("lock.door", "jammed", None),
        ("light.a", "on", True),
        ("light.a", "off", False),
        ("switch.s", "on", True),
        ("fan.f", "off", False),
    ],
)
def test_ha_device_power_per_domain(entity, state, want):
    from homeassistant.core import State

    from custom_components.ha_harmony.coordinator import _ha_device_state

    assert _ha_device_state(State(entity, state)).power is want
    assert _ha_device_state(None).power is None
    assert _ha_device_state(State("media_player.tv", "on", {"source": "HDMI 1"})).input == "HDMI 1"


# 12: tiles follow an entity rename
async def test_tiles_follow_an_entity_rename(hass, rig, clock):
    ereg = er.async_get(hass)
    ereg.async_get_or_create("light", "test", "a1", suggested_object_id="a")
    hass.states.async_set("light.a", "on")
    hub, c = await rig()
    await hub.push("remote_info", V2_INFO)
    await settle(hass, lambda: c.links[1].pushed and c.links[1].pushed[1] == 2)
    await settle(hass, lambda: hub.tiles())
    await asyncio.sleep(0.2)
    hub.got.clear()
    ereg.async_update_entity("light.a", new_entity_id="light.kitchen")
    hass.states.async_remove("light.a")  # what the entity platform does on a rename
    hass.states.async_set("light.kitchen", "off")
    await settle(hass)
    await _until(lambda: any(t["state"] == 1 for t in hub.tiles() if "state" in t))
    hub.got.clear()
    hass.states.async_set("light.kitchen", "on")  # later changes of the renamed entity still arrive
    await _until(lambda: any(t.get("state") == 2 for t in hub.tiles()))


# 12: diagnostics, entry removal, cleanup, static quality checks
async def test_diagnostics_are_redacted_and_complete(hass, rig, clock):
    import json

    from custom_components.ha_harmony.diagnostics import async_get_config_entry_diagnostics

    hub, c = await rig()
    await hub.push("remote_info", {**V2_INFO, "gen": 9, "pages": 3})
    await settle(hass, lambda: c.links[1].info is not None)
    diag = await async_get_config_entry_diagnostics(hass, c.config_entry)
    text = json.dumps(diag, default=str)
    for secret in (c.config_entry.data["token"], c.config_entry.data["addon_token"], "127.0.0.1"):
        assert secret not in text
    assert diag["entry"]["data"]["token"] == "**REDACTED**"
    r1 = diag["remotes"]["1"]
    assert r1["announced"]["stored_gen"] == 9 and r1["announced"]["stored_pages"] == 3
    assert r1["page_format"] == 2 and diag["config"]["pages_v2"] == len(c.compiled.pages_v2)
    assert diag["addon_link"]["state"] == "ok" and diag["hub"]["config_accepted"] is True
    assert diag["timings_ms"]["compile_ms"] >= 0 and "set_config_ms" in diag["timings_ms"]
    assert diag["counters"]["config_pushes"] == 1


async def test_removing_the_entry_deletes_its_cached_bundle(hass, rig, clock):
    from homeassistant.helpers.storage import Store

    _hub, c = await rig()
    store = Store(hass, 1, f"ha_harmony.bundle.{c.hub_id}")
    assert await store.async_load() is not None
    await hass.config_entries.async_remove(c.config_entry.entry_id)
    assert await Store(hass, 1, f"ha_harmony.bundle.{c.hub_id}").async_load() is None


async def test_registry_entries_of_a_gone_remote_are_removed(hass, rig, clock, fake_addon):
    from homeassistant.helpers import device_registry as dr

    _hub, c = await rig()
    ereg, dreg = er.async_get(hass), dr.async_get(hass)
    entry_id = c.config_entry.entry_id
    dev = dreg.async_get_or_create(
        config_entry_id=entry_id, identifiers={("ha_harmony", f"{c.hub_id}_remote_2")}
    )
    ereg.async_get_or_create(
        "sensor", "ha_harmony", f"{c.hub_id}_battery_2", config_entry=c.config_entry, device_id=dev.id
    )
    fake_addon.apply_yaml(YAML.replace("name: Stream", "name: Stream3"))  # any config change
    await c.async_refresh_from_addon()
    assert ereg.async_get_entity_id("sensor", "ha_harmony", f"{c.hub_id}_battery_2") is None
    assert dreg.async_get(dev.id) is None or entry_id not in dreg.async_get(dev.id).config_entries


def test_translations_icons_and_parallel_updates_are_complete():
    import json
    import re
    from pathlib import Path

    pkg = Path(__file__).resolve().parents[2] / "custom_components" / "ha_harmony"
    strings = json.loads((pkg / "strings.json").read_text())
    assert json.loads((pkg / "translations" / "en.json").read_text()) == strings
    icons = json.loads((pkg / "icons.json").read_text())
    for py in pkg.glob("*.py"):
        text = py.read_text()
        domain = py.stem
        for key in re.findall(r'_attr_translation_key = "(\w+)"', text):
            assert key in strings["entity"][domain], (py.name, key)
            assert key in icons["entity"][domain], (py.name, key)
        if domain in {"sensor", "binary_sensor", "select", "button", "event", "remote"}:
            assert "PARALLEL_UPDATES = 0" in text, py.name
    for key in re.findall(r'(?:ha_error|invalid)\(\s*"(\w+)"', "".join(p.read_text() for p in pkg.glob("*.py"))):
        assert key in strings["exceptions"], key
    for svc in ("reload_config", "push_pages", "send_ir", "set_device_state"):
        assert svc in icons["services"] and svc in strings["services"]
