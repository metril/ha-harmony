"""App rings: app_id inference, compiler bindings, status transitions, source_list inputs; no timers."""

import re
from pathlib import Path

from homeassistant.core import State

import custom_components.ha_harmony  # noqa: F401  (before the HA plugin shadows it)
from custom_components.ha_harmony import app_status as aps
from harmony_core.compiler import compile_config
from harmony_core.config_model import infer_app_ids, parse_model
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
    rows: 3
    widgets:
      - {{type: button, id: b1, x: 0, y: 0, w: 12, shape: row, label: Plex, action: {{device: sh, command: Plex}}}}
      - {{type: button, id: b2, x: 0, y: 1, w: 12, shape: row, label: Video, app_id: [{YT}, com.google.android.youtube.tv], action: {{device: sh, command: Plex}}}}
      - {{type: button, id: b3, x: 0, y: 2, w: 12, shape: row, label: Other, action: {{device: sh, command: Plex}}}}
"""


def test_inference_table():
    assert infer_app_ids("Plex") == [PLEX]
    assert infer_app_ids("YouTube") == [YT, "com.google.android.youtube.tv"]
    assert infer_app_ids("Go", "kodi") == ["org.xbmc.kodi"]
    assert infer_app_ids("Netflix") == ["com.netflix.ninja"]
    assert infer_app_ids("Camera") == []


def test_compiler_bindings_media_and_source_list_inputs():
    c = compile_config(parse_model(CFG))
    assert [(b.app_ids, b.entity_id) for b in c.apps] == [
        ((PLEX,), SHIELD),
        ((YT, "com.google.android.youtube.tv"), SHIELD),
    ]
    assert len({(b.page, b.widget) for b in c.apps}) == 2
    assert c.media == {"a": {"apps_entity": SHIELD, "now_playing_entity": "media_player.plex"}}
    step = next(s for s in c.ha_activities["a"].start if s.kind == "input")
    assert step.call.service == "media_player.select_source" and step.call.data == {"source": "HDMI 2"}


def test_source_list_names_become_inputs():
    c = compile_config(parse_model(CFG), source_lists={SHIELD: ["HDMI 1", "HDMI 2"]})
    assert c.devices["sh"].inputs == ("HDMI 1", "HDMI 2")
    assert c.ha_devices["sh"].inputs["HDMI 1"].data == {"source": "HDMI 1"}


def _st(state, app):
    return State(SHIELD, state, {"app_id": app})


def test_status_transitions():
    b = AppBinding(0, 1, (PLEX,), SHIELD)
    assert aps.status(b, _st("playing", PLEX)) == 2
    assert aps.status(b, _st("paused", PLEX)) == 3
    assert aps.status(b, _st("idle", PLEX)) == 1
    assert aps.status(b, _st("playing", YT)) == 0
    assert aps.status(b, _st("off", PLEX)) == 0
    assert aps.status(b, None) == 0


class FakeClient:
    connected = True

    def __init__(self):
        self.sent = []

    async def async_send(self, _type, data):
        self.sent.append(data["args"])
        return {}


async def test_feed_sends_only_changes_and_resends_on_attach(hass, caplog):
    caplog.set_level("INFO")
    client = FakeClient()
    feed = aps.AppStatusFeed(
        hass, client, lambda: (1,), lambda r: True, lambda coro, name: hass.async_create_task(coro, name)
    )
    feed.configure([AppBinding(0, 5, (PLEX,), SHIELD), AppBinding(0, 6, (YT,), SHIELD)])
    hass.states.async_set(SHIELD, "playing", {"app_id": PLEX})
    await hass.async_block_till_done()
    assert [(a["tile"], a["state"]) for a in client.sent] == [(5, 0), (6, 0), (5, 2)]  # baseline, then the change
    assert client.sent[0]["text"] == "" and client.sent[0]["remote"] == 1
    del client.sent[:2]
    hass.states.async_set(SHIELD, "playing", {"app_id": PLEX, "x": 1})  # same status: nothing
    await hass.async_block_till_done()
    assert len(client.sent) == 1
    hass.states.async_set(SHIELD, "paused", {"app_id": YT})
    await hass.async_block_till_done()
    assert [(a["tile"], a["state"]) for a in client.sent[1:]] == [(5, 0), (6, 3)]
    hass.states.async_set(SHIELD, "playing", {"app_id": "com.unknown.app"})
    await hass.async_block_till_done()
    hass.states.async_set(SHIELD, "paused", {"app_id": "com.unknown.app"})
    await hass.async_block_till_done()
    assert sum("com.unknown.app" in r.getMessage() for r in caplog.records) == 1
    hass.states.async_set(SHIELD, "playing", {"app_id": PLEX})
    await hass.async_block_till_done()
    n = len(client.sent)
    feed.on_remote_attached(1)  # the remote lost its rings: the live one comes again
    await hass.async_block_till_done()
    assert [(a["tile"], a["state"]) for a in client.sent[n:]] == [(5, 2), (6, 0)]
    feed.stop()


def test_no_timers_in_app_status():
    src = Path(aps.__file__).read_text()
    assert not re.search(
        r"call_later|call_at|async_call_later|track_time|async_track_point|asyncio\.sleep|loop\.call_", src
    )
