"""Audit fixes: restart, desync, waste, held repeats, errors, reconnects, performance, startup."""
# ruff: noqa: F811  (imported fixtures are used by name)

import asyncio

import pytest
from homeassistant.helpers import entity_registry as er
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
    sent = lambda: [d["command"] for t, d in hub.got if t == "command"]
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
