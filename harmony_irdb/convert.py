"""Archive codeset command -> one config-format-2 IR action.

`proto` is used ONLY for protocols in `EXACT`; each entry is proven exact by
tests/py/test_irdb_convert.py (our reference encoder vs the command's own pronto: carrier, frame and
repeat frame). Everything else becomes `pronto`; a command with neither a mapping nor a pronto
is skipped.

Hub semantics (hub/engine/irout.c, hub/ir/encode.c): a pronto's once sequence is sent when a key
is pressed; while the key is held the REPEAT sequence (the pronto's own repeat part, or the whole
once sequence when it has none) is re-sent. So a distinct repeat frame IS representable, as the
repeat part of one pronto. The archive's `prontoRepeat` is a standalone copy of that repeat part
(identical to the `pronto` repeat part in every sampled command); we fold it into the pronto.
The hub rejects any duration above 65535 us, so longer gaps are clamped (noted on the action).
"""

from __future__ import annotations

import re
from dataclasses import dataclass

MAX_US = 0xFFFF
MAX_PRONTO_WORDS = 520  # compiler LIMITS["MAX_PRONTO"]
MAX_DURATIONS = 512  # hub HP_IR_MAX_DUR (once + repeat)

_KEYCODE = re.compile(r"^G:([^:()]{1,80}):\(([^()]*)\)\(([^()]*)\)\(([^()]*)\):\d+$")
_WORD = re.compile(r"[0-9A-Fa-f]{4}")


@dataclass(frozen=True)
class Converted:
    action: dict
    form: str  # "proto" | "pronto"
    notes: tuple[str, ...] = ()


@dataclass(frozen=True)
class Skipped:
    reason: str


def _rev(v: int, n: int) -> int:
    return int(format(v, f"0{n}b")[::-1], 2)


def _hex(s: str, digits: int) -> int | None:
    if re.fullmatch(rf"0x[0-9A-Fa-f]{{{digits}}}", s):
        return int(s, 16)
    return None


def _sirc(bits: int):
    def f(g: list[str]) -> dict | None:
        if g[0] != "" or g[2] != "":
            return None
        v = _hex(g[1], (bits + 3) // 4)
        if v is None or v >> bits:
            return None
        r = _rev(v, bits)  # the archive hex is wire order; SIRC sends command first, LSB first
        return {"proto": f"sirc{bits}", "addr": r >> 7, "cmd": r & 0x7F}

    return f


def _nec(g: list[str]) -> dict | None:
    if g[1] != "Repeat" or g[2] != "":
        return None
    v = _hex(g[0], 8)
    if v is None:
        return None
    a, c = _rev(v >> 16, 16), _rev(v & 0xFFFF, 16)  # wire bits are LSB first
    if a >> 8 == ~a & 0xFF and c >> 8 == ~c & 0xFF:  # classic NEC: complement bytes
        return {"proto": "nec", "addr": a & 0xFF, "cmd": c & 0xFF}
    return {"proto": "necext", "addr": a, "cmd": c}


# Not exact, so absent: Samsung/LG 32 (the archive's repeat is a short header frame, ours repeats
# the whole frame), RC5/RC6 (the archive fixes the toggle bit; the hub alternates it), Pioneer and
# GoVideo (40 kHz carrier / other timings), Memorex (no repeat data to verify), everything else.
EXACT = {
    "Sony 12 Bit": _sirc(12),
    "Sony 15 Bit": _sirc(15),
    "Sony 20 Bit": _sirc(20),
    "NEC 32 Bit": _nec,
    "Toshiba 32 Bit": _nec,
}


def exact_proto(protocol: object, keycode: object) -> dict | None:
    fn = EXACT.get(protocol) if isinstance(protocol, str) else None
    m = _KEYCODE.match(keycode) if isinstance(keycode, str) else None
    if fn is None or m is None or m.group(1) != protocol:
        return None
    return fn(list(m.groups()[1:]))


# ---- pronto
def _parse(text: object) -> list[int]:
    words = text.split() if isinstance(text, str) else []
    if len(words) < 6 or not all(_WORD.fullmatch(w) for w in words):
        raise ValueError("malformed pronto")
    v = [int(w, 16) for w in words]
    if v[0] != 0 or v[1] == 0 or v[2] + v[3] == 0 or len(v) != 4 + 2 * (v[2] + v[3]):
        raise ValueError("unsupported pronto structure")
    return v


def _fmt(v: list[int]) -> str:
    return " ".join(f"{w:04X}" for w in v)


def _clamp(v: list[int]) -> int:
    """Clamp words whose duration the hub would reject (> 65535 us); returns how many changed."""
    per = v[1] * 241246
    wmax = (MAX_US * 1_000_000 + 499_999) // per
    n = 0
    for i in range(4, len(v)):
        if v[i] > wmax:
            v[i] = wmax
            n += 1
    return n


def _pronto(cmd: dict) -> Converted | Skipped:
    try:
        v = _parse(cmd["pronto"])
    except ValueError as exc:
        return Skipped(str(exc))
    notes: list[str] = []
    if cmd.get("prontoRepeat"):
        try:
            r = _parse(cmd["prontoRepeat"])
        except ValueError:
            notes.append("prontoRepeat malformed, ignored")
        else:
            if r[1] != v[1]:
                notes.append("prontoRepeat has another carrier, ignored")
            else:
                once = v[4 : 4 + 2 * v[2]]
                rep = r[4 + 2 * r[2] :] if r[3] else r[4 : 4 + 2 * r[2]]
                merged = [0, v[1], v[2], len(rep) // 2, *once, *rep]
                if merged != v:
                    notes.append("repeat frame taken from prontoRepeat")
                v = merged
    if v[2] + v[3] > MAX_DURATIONS // 2 or len(v) > MAX_PRONTO_WORDS:
        return Skipped(f"pronto too long for the hub ({len(v)} words)")
    n = _clamp(v)
    if n:
        notes.append(f"{n} gap(s) clamped to 65535 us")
    return Converted({"pronto": _fmt(v)}, "pronto", tuple(notes))


def convert_command(cmd: object) -> Converted | Skipped:
    if not isinstance(cmd, dict):
        return Skipped("not a command object")
    proto = exact_proto(cmd.get("protocol"), cmd.get("keycode"))
    if proto is not None:
        return Converted(proto, "proto")
    if not cmd.get("pronto"):
        return Skipped(f"no pronto and protocol {cmd.get('protocol')!r} has no exact mapping")
    return _pronto(cmd)
