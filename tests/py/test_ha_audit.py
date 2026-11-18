"""Audit fixes: restart, desync, waste, held repeats, errors, reconnects, performance, startup."""

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

pytestmark = pytest.mark.usefixtures("enable_custom_integrations", "fast_client")


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
    await settle(hass, lambda: c.links[1].pushed == c.links[1].inflight is None or c.links[1].pushed)
    await asyncio.sleep(0.2)
    await settle(hass)
    assert len(hub.pages()) == len(c.compiled.pages_v2)
