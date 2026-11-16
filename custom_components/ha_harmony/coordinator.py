"""Push coordinator: hub state lives here, entities subscribe to changes."""

from __future__ import annotations

import logging
from collections.abc import Callable
from typing import Any

from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant, callback
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers.update_coordinator import DataUpdateCoordinator

from .api import (
    HarmonyAuthError,
    HarmonyCommandError,
    HarmonyConnectionError,
    HarmonyError,
    HarmonyHubClient,
    HarmonyVersionError,
)
from .const import (
    CMD_POWER_OFF,
    CMD_START_ACTIVITY,
    DEFAULT_REMOTE_IDS,
    DOMAIN,
    HUB_DEVICE,
    KEY_ACTIONS,
)
from .harmony_proto import MsgType

_LOGGER = logging.getLogger(__name__)

KeyListener = Callable[[int, int, str], None]


class HarmonyCoordinator(DataUpdateCoordinator[None]):
    """Holds activities/state; updates arrive by push, never by polling."""

    config_entry: ConfigEntry

    def __init__(self, hass: HomeAssistant, entry: ConfigEntry, client: HarmonyHubClient) -> None:
        super().__init__(hass, _LOGGER, config_entry=entry, name=DOMAIN)
        self.client = client
        self.hub_id: str = client.hub_id or ""
        self.fw_version = client.fw_version
        self.activities: list[str] = list(client.activities)
        self.devices: list[str] = []
        self.remotes: tuple[int, ...] = DEFAULT_REMOTE_IDS
        self.current_activity: str | None = None
        self.last_activity: str | None = None
        self.available = True
        self._key_listeners: list[KeyListener] = []
        self._unsubs = [
            client.async_subscribe(self._on_push),
            client.async_subscribe_state(self._on_state),
        ]
        self.async_set_updated_data(None)

    async def _async_update_data(self) -> None:  # push only
        return None

    async def async_shutdown(self) -> None:
        for unsub in self._unsubs:
            unsub()
        self._unsubs = []
        await super().async_shutdown()
        await self.client.async_close()

    # ------------------------------------------------------------ listeners
    def async_add_key_listener(self, cb: KeyListener) -> Callable[[], None]:
        self._key_listeners.append(cb)
        return lambda: self._key_listeners.remove(cb) if cb in self._key_listeners else None

    # ------------------------------------------------------------ hub -> HA
    @callback
    def _on_state(self, connected: bool, err: HarmonyError | None) -> None:
        self.available = connected
        if connected:
            # hello was re-sent by the client: refresh what the hub told us.
            self.activities = list(self.client.activities)
            if self.current_activity not in self.activities:
                self.current_activity = None
            self.last_activity = (
                self.last_activity if self.last_activity in self.activities else None
            )
        if isinstance(err, HarmonyAuthError):
            self.config_entry.async_start_reauth(self.hass)
        elif isinstance(err, HarmonyVersionError):
            _LOGGER.error("Hub protocol version mismatch, not reconnecting: %s", err)
            # Reload so setup surfaces the explicit ConfigEntryError (SETUP_ERROR, no retry loop).
            self.hass.config_entries.async_schedule_reload(self.config_entry.entry_id)
        self.async_update_listeners()

    @callback
    def _on_push(self, mtype: MsgType, data: dict[str, Any]) -> None:
        if mtype is MsgType.STATE_UPDATE:
            idx = data["activity"]
            if not data["power"]:
                self.current_activity = None
                self.async_update_listeners()
                return
            if idx >= len(self.activities):
                _LOGGER.debug("state_update for unknown activity index %s", idx)
                return
            name = self.activities[idx]
            self.last_activity = name
            self.current_activity = name
            self.async_update_listeners()
        elif mtype is MsgType.KEY_EVENT:
            action = KEY_ACTIONS.get(data["action"], "press")
            for cb in list(self._key_listeners):
                cb(data["remote"], data["key"], action)

    # ------------------------------------------------------------ HA -> hub
    async def async_command(
        self, device: str, command: str, args: dict[str, Any] | None = None
    ) -> None:
        payload: dict[str, Any] = {"device": device, "command": command}
        if args is not None:
            payload["args"] = args
        try:
            res = await self.client.async_send(MsgType.COMMAND, payload)
        except HarmonyCommandError as exc:
            raise HomeAssistantError(f"Hub rejected command: {exc.message}") from exc
        except HarmonyConnectionError as exc:
            raise HomeAssistantError(f"Hub unreachable: {exc}") from exc
        if not res.get("ok"):
            raise HomeAssistantError(f"Hub command failed: {res.get('message', 'unknown error')}")

    async def async_start_activity(self, name: str) -> None:
        await self.async_command(HUB_DEVICE, CMD_START_ACTIVITY, {"name": name})

    async def async_power_off(self) -> None:
        await self.async_command(HUB_DEVICE, CMD_POWER_OFF)
