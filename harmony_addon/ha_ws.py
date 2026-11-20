"""Read-only Home Assistant registries over the Supervisor's WebSocket proxy.

The editor's entity navigator groups by area and device, which the REST API cannot tell it. The
registries are only available over the WebSocket API, so this module opens one connection per
refresh (the caller caches ~60 s), authenticates with SUPERVISOR_TOKEN and asks for the registry
lists. Safety by construction: `_send` refuses every command that is not in `ALLOWED_COMMANDS`, and
that set holds only `*/list` registry reads. Message size and total time are bounded.
"""

from __future__ import annotations

import asyncio
import os
from typing import Any, Protocol

import aiohttp

CORE_WS_URL = "ws://supervisor/core/websocket"
ALLOWED_COMMANDS = frozenset(
    {
        "config/area_registry/list",
        "config/device_registry/list",
        "config/entity_registry/list",
        "config/floor_registry/list",
        "config/label_registry/list",
    }
)
OPTIONAL_COMMANDS = frozenset(
    {"config/floor_registry/list", "config/label_registry/list"}
)  # a Core without floors or labels still works
MAX_MESSAGE = 8 * 1024 * 1024  # one registry list; a large install's entity registry is ~2 MB
TIMEOUT_SECONDS = 20  # whole conversation: connect, auth and every list


class RegistryUnavailable(Exception):
    """The registries cannot be read; `reason` is a short cause, never response contents."""

    def __init__(self, reason: str) -> None:
        super().__init__(reason)
        self.reason = reason


class HaRegistryApi(Protocol):
    async def lists(self) -> dict[str, list[dict[str, Any]]]: ...


class HaWsRegistry:
    """Fetches the allow-listed registry lists. No method takes a command from the caller."""

    def __init__(self, token: str, url: str = CORE_WS_URL) -> None:
        self._token, self._url = token, url

    def __repr__(self) -> str:  # the token must never reach a log line
        return f"HaWsRegistry(url={self._url!r})"

    @staticmethod
    async def _send(ws: aiohttp.ClientWebSocketResponse, msg_id: int, command: str) -> None:
        if command not in ALLOWED_COMMANDS:
            raise RegistryUnavailable("command_not_allowed")
        await ws.send_json({"id": msg_id, "type": command})

    @staticmethod
    async def _recv(ws: aiohttp.ClientWebSocketResponse) -> dict[str, Any]:
        msg = await ws.receive()
        if msg.type != aiohttp.WSMsgType.TEXT:
            raise RegistryUnavailable(
                "ws_closed" if msg.type != aiohttp.WSMsgType.ERROR else "ws_error"
            )
        try:
            data = msg.json()
        except ValueError:
            raise RegistryUnavailable("not_json") from None
        if not isinstance(data, dict):
            raise RegistryUnavailable("unexpected_shape")
        return data

    async def lists(self) -> dict[str, list[dict[str, Any]]]:
        try:
            async with asyncio.timeout(TIMEOUT_SECONDS):
                return await self._lists()
        except RegistryUnavailable:
            raise
        except (aiohttp.ClientError, OSError, TimeoutError) as exc:
            raise RegistryUnavailable(type(exc).__name__) from None

    async def _lists(self) -> dict[str, list[dict[str, Any]]]:
        out: dict[str, list[dict[str, Any]]] = {}
        async with (
            aiohttp.ClientSession() as session,
            session.ws_connect(self._url, max_msg_size=MAX_MESSAGE, heartbeat=None) as ws,
        ):
            first = await self._recv(ws)
            if first.get("type") != "auth_required":
                raise RegistryUnavailable("no_auth_prompt")
            await ws.send_json({"type": "auth", "access_token": self._token})
            if (await self._recv(ws)).get("type") != "auth_ok":
                raise RegistryUnavailable("auth_failed")
            for n, command in enumerate(sorted(ALLOWED_COMMANDS), start=1):
                await self._send(ws, n, command)
                while True:
                    reply = await self._recv(ws)
                    if reply.get("id") == n and reply.get("type") == "result":
                        break
                if reply.get("success") is not True or not isinstance(reply.get("result"), list):
                    if command in OPTIONAL_COMMANDS:
                        out[command] = []
                        continue
                    raise RegistryUnavailable("command_failed")
                out[command] = [r for r in reply["result"] if isinstance(r, dict)]
        return out


def default_ha_registry() -> HaRegistryApi | None:
    token = os.environ.get("SUPERVISOR_TOKEN")
    return HaWsRegistry(token) if token else None
