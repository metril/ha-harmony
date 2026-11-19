"""Signed hub firmware update: client-side checks and the chunked upload (docs/proto/ota-image.md).

The hub's verification is the authority; the checks here only turn the common mistakes (an unsigned
file, a truncated download, a remote image) into a readable error before anything is sent.
"""

from __future__ import annotations

import base64
import hashlib
import os
import struct
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

from .errors import invalid

HOTA_MAGIC = b"HOTA"
HOTA_HEADER = 144
HOTA_MAX_PAYLOAD = 32 * 1024 * 1024
TARGET_HUB = 1
CHUNK = 4096  # the hub's cap on one ota_chunk (decoded bytes)
FIRMWARE_SUBDIR = ("ha_harmony", "firmware")  # under the HA config dir; always allowed
UPLOAD_ATTEMPTS = 3


@dataclass(frozen=True)
class Image:
    data: bytes
    build: int
    sha512: str  # hex of the whole file: the hub's transfer check

    @property
    def size(self) -> int:
        return len(self.data)


def parse_image(data: bytes) -> Image:
    """Friendly refusal of anything that is not a complete signed hub image. Does NOT verify the signature."""
    if len(data) < HOTA_HEADER or data[:4] != HOTA_MAGIC:
        raise invalid("ota_not_signed")
    _magic, version, target, flags, build, length = struct.unpack_from("<4sBBHII", data)
    if version != 1 or flags != 0:
        raise invalid("ota_not_signed")
    if target != TARGET_HUB:
        raise invalid("ota_wrong_target")
    if length > HOTA_MAX_PAYLOAD or len(data) != HOTA_HEADER + length:
        raise invalid("ota_truncated", expected=min(length, HOTA_MAX_PAYLOAD) + HOTA_HEADER, actual=len(data))
    return Image(data, build, hashlib.sha512(data).hexdigest())


def path_allowed(config_dir: str, is_allowed_path: Callable[[str], bool], path: str) -> str | None:
    """The resolved path when it lies under <config>/ha_harmony/firmware or an allowlist_external_dirs entry."""
    real = os.path.realpath(path)
    base = os.path.realpath(os.path.join(config_dir, *FIRMWARE_SUBDIR))
    if real == base or real.startswith(base + os.sep) or is_allowed_path(real):
        return real
    return None


def read_image(path: str) -> bytes:
    p = Path(path)
    if not p.is_file():
        raise invalid("ota_file_missing", path=path)
    if p.stat().st_size > HOTA_MAX_PAYLOAD + HOTA_HEADER:
        raise invalid("ota_truncated", expected=HOTA_MAX_PAYLOAD + HOTA_HEADER, actual=p.stat().st_size)
    return p.read_bytes()


def encode_chunk(image: Image, off: int) -> str:
    return base64.b64encode(image.data[off : off + CHUNK]).decode()
