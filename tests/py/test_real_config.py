"""The real config generated from this user's own hub, through the real integration.

Every other HA test uses hand-written fixtures. This one loads the 85-command config that
`tools/stock_to_ha_config.py` produced from the stock IR database extracted off the rooted
hub, into a real HA instance, against the real `hub --sim`, and presses real buttons.

The file lives in the gitignored `backups/` tree (it is device data, not source), so the "real"
runs skip cleanly wherever it is absent -- CI included. Point HAH_REAL_CONFIG at another copy to
run it elsewhere. Every test also runs against fixtures/living_room.yaml, an invented config of
the same structure (5 devices, 148 commands, 3 activities, 14 pages), so CI exercises these
bodies too. Command buttons are created disabled by default, so the tests enable them.
"""

from __future__ import annotations

import glob
import os
from pathlib import Path

import pytest
import yaml
from conftest import HUB_ID, make_entry
from homeassistant.config_entries import ConfigEntryState
from homeassistant.helpers import device_registry as dr
from homeassistant.helpers import entity_registry as er
from test_hub_engine import read_lines

import custom_components.ha_harmony  # noqa: F401  (before the HA plugin shadows it)

REPO = Path(__file__).resolve().parents[2]


def _find_real_config() -> Path | None:
    if env := os.environ.get("HAH_REAL_CONFIG"):
        return Path(env) if Path(env).is_file() else None
    hits = sorted(
        glob.glob(str(REPO / "backups/phase1/hub/*/extracted/ha_harmony_config.yaml"))
    )
    return Path(hits[-1]) if hits else None


REAL = _find_real_config()
LIVING_ROOM = REPO / "tests/py/fixtures/living_room.yaml"
COMMANDS = {"living_room": 148, "real": 85}  # the number of device commands in each source
pytestmark = pytest.mark.usefixtures("enable_custom_integrations", "fast_client", "buttons_enabled")


@pytest.fixture
def buttons_enabled(monkeypatch):
    """New command buttons are created disabled (the default), and pressing a disabled entity is a no-op."""
    monkeypatch.setattr("custom_components.ha_harmony.button.ENABLED_BY_DEFAULT", True)


@pytest.fixture(params=["living_room", "real"])
def source(request) -> str:
    if request.param == "real" and REAL is None:
        pytest.skip("real hub config not present (gitignored backups/)")
    return request.param


@pytest.fixture
def cfg_path(source) -> Path:
    path = LIVING_ROOM if source == "living_room" else REAL
    assert path is not None
    return path


@pytest.fixture
def real_cfg(cfg_path) -> dict:
    return yaml.safe_load(cfg_path.read_text(encoding="utf-8"))


@pytest.fixture
async def loaded(hass, hub_rf, fake_addon, cfg_path):
    """HA with the config applied in the add-on and the entry set up against hub --sim."""
    fake_addon.apply_yaml(cfg_path.read_text(encoding="utf-8"))

    entry = make_entry("127.0.0.1", hub_rf.port, addon=fake_addon)
    entry.add_to_hass(hass)
    assert await hass.config_entries.async_setup(entry.entry_id)
    if entry.runtime_data.first_sync is not None:  # setup does not wait for the hub sync
        await entry.runtime_data.first_sync
    await hass.async_block_till_done()
    return entry


def _button(hass, device: str, command: str) -> str | None:
    return er.async_get(hass).async_get_entity_id(
        "button", "ha_harmony", f"{HUB_ID}_cmd_{device}:{command}"
    )


def _ir_line(hz: int, us: list[int], port: int = 255) -> str:
    return f"ir port={port} hz={hz} n={len(us)} us={','.join(str(d) for d in us)}"


async def test_real_config_loads_and_is_pushed_once(hass, loaded):
    assert loaded.state is ConfigEntryState.LOADED
    coord = loaded.runtime_data
    assert coord.config_pushes == 1, "the real config must reach the hub, exactly once"


async def test_one_button_per_real_command(hass, loaded, real_cfg, source):
    missing = [
        f"{d['id']}:{name}"
        for d in real_cfg["devices"]
        for name in d["commands"]
        if _button(hass, d["id"], name) is None
    ]
    assert not missing, f"commands with no button entity: {missing[:10]}"
    total = sum(len(d["commands"]) for d in real_cfg["devices"])
    assert total == COMMANDS[source], f"expected {COMMANDS[source]} IR commands; a different count means it changed"


async def test_each_real_device_is_an_ha_device(hass, loaded, real_cfg):
    names = {
        d.name
        for d in dr.async_entries_for_config_entry(dr.async_get(hass), loaded.entry_id)
    }
    for dev in real_cfg["devices"]:
        assert dev["name"] in names, f"{dev['name']} missing from the device registry"


@pytest.mark.parametrize(
    ("device", "command"),
    [
        ("samsung_tv", "VolumeUp"),  # the code that drove the real TV on 2026-11-17
        ("jtech_switch", None),  # first command of each never-driven device set
        ("kvm_1", None),
        ("audioengine_a5", None),
    ],
)
async def test_pressing_a_real_button_emits_its_exact_durations(
    hass, loaded, real_cfg, hub_rf, device, command
):
    dev = next(d for d in real_cfg["devices"] if d["id"] == device)
    command = command or next(iter(dev["commands"]))
    raw = dev["commands"][command]["raw"]

    entity_id = _button(hass, device, command)
    assert entity_id, f"no button for {device}:{command}"
    await hass.services.async_call("button", "press", {"entity_id": entity_id}, blocking=True)

    assert read_lines(hub_rf, 1) == [_ir_line(raw["hz"], raw["us"])], (
        f"{device}:{command} did not reach the hub's IR sink with its configured durations"
    )


def test_stock_power_timing_and_activities_survive_conversion(real_cfg):
    """The converter carries stock's own power actions and PowerOnDelay, verbatim.

    Values from the stock DeviceList.json PowerFeature: TV on/off with a 5000 ms power-on
    delay, KVM on/off 3500 ms, J-Tech a single toggle 1500 ms, Audioengine manual power.
    """
    dev = {d["id"]: d for d in real_cfg["devices"]}
    tv, kvm, jt, ae = dev["samsung_tv"], dev["kvm_1"], dev["jtech_switch"], dev["audioengine_a5"]
    assert set(tv["power"]) == {"on", "off"} and tv["settle_ms"] == 5000
    assert set(kvm["power"]) == {"on", "off"} and kvm["settle_ms"] == 3500
    assert set(jt["power"]) == {"toggle"} and jt["settle_ms"] == 1500
    assert "power" not in ae, "stock marks the Audioengine as manually powered"
    # power and inputs fire the identical signal the matching button does
    assert tv["power"]["on"] == tv["commands"]["PowerOn"]
    assert tv["inputs"]["HDMI 1"] == tv["commands"]["InputHdmi1"]
    assert tv["inputs"]["TV"] == tv["commands"]["InputTV"], "stock's InputTv/InputTV case clash"
    acts = {a["name"]: a for a in real_cfg["activities"]}
    assert acts["SHIELD TV"]["devices"] == [{"device": "samsung_tv", "input": "HDMI 1"}]
    assert acts["Watch Roku"]["devices"][0] == {"device": "samsung_tv", "input": "HDMI 3"}


@pytest.fixture
async def loaded_fast(hass, hub_rf, fake_addon, real_cfg):
    """As `loaded`, but with settle_ms shrunk to 20: the hub waits out settle in real time,
    and these tests are about WHICH IR is sent in WHAT order, not the stock delay values
    (those are pinned by test_stock_power_timing_and_activities_survive_conversion)."""
    cfg = real_cfg
    for d in cfg["devices"]:
        if "settle_ms" in d:
            d["settle_ms"] = 20
    fake_addon.apply_yaml(yaml.safe_dump(cfg, sort_keys=False))
    entry = make_entry("127.0.0.1", hub_rf.port, addon=fake_addon)
    entry.add_to_hass(hass)
    assert await hass.config_entries.async_setup(entry.entry_id)
    if entry.runtime_data.first_sync is not None:  # setup does not wait for the hub sync
        await entry.runtime_data.first_sync
    await hass.async_block_till_done()
    return cfg


async def test_activity_select_offers_the_stock_activities(hass, loaded, real_cfg):
    sel = er.async_get(hass).async_get_entity_id("select", "ha_harmony", f"{HUB_ID}_activity")
    names = [a["name"] for a in real_cfg["activities"]]
    assert {"SHIELD TV", "Watch Roku"} <= set(names)
    assert hass.states.get(sel).attributes["options"] == [*names, "Off"]


async def test_stock_activities_drive_the_tv_like_stock_does(hass, loaded_fast, hub_rf):
    """Off -> SHIELD TV -> Watch Roku -> Off, observed at the hub's IR sink.

    Target-state semantics, as stock's "fixit" model: switching between two activities that both
    want the TV on changes only its input -- no power cycle -- and Off powers it down.
    """
    tv = {d["id"]: d for d in loaded_fast["devices"]}["samsung_tv"]

    def line(name: str) -> str:
        raw = tv["commands"][name]["raw"]
        return _ir_line(raw["hz"], raw["us"])

    sel = er.async_get(hass).async_get_entity_id("select", "ha_harmony", f"{HUB_ID}_activity")

    async def pick(option: str) -> None:
        await hass.services.async_call(
            "select", "select_option", {"entity_id": sel, "option": option}, blocking=True
        )

    await pick("SHIELD TV")
    assert read_lines(hub_rf, 2) == [line("PowerOn"), line("InputHdmi1")]
    await pick("Watch Roku")
    assert read_lines(hub_rf, 1) == [line("InputHdmi3")], "TV already on: input change only"
    await pick("Off")
    assert read_lines(hub_rf, 1) == [line("PowerOff")]


async def test_stock_automations_work_after_an_entity_swap(hass, loaded, real_cfg, hub_rf):
    """Migration: an automation written for `remote.harmony` (stock) keeps its `device` and
    `command` verbatim -- stock's numeric device id, or the device's name -- and only its
    entity_id changes. Every alias of every device, with the device's first and last command."""
    remote = er.async_get(hass).async_get_entity_id("remote", "ha_harmony", f"{HUB_ID}_remote")
    for dev in real_cfg["devices"]:
        assert dev["aliases"], dev["id"]
        for alias in dev["aliases"]:
            for command in (next(iter(dev["commands"])), list(dev["commands"])[-1]):
                await hass.services.async_call(
                    "remote",
                    "send_command",
                    {"entity_id": remote, "device": alias, "command": command},
                    blocking=True,
                )
                raw = dev["commands"][command]["raw"]
                assert read_lines(hub_rf, 1) == [_ir_line(raw["hz"], raw["us"])], (dev["id"], alias, command)


async def test_restart_does_not_re_push_an_unchanged_config(hass, loaded):
    """The hash gate: a reload with the same file must not resend 85 commands to the hub."""
    coord = loaded.runtime_data
    assert coord.config_pushes == 1
    assert await hass.config_entries.async_reload(loaded.entry_id)
    await hass.async_block_till_done()
    assert loaded.runtime_data.config_pushes == 0, "unchanged config was pushed again"
