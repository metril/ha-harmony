"""Client for the Harmony Editor add-on's bundle feed. The add-on is never trusted.

Everything pulled is size-capped and time-bounded, then re-validated and recompiled here with the
shared `harmony_core` (no IR file references: `base_dir=None`) before anything is used.
"""

from __future__ import annotations

import json
import re
from collections.abc import Callable
from dataclasses import dataclass
from typing import Any
from urllib.parse import quote

import aiohttp
from homeassistant.exceptions import HomeAssistantError

from . import const
from .harmony_core import BUNDLE_SCHEMA
from .harmony_core.compiler import Compiled, CompileError, compile_config
from .harmony_core.config_model import ConfigError, parse_data
from .harmony_core.hub_limits import CURRENT, LIMITS_HEADER, HubLimits

HW_HEADER = "X-Hub-Hw"  # the hub's stable hardware id
PREV_HEADER = "X-Hub-Prev"  # the id the hub had before a rename
NAME_HEADER = "X-Hub-Name"  # display name, percent-encoded UTF-8
REMOTES_HEADER = "X-Hub-Remotes"  # JSON: the linked remotes' caps tier and current theme notices (docs/addon-api.md)
ADOPT_HEADER = "X-HH-Adopt"  # the add-on's answer to a rename: moved | exists | none
ENVELOPE_KEYS = frozenset({"schema", "core_version", "hub_id", "applied_at", "rev", "config"})
_REV = re.compile(r"^[A-Za-z0-9._-]{1,64}$")


class AddonError(HomeAssistantError):
    """Base for add-on feed problems."""


class AddonUnreachable(AddonError):
    pass


class AddonAuthError(AddonError):
    pass


class NothingApplied(AddonError):
    pass


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


def compile_source(source: Any, hub_id: str, limits: HubLimits | None = None) -> Compiled:
    """Validate (no file refs) and compile a source config against the hub's limits (the newest
    firmware's until the hub has reported them). Raises BundleRejected."""
    if not isinstance(source, dict):
        raise BundleRejected("config is not an object")
    try:
        json.dumps(source, allow_nan=False)  # JSON data only (the add-on's own checks use the same dict path)
        model = parse_data(source)  # not the YAML pass: ~50x faster, which matters for thousands of commands
        return compile_config(model, limits=limits or CURRENT)
    except (ConfigError, CompileError) as exc:
        raise BundleRejected(str(exc)[:300]) from None
    except ValueError, TypeError, RecursionError:
        raise BundleRejected("config is not valid JSON data") from None


def parse_bundle(raw: bytes, hub_id: str, limits: HubLimits | None = None) -> Bundle:
    """Pure: bytes from the add-on -> validated Bundle, or BundleRejected. Blocking (CPU)."""
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
        compiled = compile_source(data["config"], hub_id, limits)
    except BundleRejected as exc:
        raise BundleRejected(str(exc), rev=rev) from None
    return Bundle(rev, applied_at, data["config"], compiled)


REMOTES_HEADER_MAX = 4000


def encode_remotes(remotes: list[dict[str, Any]]) -> str:
    """The header value: compact ASCII JSON, notices dropped from the end until it fits."""
    rows = [{**r, "notices": [str(m)[:200] for m in r.get("notices", [])][:8]} for r in remotes[:8]]
    while True:
        out = json.dumps(rows, separators=(",", ":"))
        if len(out) <= REMOTES_HEADER_MAX:
            return out
        victim = max(rows, key=lambda r: len(r["notices"]))
        if not victim["notices"]:
            return "[]"
        victim["notices"].pop()


class AddonClient:
    def __init__(self, session: aiohttp.ClientSession, host: str, port: int, token: str) -> None:
        self._session = session
        self._base = f"http://{host}:{port}/api/v1/bundle"
        self._headers = {"Authorization": f"Bearer {token}"}
        # the connected hub's limits, sent with every poll so the editor shows ITS caps (an add-on
        # that does not know the header ignores it)
        self.limits_provider: Callable[[], HubLimits | None] | None = None
        # the linked remotes (caps tier, theme notices), sent with every poll so the editor can say what a remote cannot show
        self.remotes_provider: Callable[[], list[dict[str, Any]]] | None = None
        # which hub this is, sent with every poll (docs/addon-api.md "Renamed hubs"): the stable hardware id, the
        # previous id after a rename and the display name. Empty = send none (an old add-on ignores them anyway).
        self.identity: dict[str, str] = {}
        self.last_adopt: str | None = None  # the add-on's X-HH-Adopt answer to the last poll: moved | exists | none

    def _poll_headers(self) -> dict[str, str]:
        headers = dict(self._headers)
        if (hw := self.identity.get("hw")) is not None:
            headers[HW_HEADER] = hw
        if (prev := self.identity.get("prev")) is not None:
            headers[PREV_HEADER] = prev
        if (name := self.identity.get("name")) is not None:
            headers[NAME_HEADER] = quote(name, safe="")  # HTTP headers are not UTF-8: percent-encoded
        lim = self.limits_provider() if self.limits_provider else None
        if lim is not None:
            headers[LIMITS_HEADER] = json.dumps(lim.to_wire(), separators=(",", ":"))
        rem = self.remotes_provider() if self.remotes_provider else None
        if rem is not None:  # an empty list too: it clears what the add-on kept
            headers[REMOTES_HEADER] = encode_remotes(rem)
        return headers

    async def _get(self, path: str, params: dict[str, str], wait: float) -> bytes | None:
        timeout = aiohttp.ClientTimeout(total=wait, sock_connect=5, sock_read=wait)
        try:
            async with self._session.get(
                self._base + path,
                params=params,
                headers=self._poll_headers(),
                timeout=timeout,
                allow_redirects=False,
            ) as resp:
                self.last_adopt = resp.headers.get(ADOPT_HEADER)
                if resp.status in (204, 304):
                    return None
                if resp.status == 429:  # throttled by the add-on: back off, the token may be fine
                    raise AddonUnreachable("the add-on is rate limiting requests")
                if resp.status in (401, 403):
                    raise AddonAuthError("the add-on refused the token")
                if resp.status == 404:
                    raise NothingApplied(f"no config has been applied for hub {params.get('hub', '?')} in the add-on")
                if resp.status != 200:
                    raise AddonUnreachable(f"the add-on answered HTTP {resp.status}")
                if (resp.content_length or 0) > const.BUNDLE_MAX_BYTES:
                    raise BundleRejected("bundle is too large")
                body = bytearray()
                async for chunk in resp.content.iter_chunked(65536):
                    body += chunk
                    if len(body) > const.BUNDLE_MAX_BYTES:
                        raise BundleRejected("bundle is too large")
                return bytes(body)
        except (aiohttp.ClientError, TimeoutError, OSError) as exc:
            raise AddonUnreachable(f"cannot reach the add-on ({type(exc).__name__})") from None

    async def fetch(self, hub_id: str) -> bytes:
        """Raw bundle bytes; parse them with `parse_bundle` (CPU-bound: use an executor)."""
        raw = await self._get("", {"hub": hub_id}, const.LINK_FETCH_TIMEOUT)
        if raw is None:
            raise AddonUnreachable("the add-on sent an empty answer")
        return raw

    async def wait(self, hub_id: str, rev: str) -> bytes | None:
        """Long-poll: bytes of a newer bundle, or None when the add-on's wait ran out."""
        timeout = const.LINK_WAIT_TIMEOUT
        return await self._get("/wait", {"hub": hub_id, "rev": rev}, timeout)

    async def ack(self, hub_id: str, rev: str, result: str, detail: str | None = None) -> None:
        """Tell the add-on what became of a pulled bundle. Raises AddonError; callers ignore it."""
        body: dict[str, str] = {"hub": hub_id, "rev": rev, "result": result}
        if detail:
            body["detail"] = detail[:200]
        timeout = aiohttp.ClientTimeout(total=10, sock_connect=5)
        try:
            async with self._session.post(
                self._base + "/ack",
                json=body,
                headers=self._headers,
                timeout=timeout,
                allow_redirects=False,
            ) as resp:
                if resp.status in (401, 403):
                    raise AddonAuthError("the add-on refused the token")
                if resp.status != 200:
                    raise AddonUnreachable(f"the add-on answered HTTP {resp.status}")
        except (aiohttp.ClientError, TimeoutError, OSError) as exc:
            raise AddonUnreachable(f"cannot reach the add-on ({type(exc).__name__})") from None
