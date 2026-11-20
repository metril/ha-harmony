"""Per-hub pub/sub. One Bus instance lives on the app; no module-level state."""

from __future__ import annotations

import asyncio
from collections import deque
from collections.abc import Iterable
from contextlib import asynccontextmanager

QUEUE_MAX = 256


class Subscription:
    """Bounded queue; on overflow the oldest message is dropped and `resync` is set."""

    def __init__(self, hub: str, topics: frozenset[str], maxlen: int = QUEUE_MAX) -> None:
        self.hub = hub
        self.topics = topics
        self._maxlen = maxlen
        self._q: deque[tuple[str, object]] = deque()
        self._ev = asyncio.Event()
        self.resync = False

    def _put(self, topic: str, payload: object) -> None:
        if len(self._q) >= self._maxlen:
            self._q.popleft()
            self.resync = True
        self._q.append((topic, payload))
        self._ev.set()

    def take_resync(self) -> bool:
        """True once after an overflow: the consumer must re-send a snapshot."""
        r, self.resync = self.resync, False
        return r

    async def get(self) -> tuple[str, object]:
        while not self._q:
            self._ev.clear()
            await self._ev.wait()
        return self._q.popleft()

    def __aiter__(self) -> Subscription:
        return self

    async def __anext__(self) -> tuple[str, object]:
        return await self.get()


class Bus:
    def __init__(self) -> None:
        self._subs: dict[str, set[Subscription]] = {}

    def publish(self, hub: str, topic: str, payload: object) -> None:
        for s in tuple(self._subs.get(hub, ())):
            if topic in s.topics:
                s._put(topic, payload)

    def subscribers(self, hub: str, topic: str) -> int:
        return sum(topic in s.topics for s in self._subs.get(hub, ()))

    @asynccontextmanager
    async def subscribe(self, hub: str, topics: Iterable[str], maxlen: int = QUEUE_MAX):
        sub = Subscription(hub, frozenset(topics), maxlen)
        self._subs.setdefault(hub, set()).add(sub)
        try:
            yield sub
        finally:
            subs = self._subs.get(hub)
            if subs is not None:
                subs.discard(sub)
                if not subs:
                    del self._subs[hub]
