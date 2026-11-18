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
CONFIG_SUBDIR = "ha_harmony"  # only to spot a leftover <config>/ha_harmony/<hub_id>.yaml
CONF_ADDON_HOST = "addon_host"
CONF_ADDON_PORT = "addon_port"
CONF_ADDON_TOKEN = "addon_token"
CONF_ADDON_SLUG = "addon_slug"
CONF_ADDON_UUID = "addon_uuid"
ADDON_SLUG = "ha_harmony_editor"
BUNDLE_MAX_BYTES = 2 * 1024 * 1024  # a hub config is <= 128 KiB compiled; sources stay far below
LINK_FETCH_TIMEOUT = 20.0  # seconds, whole request
LINK_WAIT_TIMEOUT = 70.0  # the add-on holds a wait for 55 s
LINK_BACKOFF_MIN = 5.0
LINK_BACKOFF_MAX = 120.0
STORE_VERSION = 1
TILE_DEBOUNCE = 0.1  # one tile flush per this many seconds (bursts coalesce); tests shorten it
INFO_WAIT = 1.5  # seconds a fresh remote gets to announce its page format before v1 pages are pushed
CAP_TILE_STATE = 1  # remote_info.caps bit 0 (bit 1 = page store / paging)
CAP_PAGING = 2
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
