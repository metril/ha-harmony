"""The add-on's single write into Home Assistant: fire one IR code through `ha_harmony.send_ir`.

This is deliberately a separate, tiny class from the read-only `ha_client.HttpHaApi`: its only
capability is the one hard-coded service path below. There is no way to name another path, method
or service, and the request body is built here from an already validated IR action.
"""

from __future__ import annotations

import logging
import os
import time
from collections import deque
from collections.abc import Callable
from typing import Any, Protocol

import aiohttp

from .ha_client import CORE_API_URL, HaUnavailable

log = logging.getLogger(__name__)

SERVICE_PATH = "/services/ha_harmony/send_ir"  # the only thing this module can call
MIN_INTERVAL = 0.5  # seconds between two sends
MAX_PER_MINUTE = 30


class IrSender(Protocol):
    async def send(self, hub: str, action: dict[str, Any]) -> None: ...


class SendRefused(Exception):
    """Home Assistant answered, but not with success (carries only the HTTP status)."""

    def __init__(self, status: int) -> None:
        super().__init__(f"Home Assistant answered {status}")
        self.status = status


class HttpIrSender:
    def __init__(self, token: str, url: str = CORE_API_URL) -> None:
        self._token, self._url = token, url

    def __repr__(self) -> str:  # the token must never reach a log line
        return f"HttpIrSender(url={self._url!r})"

    async def send(self, hub: str, action: dict[str, Any]) -> None:
        """`action` must already be validated by harmony_core.config_model.validate_ir."""
        timeout = aiohttp.ClientTimeout(total=10)
        headers = {"Authorization": f"Bearer {self._token}"}
        try:
            async with (
                aiohttp.ClientSession(timeout=timeout, headers=headers) as s,
                s.post(
                    self._url + SERVICE_PATH,
                    json={"config_entry": hub, "ir": action},
                    allow_redirects=False,
                ) as r,
            ):
                status = r.status
        except (aiohttp.ClientError, OSError, TimeoutError) as exc:
            raise HaUnavailable(type(exc).__name__) from None
        if status != 200:
            raise SendRefused(status)


def default_ir_sender() -> IrSender | None:
    token = os.environ.get("SUPERVISOR_TOKEN")
    if not token:
        log.info("SUPERVISOR_TOKEN is not set: IR test-fire is off")
        return None
    return HttpIrSender(token)


class SendLimiter:
    """At least MIN_INTERVAL between sends and at most MAX_PER_MINUTE in any 60 s, per instance."""

    def __init__(self, clock: Callable[[], float] = time.monotonic) -> None:
        self._clock = clock
        self._sent: deque[float] = deque()

    def acquire(self) -> int | None:
        """None = allowed (and counted); otherwise the milliseconds to wait."""
        now = self._clock()
        while self._sent and now - self._sent[0] >= 60.0:
            self._sent.popleft()
        wait = 0.0
        if self._sent:
            wait = max(wait, MIN_INTERVAL - (now - self._sent[-1]))
        if len(self._sent) >= MAX_PER_MINUTE:
            wait = max(wait, 60.0 - (now - self._sent[0]))
        if wait > 0:
            return max(1, int(wait * 1000) + 1)
        self._sent.append(now)
        return None
