"""fake add-on -> integration -> `hub --sim` -> `remote_sim` for page format v2 and HA actions."""

import asyncio
import json
import subprocess

import pytest
from conftest import HUB_ID, RemoteDump, make_entry, push_config
from pytest_homeassistant_custom_component.common import async_mock_service
from test_activity_e2e import ir_line, nec
from test_ha_runtime import YAML
from test_hub_engine import read_lines

import custom_components.ha_harmony  # noqa: F401  (before the HA plugin shadows it)
from custom_components.ha_harmony import const

pytestmark = pytest.mark.usefixtures("enable_custom_integrations", "fast_client")

SELECT = f"select.harmony_hub_{HUB_ID.replace('-', '_')}_activity"
DOWN = 2


@pytest.fixture(autouse=True)
def _fast_tiles(monkeypatch):
    monkeypatch.setattr(const, "TILE_DEBOUNCE", 0.05)


@pytest.fixture
def info_wait(monkeypatch):
    """A remote that announces itself (`--info`) is a v2 remote only if its remote_info beats the
    integration's wait; the short fast_client wait is for remotes that never send one, and loses
    that race on a loaded machine. The wait ends as soon as the info arrives, so this costs nothing."""
    monkeypatch.setattr(const, "INFO_WAIT", 5.0)


async def until(hass, predicate, timeout=5.0):
    async with asyncio.timeout(timeout):
        while not predicate():
            await asyncio.sleep(0.02)
            await hass.async_block_till_done()


async def setup(hass, hub, fake_addon, yaml=YAML):
    fake_addon.apply_yaml(yaml)
    entry = make_entry("127.0.0.1", hub.port, addon=fake_addon)
    entry.add_to_hass(hass)
    assert await hass.config_entries.async_setup(entry.entry_id)
    if entry.runtime_data.first_sync is not None:  # setup does not wait for the hub sync
        await entry.runtime_data.first_sync
    await hass.async_block_till_done()
    return entry


def remote(built_binaries, rf, *args):
    p = subprocess.Popen(
        [str(built_binaries / "remote_sim"), "--rf", rf, "--timeout-ms", "8000", *args],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    r = RemoteDump(p)
    r.wait_attached()
    return r


async def test_v2_remote_gets_v2_pages_and_tile_state(
    hass, hub_rf, fake_addon, built_binaries, info_wait
):
    hass.states.async_set("light.a", "on")
    hass.states.async_set("sensor.t", "20.5", {"unit_of_measurement": "°C"})
    entry = await setup(hass, hub_rf, fake_addon)
    r = remote(built_binaries, hub_rf.rf, "--info", "3", "--dump-layout", "--settle-ms", "1000")
    code, out = await asyncio.to_thread(r.result)
    lay = json.loads(out)
    assert code == 0 and lay["version"] == 2
    tiles = {w["label"]: w for w in lay["widgets"] if w["kind"] == "tile"}
    assert tiles["Lamp"]["state"] == 2 and tiles["Lamp"]["text"] == "On"
    assert tiles["Temp"].get("state", 0) == 0 and tiles["Temp"]["text"] == "20.5°C"
    sensor = hass.states.get("sensor.remote_1_page_format")
    assert sensor.state == "v2" and sensor.attributes["caps"] == ["tile_state", "paging"]
    await hass.config_entries.async_unload(entry.entry_id)


async def test_remote_without_remote_info_gets_v1_pages_and_text_tiles(
    hass, hub_rf, fake_addon, built_binaries
):
    hass.states.async_set("light.a", "on")
    entry = await setup(hass, hub_rf, fake_addon)
    r = remote(built_binaries, hub_rf.rf, "--dump-layout", "--settle-ms", "1000")
    code, out = await asyncio.to_thread(r.result)
    lay = json.loads(out)
    assert code == 0 and lay.get("version", 1) == 1
    assert {w["label"]: w for w in lay["widgets"]}["Lamp"]["text"] == "on"  # legacy text
    assert hass.states.get("sensor.remote_1_page_format").state == "unknown"
    await hass.config_entries.async_unload(entry.entry_id)


async def test_held_repeat_action_fires_at_the_remotes_cadence(
    hass, hub_rf, fake_addon, run_remote
):
    entry = await setup(hass, hub_rf, fake_addon)
    sends = async_mock_service(hass, "remote", "send_command")
    hass.states.async_set("remote.shield", "on")
    res = await asyncio.to_thread(
        run_remote, hub_rf.rf, "--hold", f"{DOWN},700", "--hold-interval-ms", "100"
    )
    assert res.returncode == 0
    await until(hass, lambda: len(sends) >= 4)  # the press and the holds, none from the release
    assert {s.data["command"] for s in sends} == {"DPAD_DOWN"}
    n = len(sends)
    await asyncio.sleep(0.3)
    await hass.async_block_till_done()
    assert len(sends) == n  # released: no more repeats
    await hass.config_entries.async_unload(entry.entry_id)


async def test_activity_start_mixes_hub_ir_and_home_assistant_steps(hass, hub_rf, fake_addon):
    hass.states.async_set("media_player.shield", "off")
    hass.states.async_set("light.room", "on")
    entry = await setup(hass, hub_rf, fake_addon)
    on = async_mock_service(hass, "remote", "turn_on")
    sends = async_mock_service(hass, "remote", "send_command")
    lights = async_mock_service(hass, "light", "turn_off")
    await hass.services.async_call(
        "select", "select_option", {"entity_id": SELECT, "option": "Stream"}, blocking=True
    )
    assert read_lines(hub_rf, 2) == [ir_line(nec(4, 1)), ir_line(nec(4, 10))]  # the hub's IR part
    await until(hass, lambda: lights)  # the HA part, after the hub reported the activity running
    assert len(on) == 1 and [s.data["command"] for s in sends] == ["HOME"] and len(lights) == 1
    await hass.config_entries.async_unload(entry.entry_id)


async def test_hub_sends_a_command_repeats_times_and_ignores_it_when_absent(hub_rf, run_remote):
    cfg = {
        "format": 2,
        "keys": {
            "8": {"type": "ir", "proto": "nec", "addr": 4, "cmd": 8, "repeats": 3},
            "9": {"type": "ir", "proto": "nec", "addr": 4, "cmd": 9},
        },
    }
    await push_config(hub_rf.url, config=cfg)
    assert (await asyncio.to_thread(run_remote, hub_rf.rf, "--press", "VolumeUp")).returncode == 0
    assert read_lines(hub_rf, 3) == [ir_line(nec(4, 8))] * 3  # three full frames
    assert (await asyncio.to_thread(run_remote, hub_rf.rf, "--press", "VolumeDown")).returncode == 0
    assert read_lines(hub_rf, 1) == [ir_line(nec(4, 9))]  # no repeats key: one frame


async def _compiled_set():
    from test_ha_audit import _held

    held = _held()
    return held["gen"], held["pages"]


async def test_remote_that_holds_the_set_is_sent_no_pages(
    hass, hub_rf, fake_addon, built_binaries, info_wait
):
    gen, pages = await _compiled_set()
    entry = await setup(hass, hub_rf, fake_addon)
    r = remote(
        built_binaries, hub_rf.rf, "--info", "3", "--stored", f"{gen},{pages}",
        "--dump-page", "--timeout-ms", "2500",
    )  # fmt: skip
    code, _out = await asyncio.to_thread(r.result)
    assert code == 3  # remote_sim: timed out waiting for a page_push
    await hass.config_entries.async_unload(entry.entry_id)


async def test_remote_holding_another_set_gets_the_pages(
    hass, hub_rf, fake_addon, built_binaries, info_wait
):
    gen, pages = await _compiled_set()
    entry = await setup(hass, hub_rf, fake_addon)
    r = remote(
        built_binaries, hub_rf.rf, "--info", "3", "--stored", f"{(gen + 1) % 256},{pages}",
        "--dump-page", "--timeout-ms", "5000",
    )  # fmt: skip
    code, out = await asyncio.to_thread(r.result)
    assert code == 0 and json.loads(out)["data"]
    await hass.config_entries.async_unload(entry.entry_id)


async def test_editor_flow_config_runs_through_the_integration(
    hass, hub_rf, fake_addon, run_remote
):
    """The config the e2e editor flow wrote (fixtures/ha_action_flow.json): a hard key of the Stream
    layer runs `light.toggle`, and the tile dropped from the palette toggles on its soft key."""
    from pathlib import Path

    cfg = json.loads((Path(__file__).parent / "fixtures" / "ha_action_flow.json").read_text())
    hass.states.async_set("light.kitchen", "off", {"friendly_name": "Kitchen"})
    fake_addon.apply(cfg)
    entry = make_entry("127.0.0.1", hub_rf.port, addon=fake_addon)
    entry.add_to_hass(hass)
    assert await hass.config_entries.async_setup(entry.entry_id)
    if entry.runtime_data.first_sync is not None:
        await entry.runtime_data.first_sync
    await hass.async_block_till_done()
    toggles = async_mock_service(hass, "light", "toggle")

    # the tile (page stream_ctl, wire widget 3) -> soft key 128 + 3, "automatic" = toggle
    assert (await asyncio.to_thread(run_remote, hub_rf.rf, "--press", "131")).returncode == 0
    await until(hass, lambda: len(toggles) == 1)
    assert toggles[0].data == {"entity_id": ["light.kitchen"]}

    # the hard key only exists in the Stream layer: start it, then press ChannelUp
    await hass.services.async_call(
        "select", "select_option", {"entity_id": SELECT, "option": "Stream"}, blocking=True
    )
    await until(hass, lambda: entry.runtime_data.current_activity_id == "stream")
    assert (await asyncio.to_thread(run_remote, hub_rf.rf, "--press", "ChannelUp")).returncode == 0
    await until(hass, lambda: len(toggles) == 2)
    await hass.config_entries.async_unload(entry.entry_id)
