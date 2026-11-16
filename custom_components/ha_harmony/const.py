"""Constants for the ha_harmony integration."""

from __future__ import annotations

from typing import Any

DOMAIN = "ha_harmony"
DEFAULT_PORT = 8099
CONF_TOKEN = "token"
CLIENT_NAME = "home-assistant"

OFF_OPTION = "Off"
HUB_DEVICE = "hub"
CMD_START_ACTIVITY = "start_activity"
CMD_POWER_OFF = "power_off"

# Remotes assumed paired until the hub reports its pairing table.
DEFAULT_REMOTE_IDS: tuple[int, ...] = (1,)

# Fixed key-code -> event name map (skeleton; later driven by the hub's key map).
KEY_NAMES: dict[int, str] = {
    1: "menu",
    2: "ok",
    3: "back",
    4: "up",
    5: "down",
    6: "left",
    7: "right",
    8: "home",
}
UNKNOWN_KEY = "unknown"
EVENT_TYPES: list[str] = [*KEY_NAMES.values(), UNKNOWN_KEY]
KEY_ACTIONS = {0: "press", 1: "release", 2: "hold"}

# Reconnect/heartbeat tuning (seconds). Hub drops connections silent for 90 s.
# Mutable so tests can shorten it.
CLIENT_KWARGS: dict[str, Any] = {
    "backoff_min": 1.0,
    "backoff_max": 30.0,
    "heartbeat_interval": 30.0,
    "pong_timeout": 10.0,
    "request_timeout": 10.0,
}
