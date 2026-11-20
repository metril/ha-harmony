"""What a tile shows for an entity: (state code, short text), at most 31 bytes, UTF-8 safe.

State codes (docs/proto/page-format.md): 0 value only, 1 off, 2 on, 3 unavailable. Only remotes
that announced the `tile_state` capability get the code; the others keep the legacy text.
"""

from __future__ import annotations

import math

from homeassistant.core import State

from . import const

TILE_VALUE, TILE_OFF, TILE_ON, TILE_UNAVAILABLE = 0, 1, 2, 3
_ONOFF_DOMAINS = frozenset(
    {
        "light",
        "switch",
        "fan",
        "input_boolean",
        "media_player",
        "remote",
        "humidifier",
        "siren",
        "automation",
        "cover",
        "lock",
    }
)
_ON_WORDS = {"on": TILE_ON, "off": TILE_OFF, "open": TILE_ON, "closed": TILE_OFF}
_ACTION_DOMAINS = frozenset({"scene", "script", "button", "input_button"})  # state = a timestamp


def cut(text: str, limit: int = const.TILE_TEXT_MAX) -> str:
    """At most `limit` bytes of UTF-8, never splitting a character."""
    return text.encode()[:limit].decode(errors="ignore")


def _number(raw: str, precision: int | None) -> str:
    try:
        x = float(raw)
    except ValueError:
        return raw
    if not math.isfinite(x):
        return raw
    if precision is not None:
        return f"{x:.{max(precision, 0)}f}"
    if x == int(x) and "." not in raw and "e" not in raw.lower():
        return raw
    return f"{x:.1f}".rstrip("0").rstrip(".") if abs(x) < 1000 else f"{x:.0f}"


def tile_text(state: State | None) -> str:
    """Legacy text (remotes without tile_state): state plus unit, cut on a UTF-8 boundary."""
    if state is None:
        return ""
    if state.state in ("unavailable", "unknown"):
        return "--"
    unit = state.attributes.get("unit_of_measurement")
    return cut(f"{state.state} {unit}" if unit else state.state)


def tile_state(state: State | None) -> tuple[int, str]:
    """(code, text) for a remote with the tile_state capability."""
    if state is None or state.state in ("unavailable", "unknown"):
        return TILE_UNAVAILABLE, "--"
    domain = state.entity_id.split(".", 1)[0]
    attrs = state.attributes
    if domain in _ACTION_DOMAINS:
        return TILE_VALUE, ""
    if domain in _ONOFF_DOMAINS and state.state in _ON_WORDS:
        code = _ON_WORDS[state.state]
        text = state.state.capitalize()
        if domain == "light" and code == TILE_ON and isinstance(attrs.get("brightness"), int | float):
            text = f"{round(attrs['brightness'] * 100 / 255)}%"
        elif domain == "cover" and code == TILE_ON and isinstance(attrs.get("current_position"), int):
            text = f"{attrs['current_position']}%"
        return code, cut(text)
    dc = attrs.get("device_class")
    if dc in ("timestamp", "date") and "T" in state.state:
        return TILE_VALUE, cut(state.state[:16].replace("T", " "))
    unit = attrs.get("unit_of_measurement")
    prec = attrs.get("suggested_display_precision")
    value = _number(state.state, prec if isinstance(prec, int) else None)
    if not unit:
        return TILE_VALUE, cut(value.replace("_", " ").capitalize() if value.islower() else value)
    sep = "" if unit in ("%", "°C", "°F", "°") else " "
    return TILE_VALUE, cut(f"{value}{sep}{unit}")
