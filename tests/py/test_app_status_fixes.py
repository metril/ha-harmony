"""Review fixes: source_list wiring and checks, activity-aware rings, full resends, rename watching."""

from types import SimpleNamespace

import pytest
import yaml
from homeassistant.core import State

import custom_components.ha_harmony  # noqa: F401  (before the HA plugin shadows it)
from custom_components.ha_harmony import app_status as aps
from custom_components.ha_harmony.addon_link import BundleRejected, compile_source
from custom_components.ha_harmony.coordinator import HarmonyCoordinator
from harmony_core.compiler import CompileError, compile_config
from harmony_core.config_model import parse_model
from harmony_core.page_build import AppBinding

SHIELD = "media_player.nvidia_shield_tv"
PLEX = "com.plexapp.android"
YT = "org.smarttube.stable"

CFG = f"""
devices:
  - {{id: sh, kind: ha, entity: {SHIELD}, source_list: true, commands: {{Plex: {{ha: {{service: media_player.play_media, target: {{entity_id: {SHIELD}}}}}}}}}}}
activities:
  - id: a
    media: {{apps_entity: {SHIELD}, now_playing_entity: media_player.plex}}
    devices: [{{device: sh, input: HDMI 2}}]
pages:
  - id: p
    activity: a
    layout: fixed
    cols: 12
    rows: 2
    widgets:
      - {{type: button, id: b1, x: 0, y: 0, w: 12, shape: row, label: Plex, action: {{device: sh, command: Plex}}}}
      - {{type: button, id: b2, x: 0, y: 1, w: 12, shape: row, label: YouTube, action: {{device: sh, command: Plex}}}}
"""


def test_unknown_input_is_rejected_when_the_source_list_is_known():
    model = parse_model(CFG)
    with pytest.raises(CompileError, match="HDMI 2.*source_list"):
        compile_config(model, source_lists={SHIELD: ["HDMI 1", "Netflix"]})
    compile_config(model, source_lists={SHIELD: ["HDMI 2"]})
    compile_config(model)  # list unknown: lenient


def test_compile_source_takes_source_lists_and_coordinator_reads_them(hass):
    cfg = yaml.safe_load(CFG)
    with pytest.raises(BundleRejected, match="source_list"):
        compile_source(cfg, "h", None, {SHIELD: ["HDMI 1"]})
    hass.states.async_set(SHIELD, "on", {"source_list": ["HDMI 1", "HDMI 2"]})
    fake = SimpleNamespace(hass=hass, _cur=lambda e: e, _sl_used={})
    got = HarmonyCoordinator._source_lists(fake, cfg)
    assert got == {SHIELD: ("HDMI 1", "HDMI 2")}
    assert compile_source(cfg, "h", None, got).devices["sh"].inputs == ("HDMI 1", "HDMI 2")


async def _noop():
    return None


def test_source_list_change_recompiles_only_on_a_new_list():
    tasks = []

    def spawn(_hass, coro, name):
        coro.close()
        tasks.append(name)

    fake = SimpleNamespace(
        _sl_used={SHIELD: ("A",)},
        _source_cfg={"devices": []},
        managed=True,
        config_entry=SimpleNamespace(async_create_background_task=spawn),
        hass=None,
        _recompile=_noop,
    )

    def on(lst):
        HarmonyCoordinator._on_source_list(fake, SHIELD, State(SHIELD, "on", {"source_list": lst}))

    on(["A"])
    assert tasks == []
    on(["A", "B"])
    assert tasks == ["ha_harmony source_list"]


def _st(state, app):
    return State(SHIELD, state, {"app_id": app})


def test_rings_clear_when_the_activity_is_not_running():
    b = AppBinding(0, 1, (PLEX,), SHIELD, "a")
    assert aps.status(b, _st("playing", PLEX), "a") == 2
    assert aps.status(b, _st("playing", PLEX), "other") == 0
    assert aps.status(b, _st("playing", PLEX), None) == 0
    assert [x.activity for x in compile_config(parse_model(CFG)).apps] == ["a", "a"]


class FakeClient:
    connected = True

    def __init__(self):
        self.sent = []

    async def async_send(self, _type, data):
        self.sent.append(data["args"])
        return {}


async def test_full_resend_includes_zero_and_activity_changes_clear(hass):
    client, running = FakeClient(), ["a"]
    feed = aps.AppStatusFeed(
        hass, client, lambda: (1,), lambda r: True,
        lambda coro, name: hass.async_create_task(coro, name), lambda e: e, lambda: running[0],
    )
    hass.states.async_set(SHIELD, "playing", {"app_id": PLEX})
    feed.configure([AppBinding(0, 5, (PLEX,), SHIELD, "a"), AppBinding(0, 6, (YT,), SHIELD, "a")])
    await hass.async_block_till_done()
    assert sorted((a["tile"], a["state"]) for a in client.sent) == [(5, 2), (6, 0)]  # the 0 is sent too
    running[0] = None  # the activity turned off
    feed.refresh()
    await hass.async_block_till_done()
    assert [(a["tile"], a["state"]) for a in client.sent[2:]] == [(5, 0)]
    n = len(client.sent)
    feed.on_remote_attached(1)
    await hass.async_block_till_done()
    assert sorted((a["tile"], a["state"]) for a in client.sent[n:]) == [(5, 0), (6, 0)]
    feed.stop()


def test_renames_watch_the_media_entities():
    import inspect

    src = inspect.getsource(HarmonyCoordinator._on_registry_update)
    assert "compiled.media" in src
    media = compile_config(parse_model(CFG)).media
    assert {e for m in media.values() for e in m.values()} == {SHIELD, "media_player.plex"}
