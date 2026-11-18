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
