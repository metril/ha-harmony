"""Constants for the ha_harmony integration."""

from __future__ import annotations

from typing import Any

DOMAIN = "ha_harmony"
DEFAULT_PORT = 8099
CONF_TOKEN = "token"
CLIENT_NAME = "home-assistant"
CONFIG_SUBDIR = "ha_harmony"  # <HA config dir>/ha_harmony/<hub_id>.yaml
TILE_DEBOUNCE = 0.5  # seconds between a bound entity changing and its tile_update; tests shorten it
TILE_TEXT_MAX = 31  # bytes, docs/proto/page-format.md

OFF_OPTION = "Off"
HUB_DEVICE = "hub"
CMD_START_ACTIVITY = "start_activity"
CMD_POWER_OFF = "power_off"
CMD_IR_SEND = "ir_send"
CMD_TILE_UPDATE = "tile_update"
CMD_SET_DEVICE_STATE = "set_device_state"

# Remotes assumed paired until the hub reports its pairing table.
DEFAULT_REMOTE_IDS: tuple[int, ...] = (1,)

# PROVISIONAL: symbolic key name -> physical key code (0..127). The Elite's real key codes are not
# known yet; this is the ONE table to fix. YAML key names, event types and event names are all
# derived from it. Codes must be unique and in 0..127 (128+ are page soft keys).
KEY_CODES: dict[str, int] = {
    "menu": 1,
    "ok": 2,
    "back": 3,
    "up": 4,
    "down": 5,
    "left": 6,
    "right": 7,
    "home": 8,
    "vol_up": 9,
    "vol_down": 10,
    "mute": 11,
    "ch_up": 12,
    "ch_down": 13,
    "play": 14,
    "pause": 15,
    "stop": 16,
    "rewind": 17,
    "forward": 18,
    "red": 19,
    "green": 20,
    "yellow": 21,
    "blue": 22,
    **{f"num_{n}": 30 + n for n in range(10)},
}
KEY_NAMES: dict[int, str] = {code: name for name, code in KEY_CODES.items()}
SOFT_KEY_BASE = 128  # page widget wire id w -> key code 128 + w
UNKNOWN_KEY = "unknown"
EVENT_TYPES: list[str] = [*KEY_NAMES.values(), UNKNOWN_KEY]
KEY_ACTIONS = {0: "press", 1: "release", 2: "hold"}


def key_code(name: str) -> int | None:
    """Code for a symbolic key name, or for the generic `key_<n>` form (n in 0..127)."""
    if name in KEY_CODES:
        return KEY_CODES[name]
    if name.startswith("key_") and name[4:].isdecimal() and int(name[4:]) < SOFT_KEY_BASE:
        return int(name[4:])
    return None


# Reconnect/heartbeat tuning (seconds). Hub drops connections silent for 90 s.
# Mutable so tests can shorten it.
CLIENT_KWARGS: dict[str, Any] = {
    "backoff_min": 1.0,
    "backoff_max": 30.0,
    "heartbeat_interval": 30.0,
    "pong_timeout": 10.0,
    "request_timeout": 10.0,
}
