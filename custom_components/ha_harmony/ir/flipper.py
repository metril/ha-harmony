"""Flipper Zero .ir parser.

Parsed signals: address/command are little-endian hex byte strings ("87 7C 00 00" -> 0x7C87).
Unsupported protocols raise UnsupportedProtocolError (nothing is silently dropped).
"""

from __future__ import annotations

import re
from pathlib import Path

from .model import IrCode, IrImportError, UnsupportedProtocolError, normalise_raw

FLIPPER_PROTOCOLS = {
    "NEC": "nec",
    "NECEXT": "necext",
    "SAMSUNG32": "samsung32",
    "RC5": "rc5",
    "RC5X": "rc5",  # 6-bit file command; our rc5 cmd gets bit 6 set (field bit = 0)
    "RC6": "rc6",
    "SIRC": "sirc12",
    "SIRC15": "sirc15",
    "SIRC20": "sirc20",
}

_HEXBYTE = re.compile(r"[0-9A-Fa-f]{2}")


def _le_int(value: str, field: str, name: str) -> int:
    toks = value.split()
    if not toks or not all(_HEXBYTE.fullmatch(t) for t in toks):
        raise IrImportError(f"signal {name!r}: {field} is not a hex byte string: {value!r}")
    return int.from_bytes(bytes(int(t, 16) for t in toks), "little")


def _need(fields: dict[str, str], key: str, name: str) -> str:
    if key not in fields:
        raise IrImportError(f"signal {name!r}: missing {key}")
    return fields[key]


def _build(fields: dict[str, str]) -> IrCode:
    name = fields["name"]
    kind = _need(fields, "type", name).lower()
    if kind == "parsed":
        proto = _need(fields, "protocol", name)
        addr = _need(fields, "address", name)
        cmd = _need(fields, "command", name)
        ours = FLIPPER_PROTOCOLS.get(proto.upper())
        if ours is None:
            raise UnsupportedProtocolError(
                f"signal {name!r}: protocol {proto!r} is not supported by the hub"
            )
        a, c = _le_int(addr, "address", name), _le_int(cmd, "command", name)
        if ours == "rc5":
            if c > 0x3F:
                raise IrImportError(f"signal {name!r}: {proto} command {c} outside 0..63")
            if proto.upper() == "RC5X":
                c |= 0x40  # RC5X: field bit = 0, i.e. our rc5 cmd bit 6 set
        return IrCode(proto=ours, addr=a, cmd=c)
    if kind == "raw":
        try:
            hz = int(_need(fields, "frequency", name))
            us = [int(t) for t in _need(fields, "data", name).split()]
        except ValueError as e:
            raise IrImportError(f"signal {name!r}: bad raw number ({e})") from None
        return IrCode(hz=hz, us=normalise_raw(us))
    raise IrImportError(f"signal {name!r}: unknown type {kind!r}")


def parse_flipper(text: str) -> dict[str, IrCode]:
    """Parse .ir text. Duplicate names get _2, _3, ... suffixes in file order."""
    header: dict[str, str] = {}
    blocks: list[dict[str, str]] = []
    cur: dict[str, str] | None = None
    for lineno, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line:
            continue
        if line.startswith("#"):
            cur = None
            continue
        key, sep, val = line.partition(":")
        if not sep:
            raise IrImportError(f"line {lineno}: expected 'key: value', got {line!r}")
        key, val = key.strip().lower(), val.strip()
        if key == "name":
            cur = {"name": val}
            blocks.append(cur)
        elif cur is not None:
            cur[key] = val
        elif blocks or key not in ("filetype", "version"):
            raise IrImportError(f"line {lineno}: field {key!r} outside a signal")
        else:
            header[key] = val
    if header.get("filetype") not in ("IR signals file", "IR library file"):
        raise IrImportError(f"bad or missing Filetype header: {header.get('filetype')!r}")
    if header.get("version") != "1":
        raise IrImportError(f"unsupported Version: {header.get('version')!r}")
    out: dict[str, IrCode] = {}
    for fields in blocks:
        code = _build(fields)
        name, n = fields["name"], 1
        while name in out:
            n += 1
            name = f"{fields['name']}_{n}"
        out[name] = code
    return out


def parse_flipper_file(path: str | Path) -> dict[str, IrCode]:
    return parse_flipper(Path(path).read_text(encoding="utf-8"))
