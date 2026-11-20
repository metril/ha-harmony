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
from .harmony_core.theme_build import CAP_THEME_COLOR, CAP_THEME_CUSTOM  # noqa: F401  (re-exported)

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
CONF_HW_ID = "hw_id"  # the hub's stable hardware id (hello_ack.hw_id): the same hub across renames
CONF_HUB_NAME = "name"  # the hub's display name ("" / absent = none)
CONF_CAN_RENAME = "can_rename"  # the firmware understands set_hub_id (stored so the reconfigure form can say so)
CONF_ID_PINNED = "id_pinned"  # hub.conf pins the id on the device
CONF_PREV_ID = "prev_hub_id"  # the id before a rename, sent on add-on polls until the add-on has seen it
HUB_NAME_MAX = 48  # bytes
CONF_ADDON_SLUG = "addon_slug"
CONF_ADDON_UUID = "addon_uuid"
ADDON_SLUG = "ha_harmony_editor"
BUNDLE_MAX_BYTES = 4 * 1024 * 1024  # the source of a config as large as a hub takes (its memory budget), with headroom
LINK_CONNECT_TIMEOUT = 10.0  # seconds to open the channel (or to get a one-shot hello answered)
LINK_REFRESH_TIMEOUT = 20.0  # the reload_config service waits this long for the add-on's answer
FIRST_APPLY_WAIT = 5.0  # setup waits at most this long for the first config when nothing is cached
LINK_BACKOFF_MIN = 1.0  # jittered +-25 %, doubling
LINK_BACKOFF_MAX = 60.0
LINK_STABLE = 30.0  # a channel that lived this long resets the back-off
BT_MAX_INFLIGHT = 8  # bt_cmd frames run at once; more are answered `busy`
STORE_VERSION = 1
TILE_DEBOUNCE = 0.1  # one tile flush per this many seconds (bursts coalesce); tests shorten it
TILE_WINDOW = 8  # tile_update requests in flight at once during a flush
INFO_WAIT = 1.5  # seconds a fresh remote gets to announce its page format before v1 pages are pushed
CAP_TILE_STATE = 1  # remote_info.caps bit 0 (bit 1 = page store / paging)
CAP_PAGING = 2
# bit 2 = CAP_THEME_COLOR (palette + presets), bit 3 = CAP_THEME_CUSTOM (free colours + user theme)
THEME_TRANSPORT = True  # theme_push exists: bit 3 counts (False = an integration without it: bit 2 behaviour only)
PUSH_RETRY_BASE = 15.0  # seconds before the first re-push of a set the remote did not confirm; doubles each time
PUSH_RETRY_MAX = 300.0  # ... up to this
THEME_ACK_WAIT = 20.0  # seconds a pushed custom theme gets to show up as a complete set in the remote's remote_info
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

# Bluetooth (bluetooth.py)
EVENT_BT_REMOTE_KEY = "ha_harmony_bt_remote_key"
BT_LISTEN_DEFAULT = 60  # `ha_harmony.bt_listen` lease, seconds (max 300)
