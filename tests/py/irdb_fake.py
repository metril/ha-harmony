"""Test helpers for the archive client: a fake aiohttp-like session over the fixture slice. No network."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
from urllib.parse import unquote

import aiohttp

from harmony_irdb.archive import BASES

FIX = Path(__file__).parent / "fixtures" / "irdb"


def fixture_files() -> dict[str, bytes]:
    return {
        str(p.relative_to(FIX)): p.read_bytes()
        for p in FIX.rglob("*.json")
        if p.name != "exactness.json"
    }


class _Content:
    def __init__(self, body: bytes) -> None:
        self._body = body

    async def iter_chunked(self, n: int):
        for i in range(0, len(self._body), n):
            yield self._body[i : i + n]


class _Resp:
    def __init__(self, status: int, body: bytes = b"", headers: dict | None = None) -> None:
        self.status = status
        self.headers = headers or {}
        self.content = _Content(body)

    async def __aenter__(self):
        return self

    async def __aexit__(self, *exc):
        return False


class FakeSession:
    """Serves `files` under either archive base. `down` bases raise a connection error."""

    def __init__(self, files: dict[str, bytes] | None = None) -> None:
        self.files = fixture_files() if files is None else files
        self.down: set[str] = set()
        self.status: dict[str, int] = {}  # base -> forced status
        self.calls: list[tuple[str, dict, dict]] = []
        self.content_length: int | None = None

    def get(self, url, headers=None, **kw):
        self.calls.append((url, dict(headers or {}), kw))
        for base in BASES:
            if url.startswith(base):
                break
        else:
            raise AssertionError(f"contacted a host outside the fixed list: {url}")
        if base in self.down:
            raise aiohttp.ClientConnectionError("down")
        if base in self.status:
            return _Resp(self.status[base])
        path = unquote(url[len(base) :])
        body = self.files.get(path)
        if body is None:
            return _Resp(404)
        etag = '"' + hashlib.sha1(body).hexdigest()[:12] + '"'
        if (headers or {}).get("If-None-Match") == etag:
            return _Resp(304)
        h = {"ETag": etag}
        if self.content_length is not None:
            h["Content-Length"] = str(self.content_length)
        return _Resp(200, body, h)

    def urls(self) -> list[str]:
        return [c[0] for c in self.calls]


def jbytes(obj) -> bytes:
    return json.dumps(obj).encode()
