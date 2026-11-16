"""SmartIR device JSON parser and Broadlink IR packet decoder."""

from __future__ import annotations

import base64
import binascii
import json
from pathlib import Path
from typing import Any

from .model import MAX_US, IrCode, IrImportError, normalise_raw
from .pronto import normalise_pronto

_TICK_US = 8192 / 269  # Broadlink tick length in microseconds
_DEFAULT_HZ = 38000


def decode_broadlink(packet: bytes) -> list[int]:
    """Decode a Broadlink IR packet to mark/space microseconds (38 kHz carrier)."""
    if len(packet) < 4:
        raise IrImportError("broadlink: packet too short")
    if packet[0] != 0x26:
        raise IrImportError(f"broadlink: not an IR packet (type byte 0x{packet[0]:02x}, need 0x26)")
    length = int.from_bytes(packet[2:4], "little")
    data = packet[4 : 4 + length]
    if len(data) != length:
        raise IrImportError(f"broadlink: length field {length} exceeds packet ({len(data)} bytes)")
    us: list[int] = []
    i = 0
    while i < len(data):
        b = data[i]
        if b == 0:
            if i + 2 >= len(data):
                raise IrImportError("broadlink: truncated extended pulse")
            ticks = int.from_bytes(data[i + 1 : i + 3], "big")
            i += 3
        else:
            ticks = b
            i += 1
        us.append(min(MAX_US, max(1, round(ticks * _TICK_US))))
    if not us:
        raise IrImportError("broadlink: packet has no pulses")
    return us


def _raw_leaf(value: Any) -> IrCode:
    hz = _DEFAULT_HZ
    if isinstance(value, str):
        head, sep, rest = value.partition(":")
        if sep:
            hz = int(head)
            value = rest
        value = value.replace(",", " ").split()
    return IrCode(hz=hz, us=normalise_raw([abs(int(v)) for v in value]))


def _leaf(enc: str, value: Any) -> IrCode:
    if enc == "base64":
        if not isinstance(value, str):
            raise IrImportError("expected base64 string")
        try:
            packet = base64.b64decode(value, validate=True)
        except (binascii.Error, ValueError):
            raise IrImportError("invalid base64") from None
        return IrCode(hz=_DEFAULT_HZ, us=normalise_raw(decode_broadlink(packet)))
    if enc == "pronto":
        return IrCode(pronto=normalise_pronto(value))
    if not isinstance(value, (str, list)):
        raise IrImportError("expected raw timing string or list")
    try:
        return _raw_leaf(value)
    except ValueError as e:
        if isinstance(e, IrImportError):
            raise
        raise IrImportError(f"bad raw timings ({e})") from None


def _walk(enc: str, node: Any, path: str, out: dict[str, IrCode]) -> None:
    if isinstance(node, dict):
        for k, v in node.items():
            _walk(enc, v, f"{path}.{k}" if path else str(k), out)
        return
    if isinstance(node, str) or (enc == "raw" and isinstance(node, list)):
        try:
            out[path] = _leaf(enc, node)
        except IrImportError as e:
            raise IrImportError(f"command {path}: {e}") from None
        return
    raise IrImportError(f"command {path}: unsupported value of type {type(node).__name__}")


def parse_smartir(data: dict) -> dict[str, IrCode]:
    """Flatten SmartIR `commands` to {"a.b.c": IrCode}."""
    if not isinstance(data, dict):
        raise IrImportError("SmartIR file must be a JSON object")
    enc = str(data.get("commandsEncoding", "")).lower()
    if enc not in ("base64", "pronto", "raw"):
        raise IrImportError(f"unsupported commandsEncoding {data.get('commandsEncoding')!r}")
    cmds = data.get("commands")
    if not isinstance(cmds, dict) or not cmds:
        raise IrImportError("SmartIR file has no commands")
    out: dict[str, IrCode] = {}
    _walk(enc, cmds, "", out)
    return out


def parse_smartir_file(path: str | Path) -> dict[str, IrCode]:
    try:
        data = json.loads(Path(path).read_text(encoding="utf-8"))
    except json.JSONDecodeError as e:
        raise IrImportError(f"invalid JSON: {e}") from None
    return parse_smartir(data)
