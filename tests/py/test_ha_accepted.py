"""Runtime tables follow the config the hub ACCEPTED, never one it may not run."""
# ruff: noqa: F811  (imported fixtures are used by name)

import pytest
from homeassistant.helpers import entity_registry as er
from pytest_homeassistant_custom_component.common import async_mock_service
from test_ha_runtime import YAML, clock, press, rig, settle  # noqa: F401  (fixtures)

pytestmark = pytest.mark.usefixtures("enable_custom_integrations", "fast_client")

STREAM_BLOCK_END = "keys:\n  select: {device: tv, command: power_on}"


def _reordered(yaml: str) -> str:
    """Stream first, TV second (the hub's activity indexes swap), and `up` calls another service."""
    tv_line = "  - {id: tv_on, name: TV, devices: [{device: tv, input: hdmi1}]}\n"
    assert tv_line in yaml
    out = yaml.replace(tv_line, "").replace(STREAM_BLOCK_END, tv_line + STREAM_BLOCK_END, 1)
    return out.replace(
        "up: {ha: {service: light.toggle, target: {entity_id: light.up}}}",
        "up: {ha: {service: light.turn_on, target: {entity_id: light.new}}}",
    )


async def _state(hub, idx):
    await hub.push("state_update", {"activity": idx, "power": True})


async def test_a_rejected_config_keeps_dispatching_by_the_accepted_tables(
    hass, rig, clock, fake_addon
):
    toggles = async_mock_service(hass, "light", "toggle")
    turn_ons = async_mock_service(hass, "light", "turn_on")
    hub, c = await rig()
    old = c.compiled
    assert c.config_state == "accepted"
    hub.reject_config = True
    fake_addon.apply_yaml(_reordered(YAML))
    with pytest.raises(Exception, match="rejected"):
        await c.async_refresh_from_addon()
    assert c.config_state == "rejected" and c.compiled is old and c.pending is not old
    assert c.hub_accepted is False
    await _state(hub, 1)  # the hub still runs the OLD list: index 1 is Stream
    await settle(hass, lambda: c.current_activity == "Stream")
    assert c.current_activity_id == "stream"
    await press(hub, 4 + 0, 0)
    await press(hub, 1, 0)  # up: old default layer (light.toggle light.up)
    await press(hub, 3, 0)  # left: Stream's own layer in the OLD table (light.toggle light.lamp)
    await settle(hass, lambda: len(toggles) == 2)
    assert [x.data["entity_id"] for x in toggles] == [["light.up"], ["light.lamp"]]
    assert turn_ons == []  # nothing of the new table fired


async def test_accepting_switches_all_tables_at_once(hass, rig, clock, fake_addon):
    turn_ons = async_mock_service(hass, "light", "turn_on")
    hub, c = await rig()
    hub.reject_config = True
    fake_addon.apply_yaml(_reordered(YAML))
    with pytest.raises(Exception, match="rejected"):
        await c.async_refresh_from_addon()
    hub.reject_config = False
    await c.async_sync_config()
    assert c.config_state == "accepted" and c.compiled is c.pending
    assert c.activities == ["Stream", "TV"]
    await _state(hub, 1)  # now index 1 is TV
    await settle(hass, lambda: c.current_activity == "TV")
    assert c.current_activity_id == "tv_on"
    await press(hub, 1, 0)
    await settle(hass, lambda: len(turn_ons) == 1)
    assert turn_ons[0].data["entity_id"] == ["light.new"]


async def test_restart_with_the_hubs_hash_is_accepted_without_a_push(hass, rig, clock):
    hub, c = await rig()
    entry = c.config_entry
    hub.got.clear()
    await hass.config_entries.async_reload(entry.entry_id)
    await settle(hass)
    c2 = entry.runtime_data
    assert c2 is not c and c2.config_state == "accepted"
    assert c2.compiled.devices and c2.compiled is c2.pending
    assert not any(t == "set_config" for t, _ in hub.got)


async def test_restart_with_a_different_hub_hash_waits_for_the_push(hass, rig, clock):
    hub, c = await rig()
    entry = c.config_entry
    hub.config_hash = "00000000"  # the hub runs something else (older firmware, other config)
    hub.got.clear()
    await hass.config_entries.async_reload(entry.entry_id)
    await settle(hass, lambda: any(t == "set_config" for t, _ in hub.got))
    c2 = entry.runtime_data
    await settle(
        hass, lambda: entry.runtime_data.config_state == "accepted"
    )  # the ack arrives over the socket
    c2 = entry.runtime_data
    assert c2.config_state == "accepted" and c2.compiled.devices  # promoted by the acked push
    assert er.async_get(hass)  # registry intact
