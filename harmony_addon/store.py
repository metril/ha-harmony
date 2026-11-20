"""Hub state: what each hub runs (a snapshot of a library config revision) and its history, one JSON file per hub.

All state lives under the data dir given at construction (never anywhere else). A hub's file is
replaced atomically (temp file + fsync + rename + dir fsync), so a crash leaves the old file.
"""

from __future__ import annotations

import json
import os
import re
import tempfile
from pathlib import Path
from typing import Any

SCHEMA_VERSION = 2
HISTORY_MAX = 5
HUB_ID = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,62}$")  # the hub's own ids, e.g. ha-harmony-abc


class StoreError(Exception):
    """Unreadable/unsupported stored state or a request the state cannot satisfy."""


class RevConflict(StoreError):
    """The draft changed since the caller read it (`rev` is the current draft revision)."""

    def __init__(self, rev: int) -> None:
        super().__init__("the draft changed since it was read")
        self.rev = rev


def atomic_write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=path.parent, prefix=f".{path.name}.", suffix=".tmp")
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise
    dfd = os.open(path.parent, os.O_RDONLY)
    try:
        os.fsync(dfd)
    finally:
        os.close(dfd)


def _empty() -> dict[str, Any]:
    return {"v": SCHEMA_VERSION, "ack": None, "applied": None, "history": []}


class Store:
    def __init__(self, data_dir: Path) -> None:
        self.dir = Path(data_dir) / "hubs"

    def _path(self, hub: str) -> Path:
        if not HUB_ID.fullmatch(hub):
            raise StoreError("invalid hub id")
        return self.dir / f"{hub}.json"

    def load(self, hub: str) -> dict[str, Any]:
        path = self._path(hub)
        try:
            raw = path.read_bytes()
        except FileNotFoundError:
            return _empty()
        try:
            state = json.loads(raw)
        except ValueError:
            raise StoreError("stored state is corrupt") from None
        if not isinstance(state, dict) or state.get("v") != SCHEMA_VERSION:
            raise StoreError("stored state has an unsupported version")
        state.setdefault("ack", None)
        return state

    def hubs(self, limit: int = 200) -> list[str]:
        """Ids of the hubs that have a state file (sorted)."""
        try:
            names = sorted(p.stem for p in self.dir.glob("*.json"))
        except OSError:
            return []
        return [n for n in names if HUB_ID.fullmatch(n)][:limit]

    def adopt(self, src: str, dst: str) -> bool:
        """Move hub `src`'s state to `dst` (one atomic rename). False, and nothing changes, when `src`
        has no state or `dst` already has some, so repeating it is harmless."""
        a, b = self._path(src), self._path(dst)
        if not a.exists() or b.exists():
            return False
        os.replace(a, b)
        dfd = os.open(self.dir, os.O_RDONLY)
        try:
            os.fsync(dfd)
        finally:
            os.close(dfd)
        return True

    def set_ack(self, hub: str, ack: dict[str, Any]) -> dict[str, Any]:
        state = self.load(hub)
        state["ack"] = ack
        self._save(hub, state)
        return state

    def _save(self, hub: str, state: dict[str, Any]) -> None:
        atomic_write(self._path(hub), json.dumps(state, separators=(",", ":")).encode())

    def apply(self, hub: str, entry: dict[str, Any]) -> dict[str, Any]:
        """`entry` {config_id, rev, name, at, hash, config} becomes what the hub runs; the previous one goes to history."""
        state = self.load(hub)
        if state["applied"] is not None:
            state["history"] = [state["applied"], *state["history"]][:HISTORY_MAX]
        state["applied"] = entry
        self._save(hub, state)
        return state

    def revert(self, hub: str, target: str = "previous") -> dict[str, Any]:
        """Re-apply the newest history entry; the entry it replaces becomes the newest history entry
        (so it can be reverted again)."""
        if target != "previous":
            raise StoreError("unknown revert target")
        state = self.load(hub)
        if not state["history"]:
            raise StoreError("no previous version to revert to")
        current = state["applied"]
        state["applied"], *rest = state["history"]
        state["history"] = ([current, *rest] if current is not None else rest)[:HISTORY_MAX]
        self._save(hub, state)
        return state
