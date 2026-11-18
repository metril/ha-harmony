"""Read-only view of Home Assistant for the editor's pickers.

The add-on has `homeassistant_api: true`, so its SUPERVISOR_TOKEN can call the Core API through the
Supervisor proxy. This module only ever issues GET requests (`/api/states`, `/api/services`); it
never calls a service. Without SUPERVISOR_TOKEN (plain docker) there is no client and the routes
answer 503, so the UI falls back to typed entity ids.
"""

from __future__ import annotations

import json
import logging
import os
from collections.abc import Callable
from typing import Any, Protocol

import aiohttp

log = logging.getLogger(__name__)

CORE_API_URL = "http://supervisor/core/api"
MAX_BODY = 8 * 1024 * 1024  # a large install's /api/states is a few MB
MAX_ENTITIES = 5000
MAX_STR = 200
CACHE_SECONDS = 5.0

ACTION_DOMAINS = frozenset(
    {
        "light", "switch", "scene", "script", "media_player", "remote", "fan", "cover",
        "climate", "input_boolean", "input_button", "button", "number", "select",
    }
)  # fmt: skip
ENTITY_DOMAINS = ACTION_DOMAINS | {"sensor", "binary_sensor"}


class HaUnavailable(Exception):
    """Home Assistant cannot be read right now (no token, network error, bad answer)."""


class HaApi(Protocol):
    async def get(self, path: str) -> Any: ...


class HttpHaApi:
    """GET-only client for the Core API. There is deliberately no method that sends a body."""

    def __init__(self, token: str, url: str = CORE_API_URL) -> None:
        self._token, self._url = token, url

    def __repr__(self) -> str:  # the token must never reach a log line
        return f"HttpHaApi(url={self._url!r})"

    async def get(self, path: str) -> Any:
        timeout = aiohttp.ClientTimeout(total=10)
        headers = {"Authorization": f"Bearer {self._token}"}
        try:
            async with (
                aiohttp.ClientSession(timeout=timeout, headers=headers) as s,
                s.get(f"{self._url}{path}", allow_redirects=False) as r,
            ):
                if r.status != 200:
                    raise HaUnavailable(f"Home Assistant answered {r.status}")
                raw = await r.content.read(MAX_BODY + 1)
                if len(raw) > MAX_BODY:
                    raise HaUnavailable("Home Assistant's answer is too large")
                return json.loads(raw)
        except (aiohttp.ClientError, OSError, TimeoutError, ValueError) as exc:
            raise HaUnavailable(type(exc).__name__) from None


def default_ha_api() -> HaApi | None:
    token = os.environ.get("SUPERVISOR_TOKEN")
    if not token:
        log.info("SUPERVISOR_TOKEN is not set: Home Assistant entity pickers are off")
        return None
    return HttpHaApi(token)


def _text(v: Any) -> str | None:
    return v[:MAX_STR] if isinstance(v, str) and v else None


class Catalog:
    """Entities and services, filtered to the allow-listed domains and cached for a few seconds."""

    def __init__(self, api: HaApi | None, clock: Callable[[], float]) -> None:
        self._api, self._clock = api, clock
        self._cache: dict[str, tuple[float, Any]] = {}

    async def _get(self, path: str) -> Any:
        if self._api is None:
            raise HaUnavailable("no Home Assistant access")
        now = self._clock()
        hit = self._cache.get(path)
        if hit and now - hit[0] < CACHE_SECONDS:
            return hit[1]
        data = await self._api.get(path)
        self._cache[path] = (now, data)
        return data

    async def entities(self, domain: str | None, q: str | None) -> list[dict[str, Any]]:
        states = await self._get("/states")
        if not isinstance(states, list):
            raise HaUnavailable("unexpected answer from Home Assistant")
        needle = (q or "").casefold()
        out: list[dict[str, Any]] = []
        for st in states:
            if not isinstance(st, dict):
                continue
            eid = st.get("entity_id")
            if not isinstance(eid, str) or "." not in eid or len(eid) > MAX_STR:
                continue
            dom = eid.split(".", 1)[0]
            if dom not in ENTITY_DOMAINS or (domain and dom != domain):
                continue
            attrs = st.get("attributes") if isinstance(st.get("attributes"), dict) else {}
            name = _text(attrs.get("friendly_name")) or eid
            if needle and needle not in eid.casefold() and needle not in name.casefold():
                continue
            row: dict[str, Any] = {"entity_id": eid, "name": name, "domain": dom}
            for key, val in (
                ("state", _text(st.get("state"))),
                ("device_class", _text(attrs.get("device_class"))),
            ):
                if val is not None:
                    row[key] = val
            out.append(row)
            if len(out) >= MAX_ENTITIES:
                break
        out.sort(key=lambda r: (r["domain"], r["name"].casefold()))
        return out

    async def services(self, domain: str | None) -> list[dict[str, Any]]:
        data = await self._get("/services")
        if not isinstance(data, list):
            raise HaUnavailable("unexpected answer from Home Assistant")
        out: list[dict[str, Any]] = []
        for entry in data:
            if not isinstance(entry, dict):
                continue
            dom = entry.get("domain")
            if dom not in ACTION_DOMAINS or (domain and dom != domain):
                continue
            services = entry.get("services")
            if not isinstance(services, dict):
                continue
            for name, spec in sorted(services.items()):
                if not isinstance(name, str) or len(name) > MAX_STR:
                    continue
                spec = spec if isinstance(spec, dict) else {}
                fields = spec.get("fields") if isinstance(spec.get("fields"), dict) else {}
                out.append(
                    {
                        "domain": dom,
                        "service": name,
                        "name": _text(spec.get("name")) or name,
                        "fields": sorted(k for k in fields if isinstance(k, str))[:64],
                    }
                )
        return out
