"""The hub firmware bundled into the add-on image (CI writes `hub.img` and `hub.json` next to it).

`hub.json` is {version, build, size, sha512}. The image is announced to the integration (the `fw` frame after `hello`)
and served at GET /api/v1/firmware/hub.img, both only when the metadata is well formed and the file's size matches it.
The integration re-checks everything (header, build, size, sha512) before it sends a byte to the hub."""

from __future__ import annotations

import asyncio
import json
from pathlib import Path
from typing import Any

from aiohttp import web

from .web_util import error

FIRMWARE_KEY = web.AppKey("firmware_dir", Path)
DEFAULT_DIR = Path(__file__).resolve().parent.parent / "firmware"  # /opt/app/firmware in the image
MAX_IMAGE = 8 << 20


def _int(v: Any) -> bool:
    return type(v) is int and v >= 0


def read_meta(directory: Path) -> dict[str, Any] | None:
    """The validated hub.json, or None when no usable image is bundled (blocking: small file reads)."""
    try:
        meta = json.loads((directory / "hub.json").read_text())
        size = (directory / "hub.img").stat().st_size
    except OSError, ValueError:
        return None
    if not isinstance(meta, dict):
        return None
    ver, build, msize, sha = (meta.get(k) for k in ("version", "build", "size", "sha512"))
    if not (isinstance(ver, str) and 0 < len(ver) <= 64 and isinstance(sha, str)):
        return None
    if not (_int(build) and _int(msize) and msize == size and 0 < size <= MAX_IMAGE):
        return None
    return {"version": ver, "build": build, "size": msize, "sha512": sha}


def announce(meta: dict[str, Any]) -> dict[str, Any]:
    """The `fw` the hello answer carries: the integration checks the downloaded image against all four."""
    return dict(meta)


def read_image(directory: Path) -> bytes | None:
    return (directory / "hub.img").read_bytes() if read_meta(directory) is not None else None


async def hub_img(request: web.Request) -> web.Response:
    directory = request.app[FIRMWARE_KEY]
    data = await asyncio.get_running_loop().run_in_executor(None, read_image, directory)
    if data is None:
        raise error(404, "no_firmware", "no firmware bundled")
    return web.Response(body=data, content_type="application/octet-stream")


def add_routes(app: web.Application, api: str) -> None:
    app.router.add_get(f"{api}/firmware/hub.img", hub_img)
