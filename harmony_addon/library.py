"""Global library of named configs: <data>/library/<id>/meta.json plus immutable revs/<n>.json.

Each config has one autosaved draft and any number of on-demand numbered revisions. A revision file
never changes once written. Every write is atomic (`atomic_write`). Hubs keep their own snapshot of
what they run (store.py), so deleting a config never breaks a hub.
"""

from __future__ import annotations

import hashlib
import json
import re
import shutil
from pathlib import Path
from typing import Any

from .store import HUB_ID, RevConflict, StoreError, atomic_write

SCHEMA_VERSION = 1
NAME_MAX = 80


class NotFound(StoreError):
    """No such config or revision."""


def content_hash(config: dict[str, Any]) -> str:
    canon = json.dumps(config, sort_keys=True, separators=(",", ":"), ensure_ascii=True)
    return hashlib.sha256(canon.encode()).hexdigest()[:16]


def slugify(name: str) -> str:
    s = re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-")[:40].strip("-")
    return s or "config"


def clean_name(name: object) -> str:
    if not isinstance(name, str):
        raise StoreError("invalid name")
    n = " ".join(name.split())
    if not n or len(n) > NAME_MAX or any(ord(c) < 32 for c in n):
        raise StoreError("invalid name")
    return n


def _dump(obj: Any) -> bytes:
    return json.dumps(obj, separators=(",", ":")).encode()


class Library:
    def __init__(self, data_dir: Path) -> None:
        self.dir = Path(data_dir) / "library"

    # ------------------------------------------------------------ paths and io
    def _cfg_dir(self, cid: str) -> Path:
        if not isinstance(cid, str) or not HUB_ID.fullmatch(cid):
            raise NotFound("unknown config")
        return self.dir / cid

    def _meta(self, cid: str) -> dict[str, Any]:
        path = self._cfg_dir(cid) / "meta.json"
        try:
            raw = path.read_bytes()
        except FileNotFoundError:
            raise NotFound("unknown config") from None
        try:
            meta = json.loads(raw)
        except ValueError:
            raise StoreError("stored config is corrupt") from None
        if not isinstance(meta, dict) or meta.get("v") != SCHEMA_VERSION:
            raise StoreError("stored config has an unsupported version")
        return meta

    def _save(self, meta: dict[str, Any]) -> None:
        atomic_write(self._cfg_dir(meta["id"]) / "meta.json", _dump(meta))

    def _unique_id(self, base: str) -> str:
        cid, n = base, 2
        while (self.dir / cid).exists():
            cid, n = f"{base}-{n}", n + 1
        return cid

    # ------------------------------------------------------------ queries
    def ids(self) -> list[str]:
        try:
            names = sorted(p.name for p in self.dir.iterdir() if (p / "meta.json").is_file())
        except OSError:
            return []
        return [n for n in names if HUB_ID.fullmatch(n)]

    @staticmethod
    def summary(meta: dict[str, Any]) -> dict[str, Any]:
        revs, draft = meta["revs"], meta["draft"]
        base = None if draft is None else draft.get("base_rev")
        base_hash = next((r["hash"] for r in revs if r["rev"] == base), None)
        return {
            "id": meta["id"],
            "name": meta["name"],
            "created": meta["created"],
            "updated": meta["updated"],
            "latest_rev": revs[-1]["rev"] if revs else None,
            "rev_count": len(revs),
            "draft_rev": meta["draft_rev"],
            "has_draft": draft is not None,
            "draft_updated": None if draft is None else draft["updated"],
            "base_rev": base,
            "dirty": draft is not None and draft.get("hash") != base_hash,
        }

    def list(self) -> list[dict[str, Any]]:
        out = []
        for cid in self.ids():
            try:
                out.append(self.summary(self._meta(cid)))
            except StoreError:
                continue
        return out

    def get(self, cid: str) -> dict[str, Any]:
        """The whole meta document (draft included, rev rows without configs)."""
        return self._meta(cid)

    def get_rev(self, cid: str, rev: int) -> dict[str, Any]:
        self._meta(cid)
        try:
            raw = (self._cfg_dir(cid) / "revs" / f"{int(rev)}.json").read_bytes()
        except (FileNotFoundError, ValueError):
            raise NotFound("unknown rev") from None
        try:
            return json.loads(raw)
        except ValueError:
            raise StoreError("stored revision is corrupt") from None

    # ------------------------------------------------------------ writes
    def create(
        self,
        name: str,
        config: dict[str, Any] | None = None,
        now: int = 0,
        *,
        cid: str | None = None,
        base_rev: int | None = None,
        migrated_from: str | None = None,
    ) -> dict[str, Any]:
        name = clean_name(name)
        cid = self._unique_id(cid or slugify(name))
        meta = {
            "v": SCHEMA_VERSION,
            "id": cid,
            "name": name,
            "created": now,
            "updated": now,
            "next_rev": 1,
            "draft_rev": 0,
            "draft": None,
            "revs": [],
        }
        if migrated_from:
            meta["migrated_from"] = migrated_from
        if config is not None:
            meta["draft_rev"] = 1
            meta["draft"] = {
                "config": config,
                "updated": now,
                "base_rev": base_rev,
                "hash": content_hash(config),
            }
        self._save(meta)
        return meta

    def duplicate(
        self, cid: str, rev: int | None, name: str, now: int = 0
    ) -> dict[str, Any]:
        """New config whose draft is `rev` of `cid` (default: its draft)."""
        if rev is None:
            draft = self._meta(cid)["draft"]
            if draft is None:
                raise StoreError("that config has no draft")
            config = draft["config"]
        else:
            config = self.get_rev(cid, rev)["config"]
        return self.create(name, config, now)

    def rename(self, cid: str, name: str, now: int = 0) -> dict[str, Any]:
        meta = self._meta(cid)
        meta["name"] = clean_name(name)
        meta["updated"] = now
        self._save(meta)
        return meta

    def delete(self, cid: str) -> None:
        self._meta(cid)
        shutil.rmtree(self._cfg_dir(cid))

    def set_draft(
        self, cid: str, config: dict[str, Any], now: int, if_rev: int | None = None
    ) -> dict[str, Any]:
        meta = self._meta(cid)
        if if_rev is not None and if_rev != meta["draft_rev"]:
            raise RevConflict(meta["draft_rev"])
        base = None if meta["draft"] is None else meta["draft"].get("base_rev")
        meta["draft_rev"] += 1
        meta["draft"] = {
            "config": config,
            "updated": now,
            "base_rev": base,
            "hash": content_hash(config),
        }
        meta["updated"] = now
        self._save(meta)
        return meta

    def save_rev(
        self, cid: str, note: str, now: int, *, reuse: bool = False
    ) -> tuple[dict[str, Any], dict[str, Any]]:
        """Draft -> immutable revision N+1 (the draft stays, based on N). `reuse`: when the draft is
        unchanged since the newest revision, return that one instead. Returns (meta, rev row)."""
        meta = self._meta(cid)
        draft = meta["draft"]
        if draft is None:
            raise StoreError("there is no draft to save")
        if reuse and meta["revs"] and meta["revs"][-1]["hash"] == draft["hash"]:
            return meta, meta["revs"][-1]
        rev = meta["next_rev"]
        row = {
            "rev": rev,
            "at": now,
            "note": str(note)[:200],
            "hash": draft["hash"],
            "parent": draft.get("base_rev"),
        }
        atomic_write(
            self._cfg_dir(cid) / "revs" / f"{rev}.json",
            _dump({**{k: row[k] for k in ("rev", "at", "note", "hash")}, "config": draft["config"]}),
        )
        meta["revs"].append(row)  # the rev file first: a crash leaves an unreferenced file only
        meta["next_rev"] = rev + 1
        meta["draft"] = {**draft, "base_rev": rev}
        meta["updated"] = now
        self._save(meta)
        return meta, row

    def restore(self, cid: str, rev: int, now: int = 0) -> dict[str, Any]:
        """Revision -> the draft (based on that revision)."""
        config = self.get_rev(cid, rev)["config"]
        meta = self._meta(cid)
        meta["draft_rev"] += 1
        meta["draft"] = {
            "config": config,
            "updated": now,
            "base_rev": int(rev),
            "hash": content_hash(config),
        }
        meta["updated"] = now
        self._save(meta)
        return meta


def migrate_v1(data_dir: Path, lib: Library) -> list[str]:
    """One-time: per-hub v1 files become a library config named after the hub plus a v2 hub file.

    History (oldest first) then applied become revs 1..n, the v1 draft stays the draft, and the hub
    points at the newest rev. The hub file is rewritten last, so a crash re-runs it cleanly (a
    config left by an earlier attempt is replaced). Returns the hub ids migrated."""
    hubs_dir = Path(data_dir) / "hubs"
    done: list[str] = []
    try:
        files = sorted(hubs_dir.glob("*.json"))
    except OSError:
        return done
    for path in files:
        hub = path.stem
        try:
            v1 = json.loads(path.read_bytes())
        except (OSError, ValueError):
            continue
        if not isinstance(v1, dict) or v1.get("v") != 1 or not HUB_ID.fullmatch(hub):
            continue
        for cid in lib.ids():  # an interrupted earlier run
            try:
                if lib._meta(cid).get("migrated_from") == hub:
                    lib.delete(cid)
            except StoreError:
                pass
        rows = [r for r in [*reversed(v1.get("history") or []), v1.get("applied")] if r]
        now = int(rows[-1]["at"]) if rows else 0
        draft = v1.get("draft")
        meta = lib.create(hub, None, now, migrated_from=hub)
        cid = meta["id"]
        entries: list[dict[str, Any]] = []
        for n, r in enumerate(rows, 1):
            cfg = r["config"]
            atomic_write(
                lib._cfg_dir(cid) / "revs" / f"{n}.json",
                _dump({"rev": n, "at": r["at"], "note": "Migrated", "hash": content_hash(cfg), "config": cfg}),
            )
            meta["revs"].append(
                {"rev": n, "at": r["at"], "note": "Migrated", "hash": content_hash(cfg), "parent": n - 1 or None}
            )
            entries.append(
                {"config_id": cid, "rev": n, "name": meta["name"], "at": r["at"], "hash": r["hash"], "config": cfg}
            )
        meta["next_rev"] = len(rows) + 1
        meta["draft_rev"] = int(v1.get("draft_rev") or 0)
        if isinstance(draft, dict) and isinstance(draft.get("config"), dict):
            meta["draft_rev"] = max(meta["draft_rev"], 1)
            meta["draft"] = {
                "config": draft["config"],
                "updated": int(draft.get("updated") or now),
                "base_rev": len(rows) or None,
                "hash": content_hash(draft["config"]),
            }
            meta["updated"] = meta["draft"]["updated"]
        lib._save(meta)
        newest_first = list(reversed(entries))
        state = {
            "v": 2,
            "ack": v1.get("ack"),
            "applied": newest_first[0] if newest_first else None,
            "history": newest_first[1:][:5],
        }
        atomic_write(path, _dump(state))
        done.append(hub)
    return done
