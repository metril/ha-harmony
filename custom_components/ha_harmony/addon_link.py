"""Client for the Harmony Editor add-on's channel (docs/addon-api.md "Channel"). The add-on is never trusted.

One WebSocket per hub carries everything both ways; nothing polls. Every bundle is size-capped, then re-validated and
recompiled here with the shared `harmony_core` (no IR file references: `base_dir=None`) before anything is used.
Liveness is TCP keepalive on the socket (no aiohttp heartbeat, no application ping).
"""

from __future__ import annotations

import asyncio
import json
import re
from collections.abc import AsyncIterator, Callable, Mapping, Sequence
from dataclasses import dataclass
from typing import Any

import aiohttp
from homeassistant.exceptions import HomeAssistantError

from . import const
from .harmony_core import BUNDLE_SCHEMA
from .harmony_core.compiler import Compiled, CompileError, compile_config
from .harmony_core.config_model import ConfigError, parse_data
from .harmony_core.hub_limits import CURRENT, HubLimits
from .sockopts import keepalive_ws

CHANNEL_VERSION = 1
CLOSE_UNSUPPORTED_VERSION = 4001
CLOSE_TOO_BIG = 1009
ENVELOPE_KEYS = frozenset({"schema", "core_version", "hub_id", "applied_at", "rev", "config"})
_REV = re.compile(r"^[A-Za-z0-9._-]{1,64}$")


class AddonError(HomeAssistantError):
    """Base for add-on feed problems."""


class AddonUnreachable(AddonError):
    pass


class AddonAuthError(AddonError):
    pass


class AddonRejected(AddonError):
    """The add-on understood a request and refused its content (HTTP 400): resending it cannot help."""


class NothingApplied(AddonError):
    pass


class AddonOutdated(AddonError):
    """The add-on has no /channel (404): it must be updated; there is no fallback."""


class BundleRejected(AddonError):
    """The bundle is unusable. `rev` (when readable) lets the caller wait for the next one;
    `schema` marks a version mismatch (a different repair than a bad config)."""

    def __init__(self, message: str, *, rev: str | None = None, schema: bool = False) -> None:
        super().__init__(message)
        self.rev, self.schema = rev, schema


@dataclass(frozen=True)
class Bundle:
    rev: str
    applied_at: int
    source: dict[str, Any]  # the applied source config, as validated
    compiled: Compiled


def compile_source(
    source: Any,
    hub_id: str,
    limits: HubLimits | None = None,
    source_lists: Mapping[str, Sequence[str]] | None = None,
) -> Compiled:
    """Validate (no file refs) and compile a source config against the hub's limits (the newest
    firmware's until the hub has reported them). Raises BundleRejected."""
    if not isinstance(source, dict):
        raise BundleRejected("config is not an object")
    try:
        json.dumps(
            source, allow_nan=False
        )  # JSON data only (the add-on's own checks use the same dict path)
        model = parse_data(
            source
        )  # not the YAML pass: ~50x faster, which matters for thousands of commands
        return compile_config(model, limits=limits or CURRENT, source_lists=source_lists)
    except (ConfigError, CompileError) as exc:
        raise BundleRejected(str(exc)[:300]) from None
    except ValueError, TypeError, RecursionError:
        raise BundleRejected("config is not valid JSON data") from None


def parse_bundle(
    raw: bytes | dict[str, Any],
    hub_id: str,
    limits: HubLimits | None = None,
    source_lists: Mapping[str, Sequence[str]] | None = None,
) -> Bundle:
    """Pure: the bundle (bytes, or the channel's already decoded `body`) -> validated Bundle, or BundleRejected.
    Blocking (CPU)."""
    if isinstance(raw, dict):
        data: Any = raw
    else:
        if len(raw) > const.BUNDLE_MAX_BYTES:
            raise BundleRejected("bundle is too large")
        try:
            data = json.loads(raw)
        except ValueError, RecursionError:
            raise BundleRejected("bundle is not valid JSON") from None
    if not isinstance(data, dict):
        raise BundleRejected("bundle is not an object")
    rev = data.get("rev")
    rev = rev if isinstance(rev, str) and _REV.fullmatch(rev) else None
    schema = data.get("schema")
    if type(schema) is not int or schema != BUNDLE_SCHEMA:
        raise BundleRejected(
            f"bundle schema {str(schema)[:12]} is not supported (this integration uses "
            f"{BUNDLE_SCHEMA}); update the add-on or the integration",
            rev=rev,
            schema=True,
        )
    if set(data) != ENVELOPE_KEYS:
        raise BundleRejected("bundle has unexpected or missing keys", rev=rev)
    applied_at = data["applied_at"]
    if rev is None or type(applied_at) is not int or not isinstance(data["core_version"], str):
        raise BundleRejected("bundle header is malformed", rev=rev)
    if data["hub_id"] != hub_id:
        raise BundleRejected("bundle is for a different hub", rev=rev)
    try:
        compiled = compile_source(data["config"], hub_id, limits, source_lists)
    except BundleRejected as exc:
        raise BundleRejected(str(exc), rev=rev) from None
    return Bundle(rev, applied_at, data["config"], compiled)


REMOTES_MAX_BYTES = 4000


def clean_remotes(remotes: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """The `remotes` field: at most 8 remotes, 8 notices of 200 chars each, notices dropped from the end until the
    JSON fits REMOTES_MAX_BYTES."""
    rows = [{**r, "notices": [str(m)[:200] for m in r.get("notices", [])][:8]} for r in remotes[:8]]
    while True:
        if len(json.dumps(rows, separators=(",", ":"))) <= REMOTES_MAX_BYTES:
            return rows
        victim = max(rows, key=lambda r: len(r["notices"]))
        if not victim["notices"]:
            return []
        victim["notices"].pop()


class AddonChannel:
    """One open channel socket: `send` frames, iterate the add-on's frames. Iteration ends when the socket closes
    (`close_code` says how); a frame that is not a JSON object with a string `t` is skipped."""

    def __init__(self, ws: aiohttp.ClientWebSocketResponse) -> None:
        self._ws = ws

    @property
    def close_code(self) -> int | None:
        return self._ws.close_code

    async def send(self, t: str, **fields: Any) -> None:
        try:
            await self._ws.send_str(
                json.dumps({"t": t, **fields}, separators=(",", ":"), allow_nan=False)
            )
        except (aiohttp.ClientError, OSError, TimeoutError, ConnectionError, ValueError) as exc:
            raise AddonUnreachable(f"cannot reach the add-on ({type(exc).__name__})") from None

    async def close(self) -> None:
        try:
            await self._ws.close()
        except aiohttp.ClientError, OSError, TimeoutError:
            pass

    def __aiter__(self) -> AsyncIterator[dict[str, Any]]:
        return self._frames()

    async def _frames(self) -> AsyncIterator[dict[str, Any]]:
        try:
            async for msg in self._ws:
                if msg.type is not aiohttp.WSMsgType.TEXT:
                    continue
                try:
                    data = json.loads(msg.data)
                except ValueError, RecursionError:
                    continue
                if isinstance(data, dict) and isinstance(data.get("t"), str):
                    yield data
        except aiohttp.ClientError, OSError, TimeoutError:
            return


class AddonClient:
    def __init__(self, session: aiohttp.ClientSession, host: str, port: int, token: str) -> None:
        self._session = session
        self._url = f"http://{host}:{port}/api/v1/channel"
        self._headers = {"Authorization": f"Bearer {token}"}
        # the connected hub's limits (hello_ack.limits), sent in `hello` so the editor shows ITS caps
        self.limits_provider: Callable[[], HubLimits | None] | None = None
        # the linked remotes (caps tier, theme notices), sent in `hello` so the editor can say what a remote cannot show
        self.remotes_provider: Callable[[], list[dict[str, Any]]] | None = None
        # which hub this is (docs/addon-api.md "Renamed hubs"): the stable hardware id, the previous id after a
        # rename and the display name. Empty = send none.
        self.identity: dict[str, str] = {}
        self.last_adopt: str | None = None  # the add-on's `adopt` answer: moved | exists | none

    def identity_fields(self) -> dict[str, Any]:
        """hw / prev / name / remotes / limits, as `hello` and `identity` carry them."""
        out: dict[str, Any] = {
            k: v for k in ("hw", "prev", "name") if (v := self.identity.get(k)) is not None
        }
        lim = self.limits_provider() if self.limits_provider else None
        if lim is not None:
            out["limits"] = lim.to_wire()
        rem = self.remotes_provider() if self.remotes_provider else None
        if rem is not None:  # an empty list too: it clears what the add-on kept
            out["remotes"] = clean_remotes(rem)
        return out

    async def connect(
        self, hub_id: str, known_rev: str | None, *, hello: bool = True
    ) -> AddonChannel:
        """Open the channel and (by default) send `hello`. Raises AddonAuthError, AddonOutdated (404) or AddonUnreachable."""
        try:
            async with asyncio.timeout(const.LINK_CONNECT_TIMEOUT):
                ws = await self._session.ws_connect(
                    self._url,
                    params={"hub": hub_id},
                    headers=self._headers,
                    heartbeat=None,  # liveness is TCP keepalive
                    autoping=True,
                    max_msg_size=const.BUNDLE_MAX_BYTES + 65536,
                    timeout=aiohttp.ClientWSTimeout(ws_close=5),
                )
        except aiohttp.WSServerHandshakeError as exc:
            if exc.status in (401, 403):
                raise AddonAuthError("the add-on refused the token") from None
            if exc.status == 404:
                raise AddonOutdated("the add-on has no channel; update it") from None
            if exc.status == 429:  # throttled by the add-on: back off, the token may be fine
                raise AddonUnreachable("the add-on is rate limiting requests") from None
            raise AddonUnreachable(f"the add-on answered HTTP {exc.status}") from None
        except (aiohttp.ClientError, TimeoutError, OSError) as exc:
            raise AddonUnreachable(f"cannot reach the add-on ({type(exc).__name__})") from None
        keepalive_ws(ws)
        channel = AddonChannel(ws)
        if hello:
            try:
                await channel.send(
                    "hello",
                    v=CHANNEL_VERSION,
                    hub=hub_id,
                    known_rev=known_rev,
                    **self.identity_fields(),
                )
            except BaseException:
                await channel.close()
                raise
        return channel

    async def probe(self, hub_id: str) -> None:
        """Does the add-on answer at this address and token? (config flow). No hello: it is closed at once."""
        channel = await self.connect(hub_id, None, hello=False)
        await channel.close()

    async def hello_once(self, hub_id: str) -> None:
        """Send one hello carrying the identity and read the add-on's answer, to learn its `adopt` verdict
        (`last_adopt`; stays None when none came). Used by a rename, which runs without a live link."""
        self.last_adopt = None
        channel = await self.connect(hub_id, None)
        try:
            async with asyncio.timeout(const.LINK_CONNECT_TIMEOUT):
                async for msg in channel:
                    t = msg.get("t")
                    if t == "adopt":
                        res = msg.get("result")
                        self.last_adopt = res if isinstance(res, str) else None
                        return
                    if t in (
                        "error",
                        "bundle",
                        "nothing_applied",
                    ):  # `adopt`, when sent, comes before these
                        return
        except TimeoutError:
            raise AddonUnreachable("the add-on did not answer") from None
        finally:
            await channel.close()
