"""Constants for the ha_harmony integration."""

from __future__ import annotations

from typing import Any

from .harmony_core.harmony_proto import SOFT_KEY_BASE  # noqa: F401  (re-exported)
from .harmony_core.keys import (  # noqa: F401  (re-exported)
    EVENT_TYPES,
    KEY_ACTIONS,
    KEY_CODES,
    KEY_NAMES,
    UNKNOWN_KEY,
    key_code,
)

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
CMD_DEVICE_COMMAND = "device_command"
CMD_TILE_UPDATE = "tile_update"
CMD_SET_DEVICE_STATE = "set_device_state"

# Remotes assumed paired until the hub reports its pairing table.
DEFAULT_REMOTE_IDS: tuple[int, ...] = (1,)

# Reconnect/heartbeat tuning (seconds). Hub drops connections silent for 90 s.
# Mutable so tests can shorten it.
CLIENT_KWARGS: dict[str, Any] = {
    "backoff_min": 1.0,
    "backoff_max": 30.0,
    "heartbeat_interval": 30.0,
    "pong_timeout": 10.0,
    "request_timeout": 10.0,
}
