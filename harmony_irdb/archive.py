"""Async, cached, size-capped access to the Logitech Harmony IR archive.

Only the two fixed hosts in `BASES` are ever contacted (redirects are not followed). Every path
component that comes from archive data is validated before it is used in a URL; cache file names
are hashes, never archive-supplied text. Blocking disk I/O runs in a worker thread.
"""

from __future__ import annotations

import asyncio
import hashlib
import json
import os
import re
import time
import unicodedata
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path
from typing import Any
from urllib.parse import quote

import aiohttp

# Same order as the upstream web client: jsDelivr first, GitHub raw as the fallback.
BASES = (
    "https://cdn.jsdelivr.net/gh/pickysysadmin/logitech-harmony-ir-archive@main/",
    "https://raw.githubusercontent.com/pickysysadmin/logitech-harmony-ir-archive/main/",
)
DEFAULT_TTL = 7 * 86400
DEFAULT_MAX_BYTES = 8 * 1024 * 1024  # the biggest real file (Sony's model index) is ~1 MB
MAX_CODESET_COMMANDS = 2000  # real codesets have well under 500
MANUFACTURERS_FILE = Path(__file__).with_name("manufacturers.json")

_CODESET = re.compile(r"^codesets/[0-9A-Za-z_-]{1,16}/[0-9A-Za-z_-]{1,80}\.json$")


class ArchiveError(Exception):
    """Archive data or a response was unusable."""


class ArchiveLimitError(ArchiveError):
    """A fetched entry exceeds a processing limit (it is dropped from the cache)."""

    code = "archive_too_large"


class OfflineError(ArchiveError):
    """The network is unavailable (or disabled) and the item is not in the cache."""


def safe_segment(value: object) -> str:
    """A single path component taken from archive data; raises ArchiveError if suspicious."""
    if (
        not isinstance(value, str)
        or not 0 < len(value) <= 200
        or value in (".", "..")
        or any(c in value for c in "/\\")
        or any(ord(c) < 32 or ord(c) == 127 for c in value)
    ):
        raise ArchiveError(f"unsafe name in archive data: {value!r}"[:120])
    return value


def safe_codeset_path(value: object) -> str:
    if not isinstance(value, str) or not _CODESET.match(value):
        raise ArchiveError(f"unexpected codeset path: {value!r}"[:120])
    return value


def fold(text: str) -> str:
    """Case-, diacritic- and punctuation-insensitive form used for searching."""
    s = unicodedata.normalize("NFKD", text)
    s = "".join(c for c in s if not unicodedata.combining(c)).casefold()
    return re.sub(r"[^0-9a-z]+", " ", s).strip()


def _rank(name_folded: str, q: str) -> int | None:
    if name_folded == q:
        return 0
    if name_folded.startswith(q):
        return 1
    if any(w.startswith(q) for w in name_folded.split()):
        return 2
    if q in name_folded:
        return 3
    return None


@dataclass(frozen=True)
class Brand:
    name: str
    slug: str
    count: int = 0


@dataclass(frozen=True)
class Model:
    file: str  # file name under devices/<slug>/
    name: str
    id: int | None = None


def load_manufacturers(path: Path = MANUFACTURERS_FILE) -> list[Brand]:
    """Blocking; the shipped list is `[[name, count] | [name, count, slug], ...]`."""
    out = []
    for row in json.loads(path.read_text(encoding="utf-8")):
        out.append(Brand(row[0], row[2] if len(row) > 2 else row[0], row[1]))
    return out


def search_brands(brands: list[Brand], query: str, limit: int = 50) -> list[Brand]:
    q = fold(query)
    if not q:
        return sorted(brands, key=lambda b: (-b.count, b.name))[:limit]
    hits = []
    for b in brands:
        r = _rank(fold(b.name), q)
        if r is not None:
            hits.append((r, -b.count, b.name, b))
    hits.sort(key=lambda t: t[:3])
    return [t[3] for t in hits[:limit]]


def search_models(models: list[Model], query: str, limit: int = 50) -> list[Model]:
    toks = fold(query).split()
    if not toks:
        return models[:limit]
    hits = []
    for m in models:
        f = fold(m.name)
        if all(t in f for t in toks):
            hits.append(
                (_rank(f, " ".join(toks)) if len(toks) > 1 else _rank(f, toks[0]), m.name, m)
            )
    hits.sort(key=lambda t: (9 if t[0] is None else t[0], t[1]))
    return [t[2] for t in hits[:limit]]


class IrdbArchive:
    def __init__(
        self,
        session: Any,
        cache_dir: str | os.PathLike[str],
        *,
        bases: tuple[str, ...] = BASES,
        ttl: float = DEFAULT_TTL,
        max_bytes: int = DEFAULT_MAX_BYTES,
        offline: bool = False,
        timeout: float = 30.0,
        now: Callable[[], float] = time.time,
    ) -> None:
        self._session = session
        self._dir = Path(cache_dir)
        self._bases = bases
        self._ttl = ttl
        self._max = max_bytes
        self._offline = offline
        self._timeout = timeout
        self._now = now
        self._brands: list[Brand] | None = None

    # ---- cache
    def _file(self, path: str) -> Path:
        return self._dir / (hashlib.sha256(path.encode()).hexdigest()[:40] + ".json")

    def _read(self, path: str) -> dict | None:
        try:
            entry = json.loads(self._file(path).read_text(encoding="utf-8"))
            return entry if entry.get("path") == path and "data" in entry else None
        except OSError, ValueError, AttributeError:
            return None

    def _drop(self, path: str) -> None:
        """Forget a cache entry (a poisoned file must not be served again)."""
        try:
            self._file(path).unlink()
        except OSError:
            pass

    def _write(self, path: str, entry: dict) -> None:
        self._dir.mkdir(parents=True, exist_ok=True)
        f = self._file(path)
        tmp = f.with_name(f.name + f".{os.getpid()}.tmp")
        tmp.write_text(json.dumps(entry, separators=(",", ":")), encoding="utf-8")
        os.replace(tmp, f)

    # ---- network
    async def _get(self, url: str, etag: str | None) -> tuple[int, bytes, str | None]:
        headers = {"Accept": "application/json"}
        if etag:
            headers["If-None-Match"] = etag
        async with self._session.get(
            url,
            headers=headers,
            allow_redirects=False,
            timeout=aiohttp.ClientTimeout(total=self._timeout),
        ) as resp:
            if resp.status != 200:
                return resp.status, b"", None
            declared = resp.headers.get("Content-Length")
            if declared and declared.isdigit() and int(declared) > self._max:
                raise ArchiveError(f"response larger than {self._max} bytes")
            body = bytearray()
            async for chunk in resp.content.iter_chunked(65536):
                body += chunk
                if len(body) > self._max:
                    raise ArchiveError(f"response larger than {self._max} bytes")
            return 200, bytes(body), resp.headers.get("ETag")

    async def fetch_json(self, path: str) -> Any:
        """`path` is relative to the archive root and already validated by the caller."""
        entry = await asyncio.to_thread(self._read, path)
        now = self._now()
        if entry and (self._offline or now - float(entry.get("ts", 0)) < self._ttl):
            return entry["data"]
        if self._offline:
            raise OfflineError(f"offline and {path} is not cached")
        last: Exception | None = None
        network_down = True
        for base in self._bases:
            try:
                status, body, etag = await self._get(
                    base + quote(path), entry.get("etag") if entry else None
                )
            except ArchiveError:
                raise
            except (aiohttp.ClientError, TimeoutError, OSError) as exc:
                last = exc
                continue
            network_down = False
            if status == 304 and entry:
                entry["ts"] = now
                await asyncio.to_thread(self._write, path, entry)
                return entry["data"]
            if status != 200:
                last = ArchiveError(f"HTTP {status} for {path}")
                continue
            try:
                data = await asyncio.to_thread(json.loads, body)
            except ValueError, RecursionError:
                last = ArchiveError(f"{path}: not valid JSON")
                continue
            await asyncio.to_thread(
                self._write, path, {"path": path, "etag": etag, "ts": now, "data": data}
            )
            return data
        if entry:  # stale beats nothing
            return entry["data"]
        if network_down:
            raise OfflineError(f"cannot reach the archive and {path} is not cached: {last}")
        raise ArchiveError(str(last))

    # ---- typed accessors
    async def brands(self) -> list[Brand]:
        if self._brands is None:
            self._brands = await asyncio.to_thread(load_manufacturers)
        return self._brands

    async def search_brands(self, query: str, limit: int = 50) -> list[Brand]:
        return search_brands(await self.brands(), query, limit)

    async def models(self, slug: str) -> list[Model]:
        data = await self.fetch_json(f"devices/{safe_segment(slug)}/index.json")
        if not isinstance(data, list):
            raise ArchiveError("model index is not a list")
        out = []
        for e in data:
            if not isinstance(e, dict) or not isinstance(e.get("f"), str):
                continue
            try:
                safe_segment(e["f"])
            except ArchiveError:
                continue  # one bad entry must not poison the brand
            if not e["f"].endswith(".json") or e["f"] == "index.json":
                continue
            name = e.get("m") if isinstance(e.get("m"), str) else ""
            ident = e.get("id")
            out.append(
                Model(
                    e["f"], name.strip() or e["f"][:-5], ident if isinstance(ident, int) else None
                )
            )
        return out

    async def device(self, slug: str, file: str) -> dict:
        data = await self.fetch_json(f"devices/{safe_segment(slug)}/{safe_segment(file)}")
        if not isinstance(data, dict):
            await asyncio.to_thread(self._drop, f"devices/{slug}/{file}")
            raise ArchiveError("device file is not an object")
        return data

    async def codeset(self, path: str) -> dict:
        data = await self.fetch_json(safe_codeset_path(path))
        if not isinstance(data, dict) or not isinstance(data.get("commands"), list):
            await asyncio.to_thread(self._drop, path)
            raise ArchiveError("codeset has no command list")
        if len(data["commands"]) > MAX_CODESET_COMMANDS:
            await asyncio.to_thread(self._drop, path)
            raise ArchiveLimitError(
                f"codeset has {len(data['commands'])} commands (limit {MAX_CODESET_COMMANDS})"
            )
        return data
