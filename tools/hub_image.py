#!/usr/bin/env python3
"""Offline verification of a signed hub image (the `hub-X.Y.Z.img` a release attaches), for a first install.

The image is the daemon behind a 144-byte header (docs/proto/ota-image.md): magic "HOTA", format, target, flags, build id,
payload length, SHA-512 of the payload, and an Ed25519 signature over those first 80 bytes. `verify_image` checks all of
it against a public key in pure Python (RFC 8032, no dependency; one verify takes a few milliseconds), the same checks
as common/ota/ota_image.c. The key is the committed release key (common/ota/ota_pubkey_release.h), never the dev key.
"""

from __future__ import annotations

import hashlib
import re
import struct
import urllib.request
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
RELEASE_HEADER = REPO / "common" / "ota" / "ota_pubkey_release.h"
RELEASE_URL = "https://github.com/metril/ha-harmony/releases/download/v{v}/hub-{v}.img"
HDR = 144
SIGNED = 80
MAX_PAYLOAD = 32 * 1024 * 1024
MAX_IMAGE = (
    8 * 1024 * 1024
)  # a hub image is under 1 MiB; refuse to buffer more than this from the network
TARGET_HUB = 1
_VERSION = re.compile(r"\d{1,4}\.\d{1,4}\.\d{1,4}")


class ImageError(Exception):
    pass


@dataclass(frozen=True)
class Image:
    build: int
    payload: bytes


# ---------------------------------------------------------------- Ed25519 verify (RFC 8032, section 5.1.7)
_P = 2**255 - 19
_L = 2**252 + 27742317777372353535851937790883648493


def _inv(x: int) -> int:
    return pow(x, _P - 2, _P)


_D = -121665 * _inv(121666) % _P
_I = pow(2, (_P - 1) // 4, _P)


def _recover_x(y: int, sign: int) -> int | None:
    if y >= _P:
        return None
    x2 = (y * y - 1) * _inv(_D * y * y + 1) % _P
    if x2 == 0:
        return None if sign else 0
    x = pow(x2, (_P + 3) // 8, _P)
    if (x * x - x2) % _P:
        x = x * _I % _P
    if (x * x - x2) % _P:
        return None
    return _P - x if (x & 1) != sign else x


_Gy = 4 * _inv(5) % _P
_Gx = _recover_x(_Gy, 0)
assert _Gx is not None
_G = (_Gx, _Gy, 1, _Gx * _Gy % _P)


def _add(p: tuple[int, ...], q: tuple[int, ...]) -> tuple[int, ...]:
    a = (p[1] - p[0]) * (q[1] - q[0]) % _P
    b = (p[1] + p[0]) * (q[1] + q[0]) % _P
    c = 2 * p[3] * q[3] * _D % _P
    d = 2 * p[2] * q[2] % _P
    e, f, g, h = b - a, d - c, d + c, b + a
    return (e * f % _P, g * h % _P, f * g % _P, e * h % _P)


def _mul(s: int, p: tuple[int, ...]) -> tuple[int, ...]:
    q: tuple[int, ...] = (0, 1, 1, 0)
    while s:
        if s & 1:
            q = _add(q, p)
        p = _add(p, p)
        s >>= 1
    return q


def _same(p: tuple[int, ...], q: tuple[int, ...]) -> bool:
    return (p[0] * q[2] - q[0] * p[2]) % _P == 0 and (p[1] * q[2] - q[1] * p[2]) % _P == 0


def _decode(b: bytes) -> tuple[int, ...] | None:
    y = int.from_bytes(b, "little")
    sign, y = y >> 255, y & ((1 << 255) - 1)
    x = _recover_x(y, sign)
    return None if x is None else (x, y, 1, x * y % _P)


def ed25519_verify(pub: bytes, msg: bytes, sig: bytes) -> bool:
    if len(pub) != 32 or len(sig) != 64:
        return False
    a, r = _decode(pub), _decode(sig[:32])
    s = int.from_bytes(sig[32:], "little")
    if a is None or r is None or s >= _L:
        return False
    h = int.from_bytes(hashlib.sha512(sig[:32] + pub + msg).digest(), "little") % _L
    return _same(_mul(s, _G), _add(r, _mul(h, a)))


# ---------------------------------------------------------------- image
def release_pubkey(header: Path = RELEASE_HEADER) -> bytes:
    """The 32 public-key bytes of a generated header (`ota_sign pubheader`), refusing a dev-key header."""
    text = header.read_text()
    if "OTA_PUBKEY_IS_DEV 0" not in text:
        raise ImageError(f"{header} is not a release public-key header")
    m = re.search(r"OTA_PUBKEY_INIT\s*\\?\s*\{([^}]*)\}", text)
    nums = re.findall(r"0x([0-9a-fA-F]{2})", m.group(1)) if m else []
    if len(nums) != 32:
        raise ImageError(f"{header} has no 32-byte public key")
    return bytes(int(n, 16) for n in nums)


def verify_image(data: bytes, pubkey: bytes) -> Image:
    """Check header, signature, size and payload digest; return the build id and the payload (the stripped daemon)."""
    if len(data) < HDR or data[:4] != b"HOTA":
        raise ImageError("not a signed Harmony image (no HOTA header)")
    _magic, version, target, flags, build, length = struct.unpack_from("<4sBBHII", data)
    if version != 1 or flags != 0:
        raise ImageError("unsupported image format")
    if target != TARGET_HUB:
        raise ImageError("this image is not for the hub")
    if not ed25519_verify(pubkey, data[:SIGNED], data[SIGNED:HDR]):
        raise ImageError("bad signature: not signed with the release key")
    if length > MAX_PAYLOAD or len(data) != HDR + length:
        raise ImageError(f"bad size: header says {HDR + length} bytes, file has {len(data)}")
    payload = data[HDR:]
    if hashlib.sha512(payload).digest() != data[16:SIGNED]:
        raise ImageError("payload digest mismatch")
    return Image(build, payload)


def release_url(version: str) -> str:
    if not _VERSION.fullmatch(version):
        raise ImageError(f"version must look like 0.5.9, got {version!r}")
    return RELEASE_URL.format(v=version)


def download(
    version: str, opener: Callable[..., object] = urllib.request.urlopen, timeout: float = 60.0
) -> bytes:
    """The release's hub image, into memory (size-capped). The caller verifies it."""
    url = release_url(version)
    try:
        with opener(url, timeout=timeout) as resp:  # type: ignore[attr-defined]
            data = resp.read(MAX_IMAGE + 1)
    except OSError as exc:
        raise ImageError(f"cannot download {url}: {exc}") from None
    if len(data) > MAX_IMAGE:
        raise ImageError("image too large")
    return data
