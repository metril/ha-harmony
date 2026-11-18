"""Tiny Supervisor client: announce this add-on to the Harmony integration (discovery).

Needs only the add-on's own SUPERVISOR_TOKEN and `discovery: [ha_harmony]` in config.yaml;
POST /discovery and /addons/self/* need no `hassio_api` (Supervisor API docs, "without any extra
permission"). Without SUPERVISOR_TOKEN (plain docker) it logs once and does nothing.
"""

from __future__ import annotations

import logging
import os
import socket
from typing import Any, Protocol

import aiohttp

log = logging.getLogger(__name__)

SERVICE = "ha_harmony"
SUPERVISOR_URL = "http://supervisor"


class Supervisor(Protocol):
    async def self_hostname(self) -> str | None: ...
    async def announce(self, config: dict[str, Any]) -> str | None: ...
    async def withdraw(self, uuid: str) -> None: ...


class HttpSupervisor:
    def __init__(self, token: str, url: str = SUPERVISOR_URL) -> None:
        self._token, self._url = token, url

    async def _call(self, method: str, path: str, **kw: Any) -> dict[str, Any]:
        timeout = aiohttp.ClientTimeout(total=10)
        headers = {"Authorization": f"Bearer {self._token}"}
        async with (
            aiohttp.ClientSession(timeout=timeout, headers=headers) as s,
            s.request(method, f"{self._url}{path}", **kw) as r,
        ):
            r.raise_for_status()
            body = await r.json()
        return body.get("data") or {}

    async def self_hostname(self) -> str | None:
        return (await self._call("GET", "/addons/self/info")).get("hostname")

    async def announce(self, config: dict[str, Any]) -> str | None:
        return (
            await self._call("POST", "/discovery", json={"service": SERVICE, "config": config})
        ).get("uuid")

    async def withdraw(self, uuid: str) -> None:
        await self._call("DELETE", f"/discovery/{uuid}")


def default_supervisor() -> Supervisor | None:
    token = os.environ.get("SUPERVISOR_TOKEN")
    if not token:
        log.info("SUPERVISOR_TOKEN is not set: not announcing to Home Assistant (discovery off)")
        return None
    return HttpSupervisor(token)


class Discovery:
    """announce() at start, withdraw() at stop. Failures are logged, never raised."""

    def __init__(self, supervisor: Supervisor | None, port: int, token: str) -> None:
        self._sup, self._port, self._token = supervisor, port, token
        self._uuid: str | None = None

    async def announce(self) -> None:
        if self._sup is None:
            return
        try:
            host = await self._sup.self_hostname() or socket.gethostname()
        except aiohttp.ClientError, OSError, TimeoutError, ValueError:
            host = socket.gethostname()
        try:
            self._uuid = await self._sup.announce(
                {"host": host, "port": self._port, "token": self._token}
            )
        except (aiohttp.ClientError, OSError, TimeoutError, ValueError) as exc:
            log.warning("could not announce to Home Assistant: %s", type(exc).__name__)
            return
        log.info("announced to Home Assistant as %s:%s", host, self._port)

    async def withdraw(self) -> None:
        uuid, self._uuid = self._uuid, None
        if self._sup is None or uuid is None:
            return
        try:
            await self._sup.withdraw(uuid)
        except (aiohttp.ClientError, OSError, TimeoutError, ValueError) as exc:
            log.warning("could not withdraw the discovery message: %s", type(exc).__name__)
