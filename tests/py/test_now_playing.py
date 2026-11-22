"""now_playing.py: the cover (1760 bytes), title lines and font fallback, and that nothing is timer driven."""

import asyncio
import base64
import io
import re
import zlib
from datetime import timedelta
from pathlib import Path

from homeassistant.util import dt as dt_util
from PIL import Image

import custom_components.ha_harmony  # noqa: F401  (before the HA plugin shadows it)
from custom_components.ha_harmony import now_playing as npl

SHIELD, PLEX = npl.DEFAULT_SHIELD, npl.DEFAULT_PLEX
APPS = npl.DEFAULT_PLEX_APPS


def _png(size=(300, 450)) -> bytes:
    img = Image.new("RGB", size)
    img.putdata(
        [((x * 255) // size[0], (y * 255) // size[1], (x + y) % 256) for y in range(size[1]) for x in range(size[0])]
    )
    buf = io.BytesIO()
    img.save(buf, "PNG")
    return buf.getvalue()


def test_cover_is_1760_bytes_with_a_16_entry_palette():
    out = npl.quantize_cover(_png())
    assert len(out) == npl.COVER_BYTES == 1760
    body, pal = out[:1728], out[1728:]
    assert len(pal) == 32
    assert max(max(b >> 4, b & 15) for b in body) < 16
    # a flat red image: its pixels index a palette colour that is red in RGB565 (little-endian)
    red = io.BytesIO()
    Image.new("RGB", (100, 100), (255, 0, 0)).save(red, "PNG")
    out = npl.quantize_cover(red.getvalue())
    idx = out[0] >> 4
    assert int.from_bytes(out[1728 + idx * 2 : 1730 + idx * 2], "little") == 0xF800


def _state(hass, entity, state, **attrs):
    hass.states.async_set(entity, state, attrs)
    return hass.states.get(entity)


PLEX_ATTRS = {
    "media_series_title": "Severance",
    "media_season": 2,
    "media_episode": 3,
    "media_title": "Who Is Alive?",
    "media_duration": 3000,
    "media_position": 100,
    "media_content_id": "/library/metadata/42",
    "media_library_title": "TV Shows",
    "entity_picture": "/api/media_player_proxy/x?token=t",
}


def test_lines_cid_and_position(hass):
    now = dt_util.utcnow()
    shield = _state(hass, SHIELD, "playing", app_id="com.plexapp.android")
    plex = _state(hass, PLEX, "playing", media_position_updated_at=now - timedelta(seconds=10), **PLEX_ATTRS)
    p = npl.build(shield, plex, now, APPS, "m16")
    assert (p.t1, p.t2) == ("Severance", "S2 E3 · Who Is Alive?")
    assert p.cid == zlib.crc32(b"/library/metadata/42")
    assert (p.flags, p.pos, p.dur) == (npl.PLAYING, 110, 3000)
    assert p.picture == PLEX_ATTRS["entity_picture"]
    paused = _state(hass, PLEX, "paused", **PLEX_ATTRS)
    p = npl.build(shield, paused, now, APPS, "m16")
    assert (p.flags, p.pos) == (npl.PAUSED, 100)
    movie = dict(PLEX_ATTRS, media_series_title=None, media_title="Heat")
    p = npl.build(shield, _state(hass, PLEX, "playing", **movie), now, APPS, "m16")
    assert (p.t1, p.t2) == ("Heat", "")
    assert len(npl.cut("é" * 80).encode()) <= 48
    other = _state(hass, SHIELD, "playing", app_id="org.smarttube.stable", media_title="Video")
    p = npl.build(other, plex, now, APPS, "m16")
    assert (p.t1, p.picture) == ("Video", None)
    off = _state(hass, SHIELD, "off", app_id="x")
    assert npl.build(off, plex, now, APPS, "m16").flags == 0


def test_font_fallback_to_the_library_title(hass):
    now = dt_util.utcnow()
    shield = _state(hass, SHIELD, "playing", app_id="com.plexapp.android")
    odd = dict(PLEX_ATTRS, media_series_title="進撃の巨人", media_title="始まり")
    assert not npl.has_glyphs("進撃の巨人", "m16")
    plex = _state(hass, PLEX, "playing", **odd)
    p = npl.build(shield, plex, now, APPS, "m16")
    assert p.t1 == "TV Shows"
    assert p.t2 == "S2 E3"  # the episode title has no glyphs: only the tag
    plex = _state(hass, PLEX, "playing", **dict(odd, media_library_title="アニメ"))
    assert npl.build(shield, plex, now, APPS, "m16").t1 == ""


class FakeClient:
    connected = True

    def __init__(self):
        self.sent = []

    async def async_send(self, _type, data):
        self.sent.append((data["command"], data["args"]))
        return {}


async def test_feed_is_event_driven_and_sends_cover_then_text(hass):
    client, fetched = FakeClient(), []

    async def fetch(url):
        fetched.append(url)
        return _png()

    feed = npl.NowPlayingFeed(
        hass,
        client,
        lambda: (1,),
        lambda r: True,
        lambda coro, name: hass.async_create_task(coro, name),
        fetch=fetch,
    )
    feed.start()
    _state(hass, SHIELD, "playing", app_id="com.plexapp.android")
    _state(hass, PLEX, "playing", media_position_updated_at=dt_util.utcnow(), **PLEX_ATTRS)
    await hass.async_block_till_done()
    cmds = [c for c, _ in client.sent]
    assert cmds.count("cover") == 1 and cmds[-2:] == ["cover", "now_playing"]
    cover = next(a for c, a in client.sent if c == "cover")
    assert len(base64.b64decode(cover["data"])) == 1760 and cover["cid"] == zlib.crc32(b"/library/metadata/42")
    assert len(fetched) == 1
    # a pure attribute tick (the position moves on by itself) sends nothing; a pause sends text only
    n = len(client.sent)
    ticked = dict(PLEX_ATTRS, media_position=101)
    _state(hass, PLEX, "playing", media_position_updated_at=dt_util.utcnow(), **ticked)
    await hass.async_block_till_done()
    assert len(client.sent) == n
    _state(hass, PLEX, "paused", **PLEX_ATTRS)
    await hass.async_block_till_done()
    assert [c for c, _ in client.sent[n:]] == ["now_playing"]
    assert client.sent[-1][1]["flags"] == npl.PAUSED
    # a remote that joins later is shown the current state again, cover included
    feed.on_remote_attached(1)
    await hass.async_block_till_done()
    assert [c for c, _ in client.sent[-2:]] == ["cover", "now_playing"] and len(fetched) == 1
    feed.stop()
    await asyncio.sleep(0)


def test_no_timers_in_the_feed():
    src = Path(npl.__file__).read_text()
    assert not re.search(
        r"call_later|call_at|async_call_later|track_time|async_track_point|asyncio\.sleep|loop\.call_", src
    )


async def test_attach_resend_runs_pos_on_and_a_failed_cover_fetch_is_retried(hass, caplog):
    client, calls, now = FakeClient(), [], [dt_util.utcnow()]

    async def fetch(url):
        calls.append(url)
        return None if len(calls) == 1 else _png()

    feed = npl.NowPlayingFeed(
        hass, client, lambda: (1,), lambda r: True,
        lambda coro, name: hass.async_create_task(coro, name), fetch=fetch, clock=lambda: now[0],
    )
    feed.start()
    _state(hass, SHIELD, "playing", app_id="com.plexapp.android")
    _state(hass, PLEX, "playing", media_position_updated_at=now[0], **PLEX_ATTRS)
    await hass.async_block_till_done()
    assert [c for c, _ in client.sent].count("cover") == 0 and len(calls) == 1
    assert sum("cover for" in r.getMessage() and r.levelname == "WARNING" for r in caplog.records) == 1
    _state(hass, PLEX, "paused", **PLEX_ATTRS)  # the next push tries again, and does not warn twice
    await hass.async_block_till_done()
    assert len(calls) == 2 and [c for c, _ in client.sent].count("cover") == 1
    _state(hass, PLEX, "playing", media_position_updated_at=now[0], **PLEX_ATTRS)
    await hass.async_block_till_done()
    now[0] += timedelta(seconds=30)
    feed.on_remote_attached(1)
    await hass.async_block_till_done()
    last = [a for c, a in client.sent if c == "now_playing"][-1]
    assert last["pos"] == 130
    feed.stop()


async def test_text_goes_out_even_when_the_cover_cannot_be_fetched(hass):
    """Field bug: with no HA URL configured the default cover fetch raised NoURLAvailableError, which killed
    the push before the now_playing text was sent (and nothing logged it)."""
    client = FakeClient()
    feed = npl.NowPlayingFeed(hass, client, lambda: (1,), lambda r: True, lambda coro, name: hass.async_create_task(coro, name))
    feed.start()
    _state(hass, SHIELD, "playing", app_id="com.plexapp.android")
    _state(hass, PLEX, "playing", media_position_updated_at=dt_util.utcnow(), **PLEX_ATTRS)
    await hass.async_block_till_done()
    assert {c for c, _ in client.sent} == {"now_playing"} and client.sent[-1][1]["t1"] == "Severance"
    assert feed.sent_now_playing >= 1 and feed.cover_errors == 1
    feed.stop()
    await asyncio.sleep(0)


async def test_feed_primes_from_the_current_state_on_start_and_on_attach(hass):
    _state(hass, SHIELD, "playing", app_id="com.plexapp.android")
    _state(hass, PLEX, "playing", media_position_updated_at=dt_util.utcnow(), **dict(PLEX_ATTRS, entity_picture=None))
    client, up = FakeClient(), [False]
    client.connected = False
    feed = npl.NowPlayingFeed(
        hass, client, lambda: (1,), lambda r: up[0], lambda coro, name: hass.async_create_task(coro, name)
    )
    feed.start()  # nothing has changed since: the start itself must read the state
    await hass.async_block_till_done()
    assert client.sent == []
    client.connected = up[0] = True
    feed.on_remote_attached(1)
    await hass.async_block_till_done()
    assert [c for c, _ in client.sent] == ["now_playing"]
    feed.stop()
    await asyncio.sleep(0)
