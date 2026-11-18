"""Client for the Harmony Editor add-on's bundle feed. The add-on is never trusted.

Everything pulled is size-capped and time-bounded, then re-validated and recompiled here with the
shared `harmony_core` (no IR file references: `base_dir=None`) before anything is used.
"""

from __future__ import annotations

import json
import re
from dataclasses import dataclass
from typing import Any

import aiohttp
from homeassistant.exceptions import HomeAssistantError

from . import const
from .harmony_core import BUNDLE_SCHEMA
from .harmony_core.compiler import Compiled, CompileError, compile_config
from .harmony_core.config_model import ConfigError, parse_model

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


def compile_source(source: Any, hub_id: str) -> Compiled:
    """Validate (no file refs) and compile a source config. Raises BundleRejected."""
    if not isinstance(source, dict):
        raise BundleRejected("config is not an object")
    try:
        text = json.dumps(source, allow_nan=False)
        model = parse_model(text, f"bundle[{hub_id}]", None)
        return compile_config(model)
    except (ConfigError, CompileError) as exc:
        raise BundleRejected(str(exc)[:300]) from None
    except ValueError, TypeError, RecursionError:
        raise BundleRejected("config is not valid JSON data") from None


def parse_bundle(raw: bytes, hub_id: str) -> Bundle:
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
        compiled = compile_source(data["config"], hub_id)
    except BundleRejected as exc:
        raise BundleRejected(str(exc), rev=rev) from None
    return Bundle(rev, applied_at, data["config"], compiled)


class AddonClient:
    def __init__(self, session: aiohttp.ClientSession, host: str, port: int, token: str) -> None:
        self._session = session
        self._base = f"http://{host}:{port}/api/v1/bundle"
        self._headers = {"Authorization": f"Bearer {token}"}

    async def _get(self, path: str, params: dict[str, str], wait: float) -> bytes | None:
        timeout = aiohttp.ClientTimeout(total=wait, sock_connect=5, sock_read=wait)
        try:
            async with self._session.get(
                self._base + path,
                params=params,
                headers=self._headers,
                timeout=timeout,
                allow_redirects=False,
            ) as resp:
                if resp.status in (204, 304):
                    return None
                if resp.status in (401, 403, 429):
                    raise AddonAuthError("the add-on refused the token")
                if resp.status == 404:
                    raise NothingApplied("no config has been applied in the add-on yet")
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
