"""Pronto hex (0000 learned format) validation and conversion."""

from __future__ import annotations

import re

from .model import IrImportError

_CLOCK_US = 0.241246  # Pronto clock unit: period = freq_word * 0.241246 us
_WORD = re.compile(r"[0-9A-Fa-f]{4}")


def normalise_pronto(text: str) -> str:
    """Return upper-case, single-space-separated words; raise IrImportError if invalid."""
    words = text.split() if isinstance(text, str) else []
    if len(words) < 6:
        raise IrImportError("pronto: too short")
    if not all(_WORD.fullmatch(w) for w in words):
        raise IrImportError("pronto: words must be 4 hex digits")
    vals = [int(w, 16) for w in words]
    if vals[0] != 0:
        raise IrImportError(f"pronto: unsupported format {words[0]} (only 0000 learned)")
    if vals[1] == 0:
        raise IrImportError("pronto: zero frequency word")
    once, rep = vals[2], vals[3]
    if once + rep == 0:
        raise IrImportError("pronto: no burst pairs")
    if len(words) != 4 + 2 * (once + rep):
        raise IrImportError(
            f"pronto: burst pair counts {once}+{rep} do not match {len(words) - 4} data words"
        )
    return " ".join(w.upper() for w in words)


def pronto_to_raw(text: str) -> tuple[int, list[int]]:
    """Convert to (carrier Hz, [mark, space, ...] us): once sequence followed by repeat sequence."""
    vals = [int(w, 16) for w in normalise_pronto(text).split()]
    period = vals[1] * _CLOCK_US
    return round(1_000_000 / period), [max(1, round(v * period)) for v in vals[4:]]
