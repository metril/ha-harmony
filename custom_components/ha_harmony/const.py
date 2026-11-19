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
CONF_HOLD_EVENTS = "hold_events"  # option: also write an event for each hold repeat
CONF_ALL_COMMAND_BUTTONS = "all_command_buttons"  # option: command buttons even for devices over the limit
COMMAND_BUTTON_LIMIT = 200  # a device with more commands gets no (new) command buttons unless the option is set
CONF_ADDON_SLUG = "addon_slug"
CONF_ADDON_UUID = "addon_uuid"
ADDON_SLUG = "ha_harmony_editor"
BUNDLE_MAX_BYTES = 4 * 1024 * 1024  # the source of a config as large as a hub takes (its memory budget), with headroom
LINK_FETCH_TIMEOUT = 20.0  # seconds, whole request
LINK_WAIT_TIMEOUT = 70.0  # the add-on holds a wait for 55 s
FIRST_APPLY_WAIT = 5.0  # setup waits at most this long for the first config when nothing is cached
LINK_BACKOFF_MIN = 5.0
LINK_BACKOFF_MAX = 120.0
STORE_VERSION = 1
TILE_DEBOUNCE = 0.1  # one tile flush per this many seconds (bursts coalesce); tests shorten it
TILE_WINDOW = 8  # tile_update requests in flight at once during a flush
INFO_WAIT = 1.5  # seconds a fresh remote gets to announce its page format before v1 pages are pushed
CAP_TILE_STATE = 1  # remote_info.caps bit 0 (bit 1 = page store / paging)
CAP_PAGING = 2
TILE_TEXT_MAX = 31  # bytes, docs/proto/page-format.md

HOLD_REPEAT = 0.4  # seconds between the resends of `remote.send_command hold_secs` (the remote's hold cadence)

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
