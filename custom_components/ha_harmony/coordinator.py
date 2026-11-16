"""Push coordinator: hub state lives here, entities subscribe to changes.

Also owns the YAML config lifecycle: load -> compile -> push `set_config` (only when the hub's
config hash differs) -> push pages -> keep tiles in sync with their bound entities.
"""

from __future__ import annotations

import asyncio
import base64
import logging
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from homeassistant.config_entries import ConfigEntry
from homeassistant.core import Event, HomeAssistant, State, callback
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers import device_registry as dr
from homeassistant.helpers import entity_registry as er
from homeassistant.helpers import issue_registry as ir
from homeassistant.helpers.event import EventStateChangedData, async_track_state_change_event
from homeassistant.helpers.update_coordinator import DataUpdateCoordinator

from . import const
from .api import (
    HarmonyAuthError,
    HarmonyCommandError,
    HarmonyConnectionError,
    HarmonyError,
    HarmonyHubClient,
    HarmonyVersionError,
)
from .compiler import Compiled, CompileError, compile_config
from .config_model import ConfigError, load_file, parse_model
from .const import (
    CMD_IR_SEND,
    CMD_POWER_OFF,
    CMD_SET_DEVICE_STATE,
    CMD_START_ACTIVITY,
    CMD_TILE_UPDATE,
    DEFAULT_REMOTE_IDS,
    DOMAIN,
    HUB_DEVICE,
    KEY_ACTIONS,
    KEY_NAMES,
    UNKNOWN_KEY,
)
from .harmony_proto import MsgType

_LOGGER = logging.getLogger(__name__)

KeyListener = Callable[[int, int, str], None]
ISSUE_INVALID = "config_invalid"
ISSUE_REJECTED = "config_rejected"


@dataclass
class DeviceState:
    power: bool | None = None  # None = not reported yet
    input: str | None = None


def tile_text(state: State | None) -> str:
    """Text for a tile: state (+ unit), at most TILE_TEXT_MAX bytes cut on a UTF-8 boundary."""
    if state is None:
        return ""
    if state.state in ("unavailable", "unknown"):
        return "--"
    unit = state.attributes.get("unit_of_measurement")
    text = f"{state.state} {unit}" if unit else state.state
    return text.encode()[: const.TILE_TEXT_MAX].decode(errors="ignore")


def _empty_compiled() -> Compiled:
    return compile_config(parse_model(""))


class HarmonyCoordinator(DataUpdateCoordinator[None]):
    """Holds activities/state; updates arrive by push, never by polling."""

    config_entry: ConfigEntry

    def __init__(self, hass: HomeAssistant, entry: ConfigEntry, client: HarmonyHubClient) -> None:
        super().__init__(hass, _LOGGER, config_entry=entry, name=DOMAIN)
        self.client = client
        self.hub_id: str = client.hub_id or ""
        self.fw_version = client.fw_version
        self.activities: list[str] = list(client.activities)
        self.device_states: dict[str, DeviceState] = {}
        self.battery: dict[int, tuple[int, int]] = {}  # remote -> (percent, millivolts)
        self.remotes: tuple[int, ...] = DEFAULT_REMOTE_IDS
        self.current_activity: str | None = None
        self.last_activity: str | None = None
        self.available = True
        self.compiled: Compiled = _empty_compiled()
        self.managed = False  # a YAML file exists for this hub
        self.last_error: str | None = None
        self.config_pushes = 0  # set_config requests sent (observable by tests)
        self.tile_updates = 0
        self._key_listeners: list[KeyListener] = []
        self._config_listeners: list[Callable[[], None]] = []
        self._sync_lock = asyncio.Lock()
        self._pages_pending = False
        self._tile_unsub: Callable[[], None] | None = None
        self._tile_timers: dict[str, Any] = {}
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
        self._untrack_tiles()
        await super().async_shutdown()
        await self.client.async_close()

    # ------------------------------------------------------------ helpers
    @property
    def config_path(self) -> Path:
        return Path(self.hass.config.path(const.CONFIG_SUBDIR, f"{self.hub_id}.yaml"))

    def key_name(self, code: int) -> str:
        return self.compiled.key_names.get(code) or KEY_NAMES.get(code, UNKNOWN_KEY)

    @property
    def event_types(self) -> list[str]:
        return sorted({*KEY_NAMES.values(), *self.compiled.key_names.values(), UNKNOWN_KEY})

    def async_add_key_listener(self, cb: KeyListener) -> Callable[[], None]:
        self._key_listeners.append(cb)
        return lambda: self._key_listeners.remove(cb) if cb in self._key_listeners else None

    def async_add_config_listener(self, cb: Callable[[], None]) -> Callable[[], None]:
        """Called after every successful (re)load of the YAML, so platforms can add entities."""
        self._config_listeners.append(cb)
        return lambda: self._config_listeners.remove(cb) if cb in self._config_listeners else None

    def expected_unique_ids(self) -> set[str]:
        h = self.hub_id
        ids = {f"{h}_remote", f"{h}_activity"}
        for r in self.remotes:
            ids |= {f"{h}_key_{r}", f"{h}_battery_{r}", f"{h}_voltage_{r}"}
        for dev in self.compiled.devices.values():
            ids |= {f"{h}_dev_{dev.id}_power", f"{h}_dev_{dev.id}_input"}
            ids |= {f"{h}_cmd_{dev.id}_{c}" for c in self.device_commands(dev.id)}
        return ids

    def device_commands(self, device: str) -> list[str]:
        """Commands exposed as buttons: the YAML commands plus power_on/power_off."""
        dev = self.compiled.devices[device]
        extra = ["power_on", "power_off"] if dev.power else []
        return [*dev.commands, *extra]

    # ------------------------------------------------------------ config lifecycle
    def _set_issue(self, key: str, message: str) -> None:
        ir.async_create_issue(
            self.hass,
            DOMAIN,
            f"{key}_{self.hub_id}",
            is_fixable=False,
            severity=ir.IssueSeverity.ERROR,
            translation_key=key,
            translation_placeholders={"hub": self.hub_id, "error": message},
        )

    def _clear_issue(self, key: str) -> None:
        ir.async_delete_issue(self.hass, DOMAIN, f"{key}_{self.hub_id}")

    async def async_load_config(self) -> bool:
        """Read, validate and compile the YAML. On error the previous config stays; returns ok."""
        path = self.config_path
        try:
            model = await self.hass.async_add_executor_job(load_file, path)
            compiled = compile_config(model) if model is not None else _empty_compiled()
        except ConfigError as exc:
            return self._load_failed(str(exc))
        except CompileError as exc:
            return self._load_failed(f"{path}: {exc}")
        self._clear_issue(ISSUE_INVALID)
        self.last_error = None
        self.managed = model is not None
        if not self.managed:
            _LOGGER.info(
                "No config file at %s: using an empty config, the hub's own config is untouched",
                path,
            )
        changed = compiled != self.compiled
        self.compiled = compiled
        if changed:
            self._track_tiles()
            self._prune_registry()
            for cb in list(self._config_listeners):
                cb()
            self.async_update_listeners()
        return True

    def _load_failed(self, message: str) -> bool:
        _LOGGER.error("Invalid ha_harmony config, keeping the previous one: %s", message)
        self.last_error = message
        self._set_issue(ISSUE_INVALID, message)
        return False

    async def async_reload_config(self) -> None:
        """Service entry point: re-read the YAML, push what changed; raises on a bad config."""
        if not await self.async_load_config():
            raise HomeAssistantError(f"Invalid config: {self.last_error}")
        await self.async_sync_config()
        if self.last_error:
            raise HomeAssistantError(f"The hub rejected the config: {self.last_error}")

    async def async_sync_config(self) -> None:
        """Push set_config iff the hub's hash differs, then pages and tile texts. Never raises."""
        async with self._sync_lock:
            if not self.managed or not self.client.connected:
                return
            try:
                if await self._push_config_locked():
                    await self._push_pages_locked()
            except (HarmonyConnectionError, HomeAssistantError) as exc:
                _LOGGER.warning("Could not sync the config to the hub (will retry): %s", exc)

    async def _push_config_locked(self) -> bool:
        """True when the hub runs the compiled config afterwards."""
        compiled = self.compiled
        want = compiled.config_hash
        if self.client.config_hash == want:
            _LOGGER.debug("Hub already runs config %s; not pushing", want)
            return True
        self.config_pushes += 1
        try:
            ack = await self.client.async_send(MsgType.SET_CONFIG, {"config": compiled.hub_json})
        except HarmonyCommandError as exc:
            ack = {"ok": False, "message": exc.message}
        if not ack.get("ok"):
            msg = str(ack.get("message") or "rejected without a reason")
            _LOGGER.error("The hub rejected the config: %s", msg)
            self.last_error = msg
            self._set_issue(ISSUE_REJECTED, msg)
            return False
        self.last_error = None
        self._clear_issue(ISSUE_REJECTED)
        self.client.config_hash = want
        # activity_list is not echoed to the sender: apply it ourselves
        self._set_activities(compiled.activities)
        return True

    def _set_activities(self, names: list[str]) -> None:
        self.activities = list(names)
        if self.current_activity not in self.activities:
            self.current_activity = None
        if self.last_activity not in self.activities:
            self.last_activity = None
        self.async_update_listeners()

    async def async_push_pages(self) -> None:
        async with self._sync_lock:
            await self._push_pages_locked()

    async def _push_pages_locked(self) -> None:
        self._pages_pending = False
        for remote in self.remotes:
            for idx, data in enumerate(self.compiled.pages):
                payload = {"remote": remote, "page": idx, "data": base64.b64encode(data).decode()}
                try:
                    await self.client.async_send(MsgType.PAGE_PUSH, payload)
                except HarmonyCommandError as exc:
                    if exc.code == "no_remote":
                        self._pages_pending = True  # retried when the remote shows signs of life
                        _LOGGER.debug("No remote attached; page %s not delivered yet", idx)
                        return
                    _LOGGER.warning("Hub refused page %s: %s", idx, exc)
        if self.compiled.pages:
            for tile in self.compiled.tiles:
                await self._send_tile(tile.entity_id, only=tile)

    def _prune_registry(self) -> None:
        """Remove entities/devices of this entry that the current config no longer defines."""
        entry_id = self.config_entry.entry_id
        want = self.expected_unique_ids()
        ereg = er.async_get(self.hass)
        for ent in er.async_entries_for_config_entry(ereg, entry_id):
            if ent.unique_id not in want:
                ereg.async_remove(ent.entity_id)
        dreg = dr.async_get(self.hass)
        keep = {f"{self.hub_id}_dev_{d}" for d in self.compiled.devices}
        for dev in dr.async_entries_for_config_entry(dreg, entry_id):
            ids = {i for dom, i in dev.identifiers if dom == DOMAIN}
            if any("_dev_" in i for i in ids) and not ids & keep:
                dreg.async_update_device(dev.id, remove_config_entry_id=entry_id)

    # ------------------------------------------------------------ tiles
    def _untrack_tiles(self) -> None:
        if self._tile_unsub:
            self._tile_unsub()
            self._tile_unsub = None
        for timer in self._tile_timers.values():
            timer.cancel()
        self._tile_timers.clear()

    def _track_tiles(self) -> None:
        self._untrack_tiles()
        entities = sorted({t.entity_id for t in self.compiled.tiles})
        if entities:
            self._tile_unsub = async_track_state_change_event(
                self.hass, entities, self._on_tile_entity
            )

    @callback
    def _on_tile_entity(self, event: Event[EventStateChangedData]) -> None:
        entity_id = event.data["entity_id"]
        if (timer := self._tile_timers.pop(entity_id, None)) is not None:
            timer.cancel()
        self._tile_timers[entity_id] = self.hass.loop.call_later(
            const.TILE_DEBOUNCE, self._fire_tile, entity_id
        )

    @callback
    def _fire_tile(self, entity_id: str) -> None:
        self._tile_timers.pop(entity_id, None)
        self.config_entry.async_create_background_task(
            self.hass, self._send_tile(entity_id), name=f"{DOMAIN} tile {entity_id}"
        )

    async def _send_tile(self, entity_id: str, only: Any = None) -> None:
        text = tile_text(self.hass.states.get(entity_id))
        for tile in self.compiled.tiles:
            if tile.entity_id != entity_id or (only is not None and tile is not only):
                continue
            for remote in self.remotes:
                args = {"remote": remote, "page": tile.page, "tile": tile.widget, "text": text}
                try:
                    await self.client.async_send(
                        MsgType.COMMAND,
                        {"device": HUB_DEVICE, "command": CMD_TILE_UPDATE, "args": args},
                    )
                    self.tile_updates += 1
                except HarmonyCommandError as exc:
                    _LOGGER.debug("tile_update for %s not delivered: %s", entity_id, exc)
                except HarmonyConnectionError as exc:
                    _LOGGER.debug("tile_update for %s failed: %s", entity_id, exc)
                    return

    # ------------------------------------------------------------ hub -> HA
    @callback
    def _on_state(self, connected: bool, err: HarmonyError | None) -> None:
        self.available = connected
        if connected:
            # hello was re-sent by the client: refresh what the hub told us.
            self._set_activities(self.client.activities)
            self.config_entry.async_create_background_task(
                self.hass, self.async_sync_config(), name=f"{DOMAIN} resync"
            )
        if isinstance(err, HarmonyAuthError):
            self.config_entry.async_start_reauth(self.hass)
        elif isinstance(err, HarmonyVersionError):
            _LOGGER.error("Hub protocol version mismatch, not reconnecting: %s", err)
            # Reload so setup surfaces the explicit ConfigEntryError (SETUP_ERROR, no retry loop).
            self.hass.config_entries.async_schedule_reload(self.config_entry.entry_id)
        self.async_update_listeners()

    def _remote_alive(self) -> None:
        if self._pages_pending:
            self._pages_pending = False
            self.config_entry.async_create_background_task(
                self.hass, self.async_push_pages(), name=f"{DOMAIN} pages"
            )

    @callback
    def _on_push(self, mtype: MsgType, data: dict[str, Any]) -> None:
        if mtype is MsgType.STATE_UPDATE:
            idx = data["activity"]
            if not data["power"]:
                self.current_activity = None
                self.async_update_listeners()
                return
            if idx >= len(self.activities):
                _LOGGER.warning(
                    "state_update for activity index %s but only %s activities are known"
                    " (%s); ignoring it",
                    idx,
                    len(self.activities),
                    self.activities,
                )
                return
            name = self.activities[idx]
            self.last_activity = name
            self.current_activity = name
            self.async_update_listeners()
        elif mtype is MsgType.KEY_EVENT:
            self._remote_alive()
            action = KEY_ACTIONS.get(data["action"], "press")
            for cb in list(self._key_listeners):
                cb(data["remote"], data["key"], action)
        elif mtype is MsgType.ACTIVITY_LIST:
            self.client.config_hash = data.get("config_hash")
            acts = data["activities"]
            self._set_activities([a for a in acts if isinstance(a, str)])
        elif mtype is MsgType.DEVICE_STATE:
            self.device_states[data["device"]] = DeviceState(data["power"], data.get("input"))
            self.async_update_listeners()
        elif mtype is MsgType.BATTERY:
            self._remote_alive()
            self.battery[data["remote"]] = (data["percent"], data["millivolts"])
            self.async_update_listeners()

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

    async def async_send_ir(self, action: dict[str, Any]) -> None:
        await self.async_command(HUB_DEVICE, CMD_IR_SEND, action)

    async def async_set_device_state(
        self, device: str, power: bool | None = None, input: str | None | bool = False
    ) -> None:
        """Tell the hub a device changed by hand (no IR). `input=None` = unknown, False = leave."""
        args: dict[str, Any] = {"device": device}
        if power is not None:
            args["power"] = power
        if input is not False:
            args["input"] = input
        await self.async_command(HUB_DEVICE, CMD_SET_DEVICE_STATE, args)

    async def async_device_command(self, device: str, command: str) -> None:
        """`remote.send_command device=...`: send a device command's IR and track what it did."""
        if device == HUB_DEVICE:
            await self.async_command(device, command)
            return
        try:
            ir_action, patch = self.compiled.resolve(device, command)
        except KeyError:
            raise HomeAssistantError(f"Unknown device command {device}.{command}") from None
        await self.async_send_ir(ir_action)
        state = self.device_states.get(device, DeviceState())
        if patch.get("toggle"):
            await self.async_set_device_state(device, power=not state.power)
        elif patch:
            await self.async_set_device_state(
                device, power=patch.get("power"), input=patch.get("input", False)
            )
