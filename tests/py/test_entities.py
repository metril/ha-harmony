"""Entities against the real sim hub: push updates, hub restart, token rotation, teardown."""

import asyncio

import pytest
from conftest import HUB_ID, free_port, make_entry, push_config
from homeassistant.config_entries import ConfigEntryState
from homeassistant.const import STATE_OFF, STATE_ON, STATE_UNAVAILABLE
from homeassistant.helpers import entity_registry as er
from homeassistant.setup import async_setup_component
from pytest_homeassistant_custom_component.common import async_fire_time_changed  # noqa: F401

import custom_components.ha_harmony  # noqa: F401  (before the HA plugin shadows it)

pytestmark = pytest.mark.usefixtures("enable_custom_integrations")

REMOTE = "remote.harmony_hub_sim_hub"
SELECT = "select.harmony_hub_sim_hub_activity"
EVENT = "event.harmony_hub_sim_hub_remote_1_keys"


async def _until(hass, predicate, timeout=5.0):
    async with asyncio.timeout(timeout):
        while not predicate():
            await asyncio.sleep(0.02)
            await hass.async_block_till_done()


def _state(hass, eid):
    st = hass.states.get(eid)
    return st.state if st else None


async def _setup(hass, hub, ok=True, **kw):
    entry = make_entry("127.0.0.1", hub.port, **kw)
    entry.add_to_hass(hass)
    assert await hass.config_entries.async_setup(entry.entry_id) is ok
    await hass.async_block_till_done()
    return entry


@pytest.fixture
async def hub_cfg(hub_rf, tmp_path):
    await push_config(hub_rf.url)
    return hub_rf


async def test_setup_creates_one_of_each(hass, hub_cfg, fast_client):
    entry = await _setup(hass, hub_cfg)
    assert entry.state is ConfigEntryState.LOADED
    reg = er.async_get(hass)
    ents = er.async_entries_for_config_entry(reg, entry.entry_id)
    assert sorted(e.domain for e in ents) == ["event", "remote", "select"]
    assert {e.unique_id for e in ents} == {
        f"{HUB_ID}_remote",
        f"{HUB_ID}_activity",
        f"{HUB_ID}_key_1",
    }
    st = hass.states.get(SELECT)
    assert st.attributes["options"] == ["TV", "Music", "Off"] and st.state == "Off"
    await hass.config_entries.async_unload(entry.entry_id)


async def test_state_update_push_flips_select_and_remote(hass, hub_cfg, fast_client):
    entry = await _setup(hass, hub_cfg)
    await hass.services.async_call(
        "select", "select_option", {"entity_id": SELECT, "option": "Music"}, blocking=True
    )
    await _until(hass, lambda: _state(hass, SELECT) == "Music")
    assert _state(hass, REMOTE) == STATE_ON
    assert hass.states.get(REMOTE).attributes["current_activity"] == "Music"
    await hass.services.async_call(
        "select", "select_option", {"entity_id": SELECT, "option": "Off"}, blocking=True
    )
    await _until(hass, lambda: _state(hass, SELECT) == "Off")
    assert _state(hass, REMOTE) == STATE_OFF
    await hass.config_entries.async_unload(entry.entry_id)


async def test_remote_services(hass, hub_cfg, fast_client):
    from homeassistant.exceptions import HomeAssistantError

    entry = await _setup(hass, hub_cfg)
    await hass.services.async_call(
        "remote", "turn_on", {"entity_id": REMOTE, "activity": "Music"}, blocking=True
    )
    await _until(hass, lambda: _state(hass, SELECT) == "Music")
    await hass.services.async_call("remote", "turn_off", {"entity_id": REMOTE}, blocking=True)
    await _until(hass, lambda: _state(hass, SELECT) == "Off")
    # default activity is the last one used
    await hass.services.async_call("remote", "turn_on", {"entity_id": REMOTE}, blocking=True)
    await _until(hass, lambda: _state(hass, SELECT) == "Music")
    await hass.services.async_call(
        "remote", "send_command",
        {"entity_id": REMOTE, "command": ["power_off"], "device": "hub"}, blocking=True,
    )  # fmt: skip
    await _until(hass, lambda: _state(hass, SELECT) == "Off")
    with pytest.raises(HomeAssistantError):
        await hass.services.async_call(
            "remote", "send_command", {"entity_id": REMOTE, "command": ["bogus"]}, blocking=True
        )
    await hass.config_entries.async_unload(entry.entry_id)


async def test_key_event_fires_event_entity(hass, hub_cfg, fast_client, run_remote):
    entry = await _setup(hass, hub_cfg)
    assert run_remote(hub_cfg.rf, "--press", "1", "--action", "2").returncode == 0
    await _until(hass, lambda: _state(hass, EVENT) not in (None, "unknown"))
    st = hass.states.get(EVENT)
    assert st.attributes["event_type"] == "menu"
    assert st.attributes["key"] == 1 and st.attributes["action"] == "hold"
    # unknown key code must not raise
    assert run_remote(hub_cfg.rf, "--press", "99").returncode == 0
    await _until(hass, lambda: hass.states.get(EVENT).attributes["event_type"] == "unknown")
    assert hass.states.get(EVENT).attributes["key"] == 99
    await hass.config_entries.async_unload(entry.entry_id)


async def test_hub_restart_recovers_without_duplicates(hass, start_hub, fast_client, tmp_path):
    port, cache = free_port(), str(tmp_path / "cache.bin")
    hub = start_hub("--cache", cache, port=port)
    await push_config(hub.url)
    entry = await _setup(hass, hub)
    reg = er.async_get(hass)
    before = {
        e.entity_id: e.unique_id for e in er.async_entries_for_config_entry(reg, entry.entry_id)
    }
    assert len(before) == 3

    await hass.services.async_call(
        "select", "select_option", {"entity_id": SELECT, "option": "TV"}, blocking=True
    )
    await _until(hass, lambda: _state(hass, SELECT) == "TV")
    hub.stop()
    await _until(hass, lambda: _state(hass, SELECT) == STATE_UNAVAILABLE)
    assert _state(hass, REMOTE) == STATE_UNAVAILABLE and _state(hass, EVENT) == STATE_UNAVAILABLE

    start_hub("--cache", cache, port=port)  # restart on the same port; config restored from cache
    await _until(hass, lambda: _state(hass, SELECT) not in (STATE_UNAVAILABLE, None))
    assert hass.states.get(SELECT).attributes["options"] == ["TV", "Music", "Off"]
    after = {
        e.entity_id: e.unique_id for e in er.async_entries_for_config_entry(reg, entry.entry_id)
    }
    assert after == before
    # the control path works again after the reconnect
    await hass.services.async_call(
        "select", "select_option", {"entity_id": SELECT, "option": "Music"}, blocking=True
    )
    await _until(hass, lambda: _state(hass, SELECT) == "Music")
    await hass.config_entries.async_unload(entry.entry_id)


async def test_token_rotation_starts_reauth(hass, start_hub, fast_client, tmp_path):
    port, cache = free_port(), str(tmp_path / "cache.bin")
    hub = start_hub("--cache", cache, port=port)
    entry = await _setup(hass, hub)
    hub.stop()
    await _until(hass, lambda: _state(hass, SELECT) == STATE_UNAVAILABLE)
    start_hub("--cache", cache, port=port, token="rotated")
    await _until(
        hass, lambda: any(hass.config_entries.flow.async_progress_by_handler("ha_harmony"))
    )
    (flow,) = hass.config_entries.flow.async_progress_by_handler("ha_harmony")
    assert flow["context"]["source"] == "reauth"
    assert _state(hass, SELECT) == STATE_UNAVAILABLE

    res = await hass.config_entries.flow.async_configure(flow["flow_id"], {"token": "wrong"})
    assert res["errors"] == {"base": "invalid_auth"}
    res = await hass.config_entries.flow.async_configure(flow["flow_id"], {"token": "rotated"})
    assert res["type"].value == "abort" and res["reason"] == "reauth_successful"
    assert entry.data["token"] == "rotated"
    await hass.async_block_till_done()
    await _until(hass, lambda: _state(hass, SELECT) not in (STATE_UNAVAILABLE, None))
    assert len(er.async_entries_for_config_entry(er.async_get(hass), entry.entry_id)) == 3
    await hass.config_entries.async_unload(entry.entry_id)


async def test_setup_with_bad_token_triggers_reauth(hass, hub_process):
    entry = await _setup(hass, hub_process, ok=False, token="bad")
    assert entry.state is ConfigEntryState.SETUP_ERROR
    assert any(
        f["context"]["source"] == "reauth"
        for f in hass.config_entries.flow.async_progress_by_handler("ha_harmony")
    )


async def test_setup_retries_when_hub_down(hass):
    entry = make_entry("127.0.0.1", free_port())
    entry.add_to_hass(hass)
    assert not await hass.config_entries.async_setup(entry.entry_id)
    assert entry.state is ConfigEntryState.SETUP_RETRY
    await hass.config_entries.async_unload(entry.entry_id)


async def test_version_skew_fails_setup_without_retry(hass, fake_hub, fast_client):
    async def handler(ws, env):
        await ws.send_json(
            {"v": 1, "id": env["id"], "t": "error", "d": {"code": "version", "message": "need v2"}}
        )

    hub = await fake_hub(handler)
    entry = await _setup(hass, hub, ok=False)
    assert entry.state is ConfigEntryState.SETUP_ERROR
    assert "incompatible protocol" in entry.reason
    await asyncio.sleep(0.4)  # several backoff periods: nobody reconnects
    assert hub.connections == 1


async def test_unload_closes_client_and_tasks(hass, hub_cfg, fast_client):
    entry = await _setup(hass, hub_cfg)
    client = entry.runtime_data.client
    assert client.connected
    assert await hass.config_entries.async_unload(entry.entry_id)
    assert entry.state is ConfigEntryState.NOT_LOADED
    assert not client.connected and client._task is None
    assert _state(hass, SELECT) == STATE_UNAVAILABLE
    assert await async_setup_component(hass, "homeassistant", {})  # hass still healthy
    assert await hass.config_entries.async_setup(entry.entry_id)  # re-setup: no duplicates
    await hass.async_block_till_done()
    reg = er.async_get(hass)
    assert len(er.async_entries_for_config_entry(reg, entry.entry_id)) == 3
    await hass.config_entries.async_unload(entry.entry_id)


async def test_runtime_version_mismatch_reloads_into_setup_error(hass, fake_hub, fast_client):
    async def handler(ws, env):
        if hub.connections == 1:
            await ws.send_json(
                {
                    "v": 1,
                    "id": env["id"],
                    "t": "hello_ack",
                    "d": {"hub_id": HUB_ID, "activities": []},
                }
            )
            await ws.send_json({"v": 2, "id": 0, "t": "state_update", "d": {}})
        else:
            await ws.send_json(
                {"v": 1, "id": env["id"], "t": "error", "d": {"code": "version", "message": "v2"}}
            )

    hub = await fake_hub(handler)
    entry = make_entry("127.0.0.1", hub.port)
    entry.add_to_hass(hass)
    # the bad push may land mid-setup; either way the entry must end in SETUP_ERROR
    await hass.config_entries.async_setup(entry.entry_id)
    await _until(hass, lambda: entry.state is ConfigEntryState.SETUP_ERROR)
    assert "incompatible protocol" in entry.reason
    leaked = [t for t in asyncio.all_tasks() if "ha_harmony" in t.get_name() and not t.done()]
    assert not leaked
    n = hub.connections
    await asyncio.sleep(0.4)
    assert hub.connections == n == 2  # initial + the reload's setup; no retry loop
