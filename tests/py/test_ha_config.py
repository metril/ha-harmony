"""YAML config as source of truth, against the real `hub --sim` and `remote_sim`."""

import asyncio
import json
import logging
import subprocess

import aiohttp
import pytest
from conftest import HUB_ID, free_port, make_entry, push_config, rf_attach
from homeassistant.config_entries import ConfigEntryState
from homeassistant.exceptions import HomeAssistantError, ServiceValidationError
from homeassistant.helpers import device_registry as dr
from homeassistant.helpers import entity_registry as er
from homeassistant.helpers import issue_registry as ir
from test_activity_e2e import ir_line, nec
from test_hub_engine import read_lines

import custom_components.ha_harmony  # noqa: F401  (before the HA plugin shadows it)
from custom_components.ha_harmony import const
from custom_components.ha_harmony.coordinator import tile_text
from custom_components.ha_harmony.harmony_proto import MsgType
from proto import rf_codec

pytestmark = pytest.mark.usefixtures("enable_custom_integrations", "fast_client")

BASE = """
devices:
  - id: tv
    name: TV
    settle_ms: 40
    power: {on: {proto: nec, addr: 4, cmd: 1}, off: {proto: nec, addr: 4, cmd: 2}}
    inputs: {hdmi1: {proto: nec, addr: 4, cmd: 10}}
    commands: {vol_up: {proto: nec, addr: 4, cmd: 16}}
  - id: amp
    name: Amp
    settle_ms: 20
    power: {toggle: {proto: nec, addr: 9, cmd: 1}}
    commands: {mute: {proto: nec, addr: 9, cmd: 7}}
activities:
  - {id: watch, name: Télé, devices: [{device: tv, input: hdmi1}]}
  - {id: music, name: Music, devices: [{device: amp}]}
keys:
  vol_up: {device: tv, command: vol_up}
pages:
  - id: main
    cols: 4
    rows: 6
    widgets:
      - {type: label, x: 0, y: 0, w: 4, label: Room}
      - {type: button, id: mute, x: 0, y: 1, w: 2, label: Mute, action: {device: amp, command: mute}}
      - {type: tile, id: temp, x: 0, y: 2, w: 4, h: 2, label: Temp, entity: sensor.temp}
"""
BASE_UIDS = {
    f"{HUB_ID}_{k}"
    for k in [
        "remote",
        "activity",
        "key_1",
        "battery_1",
        "voltage_1",
        "dev_tv_power",
        "dev_tv_input",
        "dev_amp_power",
        "dev_amp_input",
        "cmd_tv:vol_up",
        "cmd_tv:power_on",
        "cmd_tv:power_off",
        "cmd_amp:mute",
        "cmd_amp:power_on",
        "cmd_amp:power_off",
    ]
}
SELECT = f"select.harmony_hub_{HUB_ID.replace('-', '_')}_activity"


@pytest.fixture
def write_yaml(hass, tmp_path):
    """write_yaml(text) -> path of <config dir>/ha_harmony/sim-hub.yaml (per-test dir)."""
    hass.config.config_dir = str(tmp_path / "cfg")
    path = tmp_path / "cfg" / "ha_harmony" / f"{HUB_ID}.yaml"
    path.parent.mkdir(parents=True)

    def _write(text: str):
        path.write_text(text, encoding="utf-8")
        return path

    return _write


@pytest.fixture(autouse=True)
def _fast_tiles(monkeypatch):
    monkeypatch.setattr(const, "TILE_DEBOUNCE", 0.05)


async def until(hass, predicate, timeout=5.0):
    async with asyncio.timeout(timeout):
        while not predicate():
            await asyncio.sleep(0.02)
            await hass.async_block_till_done()


async def setup(hass, hub, ok=True):
    entry = make_entry("127.0.0.1", hub.port)
    entry.add_to_hass(hass)
    assert await hass.config_entries.async_setup(entry.entry_id) is ok
    await hass.async_block_till_done()
    return entry


def eid(hass, domain, key):
    return er.async_get(hass).async_get_entity_id(domain, "ha_harmony", f"{HUB_ID}_{key}")


def val(hass, domain, key):
    return hass.states.get(eid(hass, domain, key)).state


def uids(hass, entry):
    return {
        e.unique_id for e in er.async_entries_for_config_entry(er.async_get(hass), entry.entry_id)
    }


async def hub_hash(url):
    async with aiohttp.ClientSession() as s, s.ws_connect(url) as ws:
        await ws.send_json({"v": 1, "id": 1, "t": "hello", "d": {"token": "test-token"}})
        return (await ws.receive_json())["d"]["config_hash"]


class Observer:
    """A second ws client that records the activity_list broadcasts the hub sends it."""

    def __init__(self, url):
        self.url, self.lists = url, []

    async def __aenter__(self):
        self.s = aiohttp.ClientSession()
        self.ws = await self.s.ws_connect(self.url)
        await self.ws.send_json({"v": 1, "id": 1, "t": "hello", "d": {"token": "test-token"}})
        self.task = asyncio.create_task(self._pump())
        return self

    async def _pump(self):
        async for msg in self.ws:
            env = json.loads(msg.data)
            if env["t"] == "activity_list":
                self.lists.append(env["d"])

    async def __aexit__(self, *exc):
        self.task.cancel()
        await self.ws.close()
        await self.s.close()


# ------------------------------------------------------------------ config push + hash
async def test_pushed_on_first_connect_not_when_unchanged(hass, hub_rf, write_yaml):
    write_yaml(BASE)
    async with Observer(hub_rf.url) as obs:
        entry = await setup(hass, hub_rf)
        coord = entry.runtime_data
        assert coord.config_pushes == 1 and coord.last_error is None
        await until(hass, lambda: len(obs.lists) == 1)
        assert obs.lists[0]["activities"] == ["Télé", "Music"]
        # the hash we compute (incl. non-ASCII) is exactly what the hub reports for what it holds
        assert (
            await hub_hash(hub_rf.url) == coord.compiled.config_hash == obs.lists[0]["config_hash"]
        )
        assert hass.states.get(SELECT).attributes["options"] == ["Télé", "Music", "Off"]
        await hass.config_entries.async_unload(entry.entry_id)

        entry2 = await setup(hass, hub_rf)  # same hub, same YAML: nothing to push
        assert entry2.runtime_data.config_pushes == 0
        await entry2.runtime_data.async_reload_config()
        assert entry2.runtime_data.config_pushes == 0
        await asyncio.sleep(0.2)
        assert len(obs.lists) == 1
        await hass.config_entries.async_unload(entry2.entry_id)


async def test_entities_for_worked_example(hass, hub_rf, write_yaml):
    write_yaml(BASE)
    entry = await setup(hass, hub_rf)
    assert uids(hass, entry) == BASE_UIDS
    reg = er.async_get(hass)
    assert reg.async_get("button.tv_vol_up") and reg.async_get("binary_sensor.tv_power")
    assert reg.async_get("sensor.tv_input") and reg.async_get("sensor.remote_1_battery")
    assert reg.async_get("sensor.remote_1_battery").original_device_class == "battery"
    assert reg.async_get("sensor.remote_1_voltage").entity_category.value == "diagnostic"
    assert {"vol_up", "tap_mute", "tap_temp"} <= set(
        hass.states.get("event.harmony_hub_sim_hub_remote_1_keys").attributes["event_types"]
    )
    assert dr.async_get(hass).async_get_device_by_identifier(
        (const.DOMAIN, f"{HUB_ID}_dev_tv"), entry.entry_id
    )
    await hass.config_entries.async_unload(entry.entry_id)


async def test_reload_refreshes_options_and_removes_stale_entities(hass, hub_rf, write_yaml):
    path = write_yaml(BASE)
    entry = await setup(hass, hub_rf)
    coord = entry.runtime_data
    ws_before = coord.client._ws
    new = (
        BASE.replace("name: Music", "name: Music2")
        .replace("id: amp", "id: zzz")
        .replace("device: amp", "device: zzz")
        .replace("mute: {", "mute2: {")
        .replace("command: mute}", "command: mute2}")
    )
    path.write_text(new, encoding="utf-8")
    await hass.services.async_call(const.DOMAIN, "reload_config", {}, blocking=True)
    await hass.async_block_till_done()
    assert coord.client._ws is ws_before and coord.client.connected  # no reconnect
    assert hass.states.get(SELECT).attributes["options"] == ["Télé", "Music2", "Off"]
    got = uids(hass, entry)
    assert f"{HUB_ID}_cmd_amp:mute" not in got and f"{HUB_ID}_dev_amp_power" not in got
    assert {f"{HUB_ID}_cmd_zzz:mute2", f"{HUB_ID}_dev_zzz_input"} <= got
    assert got == coord.expected_unique_ids()
    assert eid(hass, "button", "cmd_amp:mute") is None
    # a second identical reload changes nothing and pushes nothing
    pushes = coord.config_pushes
    await hass.services.async_call(const.DOMAIN, "reload_config", {}, blocking=True)
    assert coord.config_pushes == pushes and uids(hass, entry) == got
    await hass.config_entries.async_unload(entry.entry_id)


async def test_reload_updates_input_options_and_device_names(hass, hub_rf, write_yaml):
    path = write_yaml(BASE)
    entry = await setup(hass, hub_rf)
    coord = entry.runtime_data
    assert hass.states.get(eid(hass, "sensor", "dev_tv_input")).attributes["options"] == ["hdmi1"]
    new = BASE.replace("inputs: {hdmi1:", "inputs: {hdmi2: {proto: nec, addr: 4, cmd: 11}, hdmi1:")
    new = new.replace("name: TV", "name: Television")
    path.write_text(new, encoding="utf-8")
    await hass.services.async_call(const.DOMAIN, "reload_config", {}, blocking=True)
    await hass.async_block_till_done()
    ent = hass.states.get(eid(hass, "sensor", "dev_tv_input"))
    assert ent.attributes["options"] == ["hdmi2", "hdmi1"]
    await coord.async_device_command("tv", "input_hdmi2")
    await until(hass, lambda: hass.states.get(ent.entity_id).state == "hdmi2")
    dev = dr.async_get(hass).async_get_device_by_identifier(
        (const.DOMAIN, f"{HUB_ID}_dev_tv"), entry.entry_id
    )
    assert dev.name == "Television"
    await hass.config_entries.async_unload(entry.entry_id)


async def _running_activity_survives(
    hass, hub_rf, write_yaml, edit, options, running, start="Télé", power="dev_tv_power"
):
    path = write_yaml(BASE)
    entry = await setup(hass, hub_rf)
    coord = entry.runtime_data
    await coord.async_start_activity(start)
    await until(hass, lambda: hass.states.get(SELECT).state == start)
    path.write_text(edit(BASE), encoding="utf-8")
    await hass.services.async_call(const.DOMAIN, "reload_config", {}, blocking=True)
    # the hub's state_update after the reload: wait for the options, then for the state to settle
    await until(hass, lambda: hass.states.get(SELECT).attributes["options"] == options)
    await until(hass, lambda: hass.states.get(SELECT).state == running)
    assert hass.states.get(SELECT).attributes["options"] == options
    remote = hass.states.get("remote.harmony_hub_sim_hub")
    assert remote.state == "on" and remote.attributes["current_activity"] == running
    assert val(hass, "binary_sensor", power) == "on"  # the hub still runs it
    await hass.config_entries.async_unload(entry.entry_id)


async def test_rename_running_activity_keeps_it_current(hass, hub_rf, write_yaml):
    await _running_activity_survives(
        hass,
        hub_rf,
        write_yaml,
        lambda t: t.replace("name: Télé", "name: Cinema"),
        ["Cinema", "Music", "Off"],
        "Cinema",
    )


async def test_reorder_and_rename_running_activity_keeps_it_current(hass, hub_rf, write_yaml):
    def edit(t):
        a = "  - {id: watch, name: Télé, devices: [{device: tv, input: hdmi1}]}\n"
        b = "  - {id: music, name: Music, devices: [{device: amp}]}\n"
        assert a + b in t
        return t.replace(a + b, b + a.replace("Télé", "Cinema"))

    await _running_activity_survives(
        hass, hub_rf, write_yaml, edit, ["Music", "Cinema", "Off"], "Cinema"
    )


async def test_state_update_before_activity_list_is_kept(hass, hub_rf, write_yaml):
    """The hub's re-report may beat the new activity list: the index is kept, not dropped."""
    write_yaml(BASE)
    entry = await setup(hass, hub_rf)
    coord = entry.runtime_data
    old = list(coord.activities)
    coord._on_push(
        MsgType.STATE_UPDATE,
        {"activity": len(old) + 1, "power": True},
    )
    assert coord.current_activity is None  # unknown yet, never a wrong name
    coord._set_activities([*old, "X", "Y"])
    assert coord.current_activity == "Y"
    await hass.config_entries.async_unload(entry.entry_id)


_EXTRA = "  - {id: extra, name: Extra, devices: [{device: tv}]}\n"
_EXTRA2 = "  - {id: extra2, name: Extra2, devices: [{device: tv}]}\n"
_MUSIC = "  - {id: music, name: Music, devices: [{device: amp}]}\n"


# two inserted: the re-reported index (3) is out of range of the OLD list (Télé, Music, Off)
async def test_insert_activities_before_running_one_keeps_it_current(hass, hub_rf, write_yaml):
    await _running_activity_survives(
        hass,
        hub_rf,
        write_yaml,
        lambda t: t.replace(_MUSIC, _EXTRA + _EXTRA2 + _MUSIC),
        ["Télé", "Extra", "Extra2", "Music", "Off"],
        "Music",
        start="Music",
        power="dev_amp_power",
    )


async def test_remove_activity_before_running_one_keeps_it_current(hass, hub_rf, write_yaml):
    gone = "  - {id: watch, name: Télé, devices: [{device: tv, input: hdmi1}]}\n"
    await _running_activity_survives(
        hass,
        hub_rf,
        write_yaml,
        lambda t: t.replace(gone, ""),
        ["Music", "Off"],
        "Music",
        start="Music",
        power="dev_amp_power",
    )


async def test_external_config_change_refreshes_select(hass, hub_rf, write_yaml):
    """activity_list pushed by the hub after someone else's set_config (the stale-options bug)."""
    assert write_yaml  # sets the per-test config dir; no file = unmanaged, hub config is shown
    await push_config(hub_rf.url)
    entry = await setup(hass, hub_rf)
    assert hass.states.get(SELECT).attributes["options"] == ["TV", "Music", "Off"]
    other = {"activities": [{"name": "Game", "steps": [{"type": "none"}]}], "keys": {}}
    await push_config(hub_rf.url, config=other)
    await until(hass, lambda: hass.states.get(SELECT).attributes["options"] == ["Game", "Off"])
    await hass.config_entries.async_unload(entry.entry_id)


# ------------------------------------------------------------------ activities, keys, pages
async def test_activity_start_sends_expected_ir_and_updates_devices(hass, hub_rf, write_yaml):
    write_yaml(BASE)
    entry = await setup(hass, hub_rf)
    await hass.services.async_call(
        "select", "select_option", {"entity_id": SELECT, "option": "Télé"}, blocking=True
    )
    assert read_lines(hub_rf, 2) == [ir_line(nec(4, 1)), ir_line(nec(4, 10))]
    await until(hass, lambda: val(hass, "sensor", "dev_tv_input") == "hdmi1")
    assert val(hass, "binary_sensor", "dev_tv_power") == "on"
    assert val(hass, "binary_sensor", "dev_amp_power") == "off"
    assert val(hass, "select", "activity") == "Télé"
    await hass.config_entries.async_unload(entry.entry_id)


async def test_key_press_fires_symbolic_event(hass, hub_rf, write_yaml, run_remote):
    write_yaml(BASE)
    entry = await setup(hass, hub_rf)
    assert run_remote(hub_rf.rf, "--press", str(const.KEY_CODES["vol_up"])).returncode == 0
    ev = "event.harmony_hub_sim_hub_remote_1_keys"
    await until(hass, lambda: hass.states.get(ev).attributes.get("event_type") == "vol_up")
    assert read_lines(hub_rf, 1) == [ir_line(nec(4, 16))]  # the key map ran on the hub
    await hass.config_entries.async_unload(entry.entry_id)


def start_remote(built_binaries, rf, *args):
    p = subprocess.Popen(
        [str(built_binaries / "remote_sim"), "--rf", rf, "--timeout-ms", "8000", *args],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    from conftest import RemoteDump

    r = RemoteDump(p)
    r.wait_attached()
    return r


async def test_page_tap_runs_button_action(hass, hub_rf, write_yaml, built_binaries):
    write_yaml(BASE)
    r = start_remote(built_binaries, hub_rf.rf, "--tap", "30,70")  # the Mute button
    entry = await setup(hass, hub_rf)
    code, out = await asyncio.to_thread(r.result)
    assert code == 0 and json.loads(out) == {"hit": True, "id": 1, "key": 129}
    assert read_lines(hub_rf, 1) == [ir_line(nec(9, 7))]
    ev = "event.harmony_hub_sim_hub_remote_1_keys"
    await until(hass, lambda: hass.states.get(ev).attributes.get("event_type") == "tap_mute")
    await hass.config_entries.async_unload(entry.entry_id)


async def test_tile_update_follows_bound_entity(hass, hub_rf, write_yaml, built_binaries):
    write_yaml(BASE)
    r = start_remote(built_binaries, hub_rf.rf, "--dump-layout", "--settle-ms", "800")
    entry = await setup(hass, hub_rf)
    coord = entry.runtime_data
    base = coord.tile_updates
    hass.states.async_set("sensor.temp", "21.5", {"unit_of_measurement": "°C"})
    await until(hass, lambda: coord.tile_updates > base)
    hass.states.async_set("sensor.temp", "22.0", {"unit_of_measurement": "°C"})
    hass.states.async_set("sensor.temp", "22.5", {"unit_of_measurement": "°C"})  # debounced
    await until(hass, lambda: coord.tile_updates == base + 2)
    code, out = await asyncio.to_thread(r.result)
    tile = json.loads(out)["widgets"][2]
    assert code == 0 and tile["kind"] == "tile" and tile["text"] == "22.5 °C"
    await hass.config_entries.async_unload(entry.entry_id)


async def test_remote_attach_pushes_pages_and_tiles_immediately(
    hass, hub_rf, write_yaml, built_binaries
):
    write_yaml(BASE)
    hass.states.async_set("sensor.temp", "21.5", {"unit_of_measurement": "°C"})
    entry = await setup(hass, hub_rf)  # no remote yet: pages could not be delivered
    r = start_remote(built_binaries, hub_rf.rf, "--dump-layout", "--settle-ms", "800")
    # no key or battery event is sent: the attach itself triggers the push
    code, out = await asyncio.to_thread(r.result)
    layout = json.loads(out)
    assert code == 0 and [w["kind"] for w in layout["widgets"]] == ["label", "button", "tile"]
    assert layout["widgets"][2]["text"] == "21.5 °C"
    await hass.config_entries.async_unload(entry.entry_id)


async def test_config_hash_is_what_the_hub_acks(hass, hub_rf, write_yaml):
    write_yaml(BASE)
    entry = await setup(hass, hub_rf)
    coord = entry.runtime_data
    assert coord.client.config_hash == coord.compiled.config_hash == await hub_hash(hub_rf.url)
    real = coord.client.async_send

    async def fake(msg_type, data):
        res = await real(msg_type, data)
        if "config" in data:
            res = {**res, "config_hash": "deadbeef"}
        return res

    coord.client.async_send = fake
    coord.client.config_hash = None
    await coord.async_sync_config()
    assert coord.client.config_hash == "deadbeef"  # stored as reported, not assumed
    await hass.config_entries.async_unload(entry.entry_id)


def test_tile_text_truncates_on_utf8_boundary():
    class S:
        def __init__(self, state, unit=None):
            self.state, self.attributes = state, {"unit_of_measurement": unit} if unit else {}

    assert tile_text(None) == "" and tile_text(S("unavailable")) == "--"
    assert tile_text(S("21", "°C")) == "21 °C"
    text = tile_text(S("é" * 40))
    assert len(text.encode()) <= const.TILE_TEXT_MAX and text == "é" * 15


# ------------------------------------------------------------------ battery, device commands
async def test_battery_entities(hass, hub_rf, write_yaml):
    write_yaml(BASE)
    entry = await setup(hass, hub_rf)
    assert val(hass, "sensor", "battery_1") == "unknown"
    sock = rf_attach(hub_rf.rf)
    for fr in rf_codec.split(13, 2, bytes([1, 77, 0x0E, 0xD8]), 32):
        sock.send(fr)
    await until(hass, lambda: val(hass, "sensor", "battery_1") == "77")
    assert val(hass, "sensor", "voltage_1") == "3800"
    sock.close()
    await hass.config_entries.async_unload(entry.entry_id)


async def test_device_commands_buttons_and_send_command(hass, hub_rf, write_yaml):
    write_yaml(BASE)
    entry = await setup(hass, hub_rf)
    await hass.services.async_call(
        "button", "press", {"entity_id": "button.tv_vol_up"}, blocking=True
    )
    assert read_lines(hub_rf, 1) == [ir_line(nec(4, 16))]
    remote = "remote.harmony_hub_sim_hub"
    await hass.services.async_call(
        "remote", "send_command", {"entity_id": remote, "device": "tv", "command": ["power_on"]},
        blocking=True,
    )  # fmt: skip
    assert read_lines(hub_rf, 1) == [ir_line(nec(4, 1))]
    await until(hass, lambda: val(hass, "binary_sensor", "dev_tv_power") == "on")
    await hass.services.async_call(  # toggle device: tracked state flips
        "button", "press", {"entity_id": "button.amp_power_on"}, blocking=True
    )
    await until(hass, lambda: val(hass, "binary_sensor", "dev_amp_power") == "on")
    with pytest.raises(HomeAssistantError, match="unknown command"):
        await hass.services.async_call(
            "remote", "send_command", {"entity_id": remote, "device": "tv", "command": ["nope"]},
            blocking=True,
        )  # fmt: skip
    await hass.config_entries.async_unload(entry.entry_id)


async def test_toggle_device_power_buttons_keep_direction(hass, hub_rf, write_yaml):
    write_yaml(BASE)
    entry = await setup(hass, hub_rf)
    press = hass.services.async_call
    # tracked off: "Power on" sends the toggle once and records on
    await press("button", "press", {"entity_id": "button.amp_power_on"}, blocking=True)
    assert read_lines(hub_rf, 1) == [ir_line(nec(9, 1))]
    await until(hass, lambda: val(hass, "binary_sensor", "dev_amp_power") == "on")
    # tracked on: "Power on" sends no IR and the state stays on
    await press("button", "press", {"entity_id": "button.amp_power_on"}, blocking=True)
    await asyncio.sleep(0.2)
    assert read_lines(hub_rf, 0) == []
    assert val(hass, "binary_sensor", "dev_amp_power") == "on"
    await press("button", "press", {"entity_id": "button.amp_power_off"}, blocking=True)
    assert read_lines(hub_rf, 1) == [ir_line(nec(9, 1))]
    await until(hass, lambda: val(hass, "binary_sensor", "dev_amp_power") == "off")
    await hass.config_entries.async_unload(entry.entry_id)


async def test_command_unique_ids_do_not_collide(hass, hub_rf, write_yaml):
    code = "{proto: nec, addr: 1, cmd: 1}"
    write_yaml(
        f"devices:\n  - {{id: tv, commands: {{a_b: {code}}}}}\n"
        f"  - {{id: tv_a, commands: {{b: {code}}}}}\n"
    )
    entry = await setup(hass, hub_rf)
    ereg = er.async_get(hass)
    ids = [e.unique_id for e in er.async_entries_for_config_entry(ereg, entry.entry_id)]
    cmd = [u for u in ids if "_cmd_" in u]
    assert len(cmd) == 2 and len(set(cmd)) == 2
    await hass.config_entries.async_unload(entry.entry_id)


# ------------------------------------------------------------------ services
async def test_services(hass, hub_rf, write_yaml):
    write_yaml(BASE)
    entry = await setup(hass, hub_rf)
    call = hass.services.async_call
    await call(
        const.DOMAIN, "send_ir", {"ir": {"proto": "nec", "addr": 4, "cmd": 99}}, blocking=True
    )
    assert read_lines(hub_rf, 1) == [ir_line(nec(4, 99))]
    with pytest.raises(ServiceValidationError, match="Invalid IR action"):
        await call(
            const.DOMAIN, "send_ir", {"ir": {"proto": "nec", "addr": 4, "cmd": 999}}, blocking=True
        )
    await call(
        const.DOMAIN,
        "set_device_state",
        {"device": "tv", "power": True, "input": "hdmi1"},
        blocking=True,
    )
    await until(hass, lambda: val(hass, "sensor", "dev_tv_input") == "hdmi1")
    assert val(hass, "binary_sensor", "dev_tv_power") == "on"
    assert hub_rf.out_path.read_bytes()[hub_rf.out_pos :] == b""  # no IR for a manual state
    await call(const.DOMAIN, "set_device_state", {"device": "tv", "power": False}, blocking=True)
    await until(hass, lambda: val(hass, "binary_sensor", "dev_tv_power") == "off")
    with pytest.raises(ServiceValidationError):
        await call(const.DOMAIN, "set_device_state", {"device": "tv"}, blocking=True)
    with pytest.raises(HomeAssistantError, match="unknown device"):
        await call(
            const.DOMAIN, "set_device_state", {"device": "nope", "power": True}, blocking=True
        )
    with pytest.raises(ServiceValidationError):
        await call(const.DOMAIN, "push_pages", {"config_entry": "missing"}, blocking=True)
    await call(const.DOMAIN, "push_pages", {}, blocking=True)  # no remote attached: no error
    assert entry.runtime_data._pages_pending
    await hass.config_entries.async_unload(entry.entry_id)


async def test_reload_service_rejects_bad_yaml_and_keeps_previous(hass, hub_rf, write_yaml):
    path = write_yaml(BASE)
    entry = await setup(hass, hub_rf)
    coord = entry.runtime_data
    path.write_text(BASE.replace("hdmi1: {proto: nec, addr: 4, cmd: 10}", "hdmi1: {proto: nec}"))
    with pytest.raises(HomeAssistantError, match=r"sim-hub\.yaml:7: devices\[0\]\.inputs\.hdmi1"):
        await hass.services.async_call(const.DOMAIN, "reload_config", {}, blocking=True)
    assert ir.async_get(hass).async_get_issue(const.DOMAIN, f"config_invalid_{HUB_ID}")
    assert uids(hass, entry) == BASE_UIDS  # previous config still in force
    path.write_text(BASE)
    await hass.services.async_call(const.DOMAIN, "reload_config", {}, blocking=True)
    assert ir.async_get(hass).async_get_issue(const.DOMAIN, f"config_invalid_{HUB_ID}") is None
    assert coord.last_error is None
    await hass.config_entries.async_unload(entry.entry_id)


# ------------------------------------------------------------------ failure modes
async def test_invalid_yaml_at_startup_falls_back_to_last_good_config(hass, hub_rf, write_yaml):
    path = write_yaml(BASE)
    entry = await setup(hass, hub_rf)
    assert uids(hass, entry) == BASE_UIDS
    await hass.config_entries.async_unload(entry.entry_id)
    await hass.async_block_till_done()
    path.write_text("devices: [oops", encoding="utf-8")  # HA restarts with a broken file
    assert await hass.config_entries.async_setup(entry.entry_id)
    await hass.async_block_till_done()
    coord = entry.runtime_data
    assert coord.managed and coord.last_error and "sim-hub.yaml" in coord.last_error
    assert ir.async_get(hass).async_get_issue(const.DOMAIN, f"config_invalid_{HUB_ID}")  # kept
    assert uids(hass, entry) == BASE_UIDS and coord.config_pushes == 0  # hub keeps its config
    assert hass.states.get(SELECT).attributes["options"] == ["Télé", "Music", "Off"]
    assert hass.states.get("button.tv_vol_up").state != "unavailable"
    await hass.config_entries.async_unload(entry.entry_id)


async def test_missing_yaml_loads_with_empty_config(hass, hub_rf, write_yaml, caplog):
    caplog.set_level(logging.INFO)
    await push_config(hub_rf.url)
    entry = await setup(hass, hub_rf)  # write_yaml fixture made the dir but no file
    coord = entry.runtime_data
    assert entry.state is ConfigEntryState.LOADED and not coord.managed
    assert coord.config_pushes == 0 and "No config file at" in caplog.text
    assert hass.states.get(SELECT).attributes["options"] == ["TV", "Music", "Off"]
    assert uids(hass, entry) == {
        f"{HUB_ID}_{k}" for k in ["remote", "activity", "key_1", "battery_1", "voltage_1"]
    }
    await hass.config_entries.async_unload(entry.entry_id)


async def test_oversize_config_is_a_clean_error(hass, hub_rf, write_yaml):
    write_yaml("devices:\n" + "".join(f"  - {{id: d{i}}}\n" for i in range(17)))
    entry = await setup(hass, hub_rf)
    coord = entry.runtime_data
    assert entry.state is ConfigEntryState.LOADED
    assert "too many devices: 17" in coord.last_error and coord.config_pushes == 0
    issue = ir.async_get(hass).async_get_issue(const.DOMAIN, f"config_invalid_{HUB_ID}")
    assert issue and issue.translation_key == "config_invalid"
    await hass.config_entries.async_unload(entry.entry_id)


async def test_hub_rejection_raises_repair_issue(hass, fake_hub, write_yaml, caplog):
    write_yaml(BASE)

    async def handler(ws, env):
        t = env["t"]
        reply = {"v": 1, "id": env["id"]}
        if t == "hello":
            await ws.send_json(
                {**reply, "t": "hello_ack", "d": {"hub_id": HUB_ID, "activities": ["A"]}}
            )
            await ws.send_json(
                {"v": 1, "id": 0, "t": "state_update", "d": {"activity": 7, "power": True}}
            )
        elif t == "set_config":
            await ws.send_json(
                {**reply, "t": "config_ack", "d": {"ok": False, "message": "bad thing"}}
            )

    hub = await fake_hub(handler)
    entry = await setup(hass, hub)
    assert entry.state is ConfigEntryState.LOADED  # not a crash
    assert entry.runtime_data.last_error == "bad thing"
    assert ir.async_get(hass).async_get_issue(const.DOMAIN, f"config_rejected_{HUB_ID}")
    assert "rejected the config: bad thing" in caplog.text
    assert "hub reports activity index 7" in caplog.text  # not silently dropped
    assert hass.states.get(SELECT).state == "Off"
    await hass.config_entries.async_unload(entry.entry_id)


@pytest.mark.parametrize("keep_cache", [True, False])
async def test_hub_restart_recovery(hass, start_hub, write_yaml, tmp_path, keep_cache):
    write_yaml(BASE)
    port, cache = free_port(), str(tmp_path / "cache.bin")
    hub = start_hub("--cache", cache, port=port)
    entry = await setup(hass, hub)
    coord = entry.runtime_data
    assert coord.config_pushes == 1
    before = uids(hass, entry)
    hub.stop()
    await until(hass, lambda: hass.states.get(SELECT).state == "unavailable")
    if not keep_cache:
        (tmp_path / "cache.bin").unlink()
    start_hub("--cache", cache, port=port)
    await until(hass, lambda: hass.states.get(SELECT).state == "Off")
    await until(hass, lambda: coord.config_pushes == (1 if keep_cache else 2))
    await asyncio.sleep(0.1)
    assert coord.config_pushes == (1 if keep_cache else 2)  # pushed again only when the hub lost it
    assert hass.states.get(SELECT).attributes["options"] == ["Télé", "Music", "Off"]
    assert uids(hass, entry) == before
    await hass.services.async_call(
        "select", "select_option", {"entity_id": SELECT, "option": "Music"}, blocking=True
    )
    await until(hass, lambda: val(hass, "select", "activity") == "Music")
    await hass.config_entries.async_unload(entry.entry_id)
