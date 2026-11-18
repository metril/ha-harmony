"""Config store: draft / applied / history per hub id, one versioned JSON file per hub.

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

SCHEMA_VERSION = 1
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
    return {
        "v": SCHEMA_VERSION,
        "next_rev": 1,
        "draft_rev": 0,
        "draft": None,
        "applied": None,
        "history": [],
    }


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
        if not isinstance(state.get("draft_rev"), int):
            state["draft_rev"] = 0  # files written before draft revisions existed
        return state

    def _save(self, hub: str, state: dict[str, Any]) -> None:
        atomic_write(self._path(hub), json.dumps(state, separators=(",", ":")).encode())

    def set_draft(
        self, hub: str, config: dict[str, Any], now: int, if_rev: int | None = None
    ) -> dict[str, Any]:
        """`if_rev` set: refuse (RevConflict) unless the draft is still at that revision."""
        state = self.load(hub)
        if if_rev is not None and if_rev != state["draft_rev"]:
            raise RevConflict(state["draft_rev"])
        state["draft_rev"] += 1
        state["draft"] = {"config": config, "updated": now}
        self._save(hub, state)
        return state

    def apply(self, hub: str, config: dict[str, Any], cfg_hash: str, now: int) -> dict[str, Any]:
        """Make `config` the applied config (draft cleared); the previous one goes to history."""
        state = self.load(hub)
        if state["applied"] is not None:
            state["history"] = [state["applied"], *state["history"]][:HISTORY_MAX]
        state["applied"] = {"rev": state["next_rev"], "at": now, "hash": cfg_hash, "config": config}
        state["next_rev"] += 1
        state["draft_rev"] += 1
        state["draft"] = None
        self._save(hub, state)
        return state

    def revert(self, hub: str, target: str) -> dict[str, Any]:
        """`applied`: discard the draft. `previous`: re-apply the newest history entry."""
        state = self.load(hub)
        state["draft_rev"] += 1
        if target == "applied":
            state["draft"] = None
        elif target == "previous":
            if not state["history"]:
                raise StoreError("no previous version to revert to")
            state["applied"], *state["history"] = state["history"]
            state["draft"] = None
        else:
            raise StoreError("unknown revert target")
        self._save(hub, state)
        return state
