"""Hub ids the integration polls the add-on for (id -> last seen), in memory and in /data."""

from __future__ import annotations

import json
import logging
import re
from collections.abc import Callable
from pathlib import Path
from typing import Any

from harmony_core.hub_limits import LEGACY, HubLimits

from .store import HUB_ID, atomic_write

log = logging.getLogger(__name__)
MAX_LINKED = 32
VERSION = 2
HW_ID = re.compile(r"[A-Za-z0-9._:-]{1,64}")  # a hub's stable hardware id (hello_ack.hw_id)
NAME_MAX_BYTES = 48  # a hub's display name


def clean_name(raw: object) -> str | None:
    """A display name as sent by an integration (already decoded), or None when it is not acceptable."""
    if not isinstance(raw, str) or len(raw.encode()) > NAME_MAX_BYTES:
        return None
    return None if any(ord(c) < 0x20 or ord(c) == 0x7F for c in raw) else raw


PERSIST_EVERY = 60  # seconds between rewrites for a hub that is already known


class LinkedHubs:
    def __init__(self, data_dir: Path, clock: Callable[[], float]) -> None:
        self._path = Path(data_dir) / "linked_hubs.json"
        self._clock = clock
        self._seen: dict[str, int] = {}
        self._limits: dict[str, dict[str, int]] = {}  # hub -> the limits its integration reported (wire form; {} = legacy)
        self._hw: dict[str, str] = {}  # hub -> its stable hardware id, as its integration reported it
        self._names: dict[str, str] = {}  # hub -> display name (absent = none)
        self._persisted = 0
        try:
            raw = json.loads(self._path.read_bytes())
            if raw["v"] != VERSION:  # v1 could hold a `default` that only an ack had put there
                raise ValueError("old file")
            for k, v in raw["hubs"].items():
                if HUB_ID.fullmatch(k) and type(v) is int and len(self._seen) < MAX_LINKED:
                    self._seen[k] = v
            for k, v in raw.get("limits", {}).items():
                if k in self._seen and isinstance(v, dict):
                    self._limits[k] = v if HubLimits.from_hello(v) is not LEGACY else {}
            for k, v in raw.get("hw", {}).items():
                if k in self._seen and isinstance(v, str) and HW_ID.fullmatch(v):
                    self._hw[k] = v
            for k, v in raw.get("names", {}).items():
                if k in self._seen and clean_name(v):
                    self._names[k] = v
        except OSError, ValueError, KeyError, AttributeError, TypeError:
            pass  # absent or damaged: it is rebuilt from the next polls

    def ids(self) -> list[str]:
        return sorted(self._seen)

    def entries(self) -> list[dict[str, int | str]]:
        """Most recently seen first."""
        by_age = sorted(self._seen.items(), key=lambda kv: -kv[1])
        return [
            {"id": k, "last_seen": v, **({"name": self._names[k]} if k in self._names else {})}
            for k, v in by_age
        ]

    def hw(self, hub: str) -> str | None:
        return self._hw.get(hub)

    def name(self, hub: str) -> str | None:
        return self._names.get(hub)

    def note_identity(self, hub: str, hw: str | None, name: str | None) -> None:
        """Record what the integration said about `hub` (hardware id, display name; None = not said)."""
        if hub not in self._seen:
            return
        changed = False
        if hw is not None and HW_ID.fullmatch(hw) and self._hw.get(hub) != hw:
            self._hw[hub] = hw
            changed = True
        if name is not None and clean_name(name) is not None:
            if name and self._names.get(hub) != name:
                self._names[hub] = name
                changed = True
            elif not name and hub in self._names:
                del self._names[hub]
                changed = True
        if changed:
            self._persist(int(self._clock()))

    def rename(self, old: str, new: str) -> None:
        """The hub polled for as `old` is now polled for as `new` (its stores have moved): carry its
        identity over and forget the old id. A no-op unless `old` is a known id."""
        if old == new or old not in self._seen:
            return
        self._seen[new] = max(self._seen.pop(old), self._seen.get(new, 0))
        for table in (self._hw, self._names, self._limits):
            if old in table:
                table.setdefault(new, table[old])
                del table[old]
        self._persist(int(self._clock()))

    def last_seen(self, hub: str) -> int | None:
        return self._seen.get(hub)

    def limits(self, hub: str) -> HubLimits:
        """The connected hub's config limits as its integration last reported them; LEGACY (the
        fixed limits of old firmware) when it never did."""
        return HubLimits.from_hello(self._limits.get(hub))

    def set_limits(self, hub: str, wire: Any) -> None:
        """Record what an integration reported for `hub` (a hello_ack.limits object; {} = old firmware)."""
        if hub not in self._seen or not isinstance(wire, dict):
            return
        lim = HubLimits.from_hello(wire)
        clean = {} if lim.legacy else {k: v for k, v in lim.to_wire().items() if k != "flash_free"}  # free flash moves: only the integration checks it, at push time; "none left" (flash_low) is passed on so the editor can say so
        if self._limits.get(hub) != clean:
            self._limits[hub] = clean
            self._persist(int(self._clock()))

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
            atomic_write(self._path, json.dumps(
                    {"v": VERSION, "hubs": self._seen, "limits": self._limits, "hw": self._hw, "names": self._names}
                ).encode())
            self._persisted = now
        except OSError:
            log.warning("could not save the linked hub ids")
