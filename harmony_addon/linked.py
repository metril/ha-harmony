"""Hub ids the integration polls the add-on for (id -> last seen), in memory and in /data."""

from __future__ import annotations

import json
import logging
from collections.abc import Callable
from pathlib import Path

from .store import HUB_ID, atomic_write

log = logging.getLogger(__name__)
MAX_LINKED = 32
PERSIST_EVERY = 60  # seconds between rewrites for a hub that is already known


class LinkedHubs:
    def __init__(self, data_dir: Path, clock: Callable[[], float]) -> None:
        self._path = Path(data_dir) / "linked_hubs.json"
        self._clock = clock
        self._seen: dict[str, int] = {}
        self._persisted = 0
        try:
            raw = json.loads(self._path.read_bytes())
            for k, v in raw["hubs"].items():
                if HUB_ID.fullmatch(k) and type(v) is int and len(self._seen) < MAX_LINKED:
                    self._seen[k] = v
        except OSError, ValueError, KeyError, AttributeError, TypeError:
            pass  # absent or damaged: it is rebuilt from the next polls

    def ids(self) -> list[str]:
        return sorted(self._seen)

    def entries(self) -> list[dict[str, int | str]]:
        """Most recently seen first."""
        by_age = sorted(self._seen.items(), key=lambda kv: -kv[1])
        return [{"id": k, "last_seen": v} for k, v in by_age]

    def last_seen(self, hub: str) -> int | None:
        return self._seen.get(hub)

    def seen(self, hub: str) -> bool:
        """Record an authenticated bundle request for `hub`; True when the id is new."""
        if not HUB_ID.fullmatch(hub):
            return False
        new = hub not in self._seen
        if new and len(self._seen) >= MAX_LINKED:
            return False
        now = int(self._clock())
        self._seen[hub] = now
        if new or now - self._persisted >= PERSIST_EVERY:
            self._persist(now)
        return new

    def _persist(self, now: int) -> None:
        try:
            atomic_write(self._path, json.dumps({"v": 1, "hubs": self._seen}).encode())
            self._persisted = now
        except OSError:
            log.warning("could not save the linked hub ids")
