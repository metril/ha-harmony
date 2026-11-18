"""Key tables: the Elite's 35 physical keys (from proto/schema.yaml via harmony_proto.KEYS)."""

from __future__ import annotations

from .harmony_proto import KEY_BY_NAME, KEYS, SOFT_KEY_BASE

# A key's YAML name and HA event type is its snake_case form (`volume_up`);
# the wire carries the logical id (< 128; 128+ are page soft keys).
KEY_CODES: dict[str, int] = {k[3]: k[0] for k in KEYS}
KEY_NAMES: dict[int, str] = {code: name for name, code in KEY_CODES.items()}
UNKNOWN_KEY = "unknown"
EVENT_TYPES: list[str] = [*KEY_NAMES.values(), UNKNOWN_KEY]
KEY_ACTIONS = {0: "press", 1: "release", 2: "hold"}


def key_code(name: str) -> int | None:
    """Code for a key's event name (`volume_up`) or stock name (`VolumeUp`), or `key_<n>` (n < 128)."""
    if name in KEY_CODES:
        return KEY_CODES[name]
    if name in KEY_BY_NAME:
        return KEY_BY_NAME[name][0]
    if name.startswith("key_") and name[4:].isdecimal() and int(name[4:]) < SOFT_KEY_BASE:
        return int(name[4:])
    return None
