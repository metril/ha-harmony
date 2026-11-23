"""Now Playing for the remote: a Shield (androidtv) media player plus the Plex player behind it.

Push only: the feed listens to state changes of the two configured entities and sends the hub a
`now_playing` command (title lines, flags, position) and, when the media changed, a `cover` command
(48x72, 4 bpp, 16 RGB565 palette entries: 1760 bytes). There is no timer anywhere: the position
runs on in the hub, which adds the time a message was held to `pos` when it goes out.

Wire (hub `command` verbs, device "hub"):
  now_playing {remote, cid u32, flags u8 (1 playing, 2 paused, 0 nothing), pos s, dur s, t1, t2}
  cover       {remote, cid u32, data = base64 of the 1760 bytes}

Cover bytes: 1728 bytes of 48x72 pixels, row by row, two per byte (first pixel in the high nibble),
each a palette index; then 16 RGB565 entries, little-endian (32 bytes).
"""

from __future__ import annotations

import asyncio
import base64
import io
import logging
import zlib
from collections.abc import Awaitable, Callable, Iterable
from dataclasses import dataclass
from datetime import datetime
from typing import Any

from homeassistant.components.media_player import DATA_COMPONENT
from homeassistant.core import Event, HomeAssistant, State, callback
from homeassistant.helpers.event import EventStateChangedData, async_track_state_change_event
from homeassistant.util import dt as dt_util

from .api import HarmonyCommandError, HarmonyConnectionError
from .harmony_core.harmony_proto import MsgType
from .harmony_core.text_fit import _metrics

_LOGGER = logging.getLogger(__name__)

# Defaults for the options below (the real entities of this install); an activity's `media:` in the
# config (apps_entity = the Shield, now_playing_entity = the Plex player) overrides them (configure()).
DEFAULT_SHIELD = "media_player.nvidia_shield_tv"
DEFAULT_PLEX = "media_player.plex_plex_for_android_tv_shield_android_tv"
DEFAULT_PLEX_APPS = ("com.plexapp.android",)
DEFAULT_FONT = "m16"  # the baked font of the remote's title lines (proto/font_metrics.json)

CMD_NOW_PLAYING = "now_playing"
CMD_COVER = "cover"
HUB_DEVICE = "hub"
TEXT_MAX = 48  # bytes of UTF-8 per title line
COVER_W, COVER_H = 48, 72
COVER_BYTES = COVER_W * COVER_H // 2 + 32  # 1760
PLAYING, PAUSED = 1, 2
_SEEK_SLACK = 3  # seconds a start-time estimate may drift before it counts as a seek


def options(entry_options: dict[str, Any]) -> dict[str, Any]:
    """The feed's settings from integration options (keys now_playing_shield / _plex / _plex_apps /
    _font); anything absent takes the documented default."""
    return {
        "shield": entry_options.get("now_playing_shield") or DEFAULT_SHIELD,
        "plex": entry_options.get("now_playing_plex") or DEFAULT_PLEX,
        "plex_apps": tuple(entry_options.get("now_playing_plex_apps") or DEFAULT_PLEX_APPS),
        "font": entry_options.get("now_playing_font") or DEFAULT_FONT,
    }


def cut(text: str, limit: int = TEXT_MAX) -> str:
    """At most `limit` bytes of UTF-8, never splitting a character."""
    return text.encode()[:limit].decode(errors="ignore")


def has_glyphs(text: str, font: str) -> bool:
    """Every character of `text` is in the remote's baked `font` (else it would show placeholders)."""
    m = _metrics()[font]
    return all(str(ord(c)) in m for c in text)


def _text(value: Any) -> str:
    return "" if value is None else str(value).strip()


def _line(text: str, fallbacks: Iterable[str], font: str) -> str:
    """`text`, or the first fallback, whose glyphs the font has; "" when none does."""
    for cand in (text, *fallbacks):
        if cand and has_glyphs(cand, font):
            return cut(cand)
    return ""


@dataclass(frozen=True)
class Playing:
    cid: int
    flags: int
    pos: int
    dur: int
    t1: str
    t2: str
    picture: str | None

    @property
    def sig(self) -> tuple[int, int, int, str, str]:
        """What a resend would change on the remote (pos excluded: the hub runs it on)."""
        return (self.cid, self.flags, self.dur, self.t1, self.t2)


def _flags(state: State | None) -> int:
    return {"playing": PLAYING, "paused": PAUSED}.get(state.state if state else "", 0)


def _int(value: Any) -> int:
    try:
        return max(int(float(value)), 0)
    except TypeError, ValueError:
        return 0


def _position(attrs: Any, flags: int, now: datetime) -> int:
    pos = _int(attrs.get("media_position"))
    at = attrs.get("media_position_updated_at")
    if isinstance(at, str):  # an MQTT sensor's attributes carry the timestamp as ISO text
        at = dt_util.parse_datetime(at)
    if flags == PLAYING and isinstance(at, datetime):
        pos += max(int((now - at).total_seconds()), 0)
    return pos


def build(
    shield: State | None,
    plex: State | None,
    now: datetime,
    plex_apps: Iterable[str],
    font: str,
    session: State | None = None,
) -> Playing | None:
    """What the remote should show now, or None when neither player is in use. The Plex player gives
    the details while the Shield runs a Plex app; any other app shows its own title (no cover, no
    position unless the player reports them)."""
    on_plex = (
        shield is not None
        and shield.attributes.get("app_id") in tuple(plex_apps)
        and plex is not None
        and plex.state in ("playing", "paused")
    )
    sess = (
        session
        if not on_plex
        and session is not None
        and session.state in ("playing", "paused")
        and (
            shield is None
            or session.attributes.get("app_id") == shield.attributes.get("app_id")
            or not shield.attributes.get("media_title")
        )
        else None
    )
    src = sess or (plex if on_plex else shield)
    if src is None or src.state in ("unavailable", "unknown"):
        return None
    flags = _flags(src)
    a = src.attributes
    title, series = _text(a.get("media_title")), _text(a.get("media_series_title"))
    library = _text(a.get("media_library_title"))
    if flags == 0 and not title:
        return Playing(0, 0, 0, 0, "", "", None)
    t1 = _line(series or title, (library,), font)
    t2 = ""
    if sess is not None:
        artist = _text(a.get("media_artist"))
        t2 = cut(artist) if artist and has_glyphs(artist, font) else ""
    season, episode = a.get("media_season"), a.get("media_episode")
    if sess is None and series and (season not in (None, "") or episode not in (None, "")):
        tag = f"S{_int(season)} E{_int(episode)}"
        t2 = cut(f"{tag} · {title}") if title and has_glyphs(f"{tag} · {title}", font) else tag
    content = _text(a.get("media_content_id")) or title
    cid = zlib.crc32(content.encode()) & 0xFFFFFFFF if content else 0
    if sess is not None:
        img = a.get("media_image_url")
        picture = img if isinstance(img, str) and img.startswith("https://") else None
    else:
        picture = a.get("entity_picture") if on_plex else None
    return Playing(
        cid, flags, _position(a, flags, now), _int(a.get("media_duration")), t1, t2, picture
    )


def _rgb565(r: int, g: int, b: int) -> int:
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def quantize_cover(raw: bytes) -> bytes:
    """Image bytes -> the 1760-byte cover: fitted (centre crop) to 48x72, 16 colours, packed 4 bpp
    with an RGB565 palette. Blocking: run it in an executor."""
    from PIL import Image, ImageOps

    with Image.open(io.BytesIO(raw)) as src:
        img = ImageOps.fit(src.convert("RGB"), (COVER_W, COVER_H), method=Image.Resampling.LANCZOS)
    q = img.quantize(colors=16, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    flat = (q.getpalette() or [])[:48]
    flat += [0] * (48 - len(flat))
    pal = b"".join(_rgb565(*flat[i * 3 : i * 3 + 3]).to_bytes(2, "little") for i in range(16))
    px = q.tobytes()
    body = bytes((px[i] << 4) | px[i + 1] for i in range(0, len(px), 2))
    out = body + pal
    assert len(out) == COVER_BYTES
    return out


Fetch = Callable[[str], Awaitable[bytes | None]]
_COVER_MAX = 2 * 1024 * 1024  # largest cover download accepted
_COVER_TIMEOUT = 10  # seconds


class NowPlayingFeed:
    """Subscribes to the Shield and Plex players and tells the hub what to show, per attached remote."""

    def __init__(
        self,
        hass: HomeAssistant,
        client: Any,
        remotes: Callable[[], Iterable[int]],
        attached: Callable[[int], bool],
        spawn: Callable[[Awaitable[None], str], asyncio.Task[None]],
        *,
        shield: str = DEFAULT_SHIELD,
        plex: str = DEFAULT_PLEX,
        session: str | None = None,
        plex_apps: Iterable[str] = DEFAULT_PLEX_APPS,
        font: str = DEFAULT_FONT,
        fetch: Fetch | None = None,
        fetch_url: Fetch | None = None,
        clock: Callable[[], datetime] = dt_util.utcnow,
    ) -> None:
        self.hass, self.client = hass, client
        self._remotes, self._attached, self._spawn = remotes, attached, spawn
        self.shield, self.plex, self.plex_apps, self.font = shield, plex, tuple(plex_apps), font
        self.session = self._opt_session = session
        self._opt_shield, self._opt_plex = shield, plex
        self._fetch = fetch or self._fetch_picture
        self._fetch_url = fetch_url or self._fetch_https
        self._clock = clock
        self._unsub: Callable[[], None] | None = None
        self._task: asyncio.Task[None] | None = None
        self._dirty = False
        self._last: Playing | None = (
            None  # the latest built state (what an attaching remote is shown)
        )
        self._sent: Playing | None = (
            None  # the state last handed to the hub for the remotes then attached
        )
        self._start = 0.0  # estimated media start (clock epoch seconds) of _last
        self._sent_start = 0.0  # the same for _sent
        self._cover: tuple[int, str] | None = None  # (cid, base64) of the cover for _last
        self._cover_sent: int | None = (
            None  # cid last handed to the hub (it keeps it; resent on a rejoin only)
        )
        self._warned: set[int] = set()
        self._cover_for: int | None = None  # cid the cover above (or a fetch in flight) belongs to
        self.sent_now_playing = 0
        self.sent_covers = 0
        self.failed_sends = 0
        self.cover_errors = 0

    # -- lifecycle (events only: nothing here schedules a timer)
    def start(self) -> None:
        self.stop()
        self._unsub = async_track_state_change_event(
            self.hass, [e for e in (self.shield, self.plex, self.session) if e], self._on_event
        )
        self._on_event(None)  # type: ignore[arg-type]  # prime from the current state

    def configure(self, shield: str | None, plex: str | None, session: str | None = None) -> None:
        """Use the entities of the live config (None keeps the option default)."""
        shield, plex = shield or self._opt_shield, plex or self._opt_plex
        session = session or self._opt_session
        if (shield, plex, session) == (self.shield, self.plex, self.session):
            return
        self.shield, self.plex, self.session = shield, plex, session
        self._last = self._sent = None
        if self._unsub:  # started: follow the new entities (start() primes)
            self.start()

    def stop(self) -> None:
        if self._unsub:
            self._unsub()
            self._unsub = None
        if self._task is not None and not self._task.done():
            self._task.cancel()
        self._task = None

    @callback
    def _on_event(self, _event: Event[EventStateChangedData]) -> None:
        """Runs one update at a time; events that arrive meanwhile are folded into one more pass."""
        self._dirty = True
        if self._task is None or self._task.done():
            self._task = self._spawn(self._run(), "ha_harmony now playing")

    def on_remote_attached(self, remote: int) -> None:
        """A remote (re)joined: it knows nothing, show it the current state again."""
        cur = self._last
        if cur is None:
            cur = self._build()
            if cur is None:
                return
            self._start = self._clock().timestamp() - cur.pos
        self._last = cur
        self._spawn(self._attach_push(cur, remote), "ha_harmony now playing attach")

    def resend(self) -> None:
        """The running activity changed: the remote dropped its card and no player event follows,
        so send the current state to every attached remote (one now_playing; the position runs on
        from the stored start estimate)."""
        cur = self._last
        if cur is None:
            cur = self._build()
            if cur is None:
                return
            self._start = self._clock().timestamp() - cur.pos
            self._last = cur
        self._spawn(self._resend_push(cur), "ha_harmony now playing resend")

    async def _resend_push(self, cur: Playing) -> None:
        try:
            await self._push(cur, tuple(self._remotes()), force=True)
        except Exception:
            _LOGGER.exception("now playing resend failed")

    async def _attach_push(self, cur: Playing, remote: int) -> None:
        try:
            await self._push(cur, (remote,), force=True)
        except Exception:
            _LOGGER.exception("now playing attach push failed")

    def _build(self) -> Playing | None:
        return build(
            self.hass.states.get(self.shield),
            self.hass.states.get(self.plex),
            self._clock(),
            self.plex_apps,
            self.font,
            self.hass.states.get(self.session) if self.session else None,
        )

    # -- work
    async def _run(self) -> None:
        while self._dirty:
            self._dirty = False
            cur = self._build()
            if cur is None:
                continue
            self._last = cur
            self._start = self._clock().timestamp() - cur.pos
            if not self._changed(cur):
                continue
            try:
                await self._push(cur, tuple(self._remotes()))
            except Exception:
                _LOGGER.exception("now playing push failed")

    def _changed(self, cur: Playing) -> bool:
        last = self._sent
        same = last is not None and cur.sig == last.sig
        return not (
            same and (cur.flags != PLAYING or abs(self._start - self._sent_start) <= _SEEK_SLACK)
        )

    async def _push(self, cur: Playing, remotes: tuple[int, ...], force: bool = False) -> None:
        targets = [r for r in remotes if self._attached(r) and self.client.connected]
        if not targets:
            return
        if cur.cid and cur.picture and self._cover_for != cur.cid:
            try:
                url = cur.picture.startswith("https://")
                raw = await (self._fetch_url(cur.picture) if url else self._fetch(self.plex))
            except Exception as exc:  # noqa: BLE001  (the cover is optional: the text must still go out)
                raw, why = None, f"fetch error: {exc!r}"
            else:
                why = "fetch failed"
            if raw is not None:
                try:
                    data = await self.hass.async_add_executor_job(quantize_cover, raw)
                except Exception as exc:  # noqa: BLE001  (a bad image is no reason to lose the text)
                    why = f"not usable: {exc}"
                else:
                    self._cover_for, self._cover = (
                        cur.cid,
                        (cur.cid, base64.b64encode(data).decode()),
                    )
                    why = ""
            if why:
                self.cover_errors += 1
            if why and cur.cid not in self._warned:  # retried on the next push; logged once per cid
                self._warned.add(cur.cid)
                _LOGGER.warning("cover for %s: %s", "URL" if url else self.plex, why)
        if self._cover and self._cover[0] == cur.cid and (force or self._cover_sent != cur.cid):
            for r in targets:
                if await self._send(
                    CMD_COVER, {"remote": r, "cid": cur.cid, "data": self._cover[1]}
                ):
                    self._cover_sent = cur.cid
                    self.sent_covers += 1
                    _LOGGER.debug("cover %08x sent to remote %s", cur.cid, r)
        pos = cur.pos
        if force and cur.flags == PLAYING:  # the stored pos is old: run it on like the hub does
            pos = max(int(self._clock().timestamp() - self._start), 0)
        ok = False
        for r in targets:
            if await self._send(
                CMD_NOW_PLAYING,
                {
                    "remote": r,
                    "cid": cur.cid,
                    "flags": cur.flags,
                    "pos": min(pos, 65535),
                    "dur": min(cur.dur, 65535),
                    "t1": cur.t1,
                    "t2": cur.t2,
                },
            ):
                ok = True
                self.sent_now_playing += 1
                _LOGGER.debug(
                    "now_playing sent to remote %s: flags=%s pos=%s dur=%s t1=%r t2=%r",
                    r,
                    cur.flags,
                    pos,
                    cur.dur,
                    cur.t1,
                    cur.t2,
                )
        if ok and not force:  # a failed send leaves it unset: the next event tries again
            self._sent, self._sent_start = cur, self._start

    async def _send(self, command: str, args: dict[str, Any]) -> bool:
        try:
            await self.client.async_send(
                MsgType.COMMAND, {"device": HUB_DEVICE, "command": command, "args": args}
            )
        except (HarmonyCommandError, HarmonyConnectionError) as exc:
            self.failed_sends += 1
            _LOGGER.debug("%s not delivered: %s", command, exc)
            return False
        return True

    async def _fetch_https(self, url: str) -> bytes | None:
        """A session cover over HTTPS: bounded in time and size, images only."""
        from aiohttp import ClientTimeout
        from homeassistant.helpers.aiohttp_client import async_get_clientsession

        sess = async_get_clientsession(self.hass)
        async with sess.get(url, timeout=ClientTimeout(total=_COVER_TIMEOUT)) as resp:
            if resp.status != 200 or not resp.headers.get("Content-Type", "").startswith("image/"):
                return None
            body = bytearray()
            async for chunk in resp.content.iter_chunked(65536):
                body += chunk
                if len(body) > _COVER_MAX:
                    return None
        return bytes(body)

    async def _fetch_picture(self, entity_id: str) -> bytes | None:
        """The player's cover bytes, in process (no URL, token or TLS): the entity's own image getter."""
        comp = self.hass.data.get(DATA_COMPONENT)
        entity = comp.get_entity(entity_id) if comp is not None else None
        if entity is None:
            return None
        raw, _ctype = await entity.async_get_media_image()
        return raw
