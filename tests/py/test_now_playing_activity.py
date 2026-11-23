"""Now Playing after an activity change: the remote drops its card then, and no player event follows."""

import asyncio
from datetime import timedelta
from types import SimpleNamespace

from homeassistant.util import dt as dt_util

import custom_components.ha_harmony  # noqa: F401  (before the HA plugin shadows it)
from custom_components.ha_harmony import now_playing as npl
from custom_components.ha_harmony.coordinator import HarmonyCoordinator

SHIELD, PLEX = npl.DEFAULT_SHIELD, npl.DEFAULT_PLEX


class FakeClient:
    connected = True

    def __init__(self):
        self.sent = []

    async def async_send(self, mtype, data):
        self.sent.append((mtype, data))


def _feed(hass, client, now):
    return npl.NowPlayingFeed(
        hass,
        client,
        lambda: (1,),
        lambda r: True,
        lambda coro, name: hass.async_create_task(coro, name),
        clock=lambda: now[0],
    )


def _np(client, since=0):
    return [m[1]["args"] for m in client.sent[since:] if m[1]["command"] == "now_playing"]


async def test_activity_start_while_plex_plays_sends_one_now_playing_with_the_running_position(
    hass,
):
    client, now = FakeClient(), [dt_util.utcnow()]
    feed = _feed(hass, client, now)
    hass.states.async_set(SHIELD, "playing", {"app_id": "com.plexapp.android"})
    hass.states.async_set(
        PLEX,
        "playing",
        {
            "media_title": "T",
            "media_content_id": "x",
            "media_duration": 3000,
            "media_position": 100,
            "media_position_updated_at": now[0],
        },
    )
    feed.start()
    await hass.async_block_till_done()
    n = len(client.sent)
    now[0] += timedelta(seconds=40)
    feed.resend()  # the activity started: the players did not change
    await hass.async_block_till_done()
    got = _np(client, n)
    assert len(got) == 1 and got[0]["pos"] == 140 and got[0]["flags"] == npl.PLAYING
    now[0] += timedelta(seconds=10)  # off, then on again
    feed.resend()
    await hass.async_block_till_done()
    got = _np(client, n)
    assert len(got) == 2 and got[1]["pos"] == 150
    feed.stop()
    await asyncio.sleep(0)


async def test_resend_with_nothing_playing_sends_nothing_playing_only(hass):
    client, now = FakeClient(), [dt_util.utcnow()]
    feed = _feed(hass, client, now)
    feed.start()
    await hass.async_block_till_done()
    n = len(client.sent)
    feed.resend()
    await hass.async_block_till_done()
    assert all(a["flags"] == 0 for a in _np(client, n))
    feed.stop()
    await asyncio.sleep(0)


def test_coordinator_resends_when_the_activity_with_the_feed_media_starts():
    calls = []
    fake = SimpleNamespace(
        compiled=SimpleNamespace(
            media={"a": {"now_playing_entity": PLEX}, "b": {"apps_entity": SHIELD}, "c": {}}
        ),
        _cur=lambda e: e,
        now_playing=SimpleNamespace(plex=PLEX, resend=lambda: calls.append(1)),
    )
    for after in (None, "c", "zz"):
        HarmonyCoordinator._np_activity(fake, after)
    assert calls == []
    HarmonyCoordinator._np_activity(fake, "a")
    HarmonyCoordinator._np_activity(
        fake, "b"
    )  # any activity with a media entity, not only the feed's Plex
    assert calls == [1, 1]
