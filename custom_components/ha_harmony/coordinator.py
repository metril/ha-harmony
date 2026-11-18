"""Push coordinator: hub state lives here, entities subscribe to changes.

Also owns the config lifecycle: the applied config comes from the Harmony Editor add-on (pulled,
re-validated and compiled here, cached in `.storage`) -> push `set_config` (only when the hub's
config hash differs) -> push pages -> keep tiles in sync with their bound entities.
"""

from __future__ import annotations

import asyncio
import base64
import logging
from collections.abc import Callable
from dataclasses import dataclass
from typing import Any

from homeassistant.config_entries import ConfigEntry
from homeassistant.core import Event, HomeAssistant, State, callback
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers import device_registry as dr
from homeassistant.helpers import entity_registry as er
from homeassistant.helpers import issue_registry as ir
from homeassistant.helpers.event import EventStateChangedData, async_track_state_change_event
from homeassistant.helpers.storage import Store
from homeassistant.helpers.update_coordinator import DataUpdateCoordinator

from . import const
from .addon_link import (
    AddonAuthError,
    AddonClient,
    AddonError,
    Bundle,
    BundleRejected,
    NothingApplied,
    compile_source,
    parse_bundle,
)
from .api import (
    HarmonyAuthError,
    HarmonyCommandError,
    HarmonyConnectionError,
    HarmonyError,
    HarmonyHubClient,
    HarmonyVersionError,
)
from .const import (
    CMD_DEVICE_COMMAND,
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
from .harmony_core.compiler import Compiled, compile_config
from .harmony_core.config_model import parse_model
from .harmony_core.harmony_proto import MsgType

_LOGGER = logging.getLogger(__name__)

KeyListener = Callable[[int, int, str], None]
ISSUE_INVALID = "config_invalid"
ISSUE_REJECTED = "config_rejected"
ISSUE_SCHEMA = "bundle_schema"
ISSUE_YAML = "yaml_unused"

# add-on link states (the diagnostic sensor's values)
LINK_STATES = (
    "not_linked",
    "connecting",
    "ok",
    "unreachable",
    "auth_failed",
    "nothing_applied",
    "rejected",
)


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
        # tracked by hub index, never by name: a rename keeps the index, a reorder gets a new
        # state_update from the hub
        self._current_idx: int | None = None
        self._last_idx: int | None = None
        self.available = True
        self.compiled: Compiled = _empty_compiled()
        self.managed = False  # an applied config (cached or fresh) exists for this hub
        # the last applied add-on config: the remote stays configured while the add-on is down
        self._store: Store[dict[str, Any]] = Store(
            hass, const.STORE_VERSION, f"{DOMAIN}.bundle.{self.hub_id}"
        )
        self.rev: str | None = None
        self.applied_at: int | None = None
        self.link_state = "not_linked"
        self._acked: tuple[str, str, str | None] | None = None
        self._link: AddonClient | None = None
        self._apply_lock = asyncio.Lock()
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
    def _activity_name(self, idx: int | None) -> str | None:
        return self.activities[idx] if idx is not None and 0 <= idx < len(self.activities) else None

    @property
    def current_activity(self) -> str | None:
        return self._activity_name(self._current_idx)

    @property
    def last_activity(self) -> str | None:
        return self._activity_name(self._last_idx)

    def key_name(self, code: int) -> str:
        return self.compiled.key_names.get(code) or KEY_NAMES.get(code, UNKNOWN_KEY)

    @property
    def event_types(self) -> list[str]:
        return sorted({*KEY_NAMES.values(), *self.compiled.key_names.values(), UNKNOWN_KEY})

    def async_add_key_listener(self, cb: KeyListener) -> Callable[[], None]:
        self._key_listeners.append(cb)
        return lambda: self._key_listeners.remove(cb) if cb in self._key_listeners else None

    def async_add_config_listener(self, cb: Callable[[], None]) -> Callable[[], None]:
        """Called after every applied config change, so platforms can add entities."""
        self._config_listeners.append(cb)
        return lambda: self._config_listeners.remove(cb) if cb in self._config_listeners else None

    def expected_unique_ids(self) -> set[str]:
        h = self.hub_id
        ids = {f"{h}_remote", f"{h}_activity"}
        if self._link is not None:
            ids.add(f"{h}_addon_link")
        for r in self.remotes:
            ids |= {f"{h}_key_{r}", f"{h}_battery_{r}", f"{h}_voltage_{r}"}
        for dev in self.compiled.devices.values():
            ids |= {f"{h}_dev_{dev.id}_power", f"{h}_dev_{dev.id}_input"}
            ids |= {f"{h}_cmd_{dev.id}:{c}" for c in self.device_commands(dev.id)}
        return ids

    def device_commands(self, device: str) -> list[str]:
        """Commands exposed as buttons: the configured commands plus power_on/power_off."""
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

    # -- add-on feed
    @property
    def has_addon_link(self) -> bool:
        return self._link is not None

    def async_set_link_state(self, state: str) -> None:
        if state != self.link_state:
            self.link_state = state
            self.async_update_listeners()

    async def async_load_cached(self) -> bool:
        """Use the last applied config from `.storage` (re-validated, like any bundle)."""
        try:
            data = await self._store.async_load()
        except (OSError, ValueError) as exc:
            _LOGGER.warning("The cached add-on config is unreadable: %s", exc)
            return False
        if not isinstance(data, dict):
            return False
        try:
            compiled = await self.hass.async_add_executor_job(
                compile_source, data.get("config"), self.hub_id
            )
        except BundleRejected as exc:
            _LOGGER.warning("The cached add-on config is unusable and ignored: %s", exc)
            return False
        rev, at = data.get("rev"), data.get("applied_at")
        self.rev = rev if isinstance(rev, str) else None
        self.applied_at = at if type(at) is int else None
        self.managed = True
        self._apply_compiled(compiled)
        return True

    async def async_process_raw(self, raw: bytes) -> Bundle | None:
        """Validate + compile a pulled bundle and, only if it is good, make it the config.
        A bad bundle changes nothing (cache, entities, hub); raises BundleRejected."""
        async with self._apply_lock:
            try:
                bundle = await self.hass.async_add_executor_job(parse_bundle, raw, self.hub_id)
            except BundleRejected as exc:
                self._reject(exc)
                raise
            self._clear_issue(ISSUE_INVALID)
            self._clear_issue(ISSUE_SCHEMA)
            self.last_error = None
            changed = bundle.rev != self.rev or not self.managed
            self.rev, self.applied_at, self.managed = bundle.rev, bundle.applied_at, True
            if changed:
                await self._store.async_save(
                    {"rev": bundle.rev, "applied_at": bundle.applied_at, "config": bundle.source}
                )
            self._apply_compiled(bundle.compiled)
            self.async_set_link_state("ok")
        await self.async_sync_config()
        return bundle

    def _ack_later(self, rev: str | None, result: str, detail: str | None = None) -> None:
        """Report to the add-on what became of a bundle. Background, deduplicated, never fatal."""
        if self._link is None or rev is None or self._acked == (rev, result, detail):
            return
        client = self._link

        async def _send() -> None:
            try:
                await client.ack(self.hub_id, rev, result, detail)
            except AddonError as exc:
                _LOGGER.debug("Could not acknowledge config %s to the add-on: %s", rev, exc)
            except Exception:
                _LOGGER.debug("Unexpected error acknowledging config %s", rev, exc_info=True)
            else:
                self._acked = (rev, result, detail)

        self.config_entry.async_create_background_task(
            self.hass, _send(), f"{DOMAIN} add-on ack {self.hub_id}"
        )

    def _reject(self, exc: BundleRejected) -> None:
        _LOGGER.error("Rejected the add-on config, keeping the previous one: %s", exc)
        detail = "schema not supported" if exc.schema else f"invalid config: {exc}"
        self._ack_later(exc.rev, "error", detail[:200])
        self.last_error = str(exc)
        self._set_issue(ISSUE_SCHEMA if exc.schema else ISSUE_INVALID, str(exc))
        self.async_set_link_state("rejected")

    def async_start_link(self, client: AddonClient) -> None:
        self._link = client
        self.link_state = "connecting"
        self.config_entry.async_create_background_task(
            self.hass, self._link_loop(client), f"{DOMAIN} add-on link {self.hub_id}"
        )

    async def async_initial_fetch(self) -> None:
        """First run with nothing cached: try once so entities exist at setup; never raises."""
        if self._link is None or self.managed:
            return
        try:
            await self.async_process_raw(await self._link.fetch(self.hub_id))
        except AddonError as exc:
            self._note_link_failure(exc)

    async def async_refresh_from_addon(self) -> None:
        """Service entry point: pull now, apply, push what changed; raises with the reason."""
        if self._link is None:
            raise HomeAssistantError("This hub is not linked to the Harmony Editor add-on")
        try:
            await self.async_process_raw(await self._link.fetch(self.hub_id))
        except NothingApplied:
            self.async_set_link_state("nothing_applied")
            raise
        except AddonError as exc:
            if not isinstance(exc, BundleRejected):
                self._note_link_failure(exc)
            raise
        if self.last_error:
            raise HomeAssistantError(f"The hub rejected the config: {self.last_error}")

    def _note_link_failure(self, exc: AddonError) -> bool:
        """Record a failed pull; True when this changed the state (so it is logged once)."""
        if isinstance(exc, AddonAuthError):
            state = "auth_failed"
        elif isinstance(exc, NothingApplied):
            state = "nothing_applied"
        elif isinstance(exc, BundleRejected):
            state = "rejected"
        else:
            state = "unreachable"
        first = state != self.link_state
        self.async_set_link_state(state)
        if first and state in ("unreachable", "auth_failed"):
            _LOGGER.warning(
                "Harmony Editor add-on: %s. Staying on the %s config",
                exc,
                "cached" if self.managed else "hub's own",
            )
        return first

    async def _link_loop(self, client: AddonClient) -> None:
        """Long-poll the add-on for newer applied configs; back off while it is unavailable."""
        loop = asyncio.get_running_loop()
        known: str | None = self.rev  # last rev handled (applied or rejected); "" = none applied
        delay = const.LINK_BACKOFF_MIN
        while True:
            started = loop.time()
            try:
                if known is None:
                    raw: bytes | None = await client.fetch(self.hub_id)
                else:
                    raw = await client.wait(self.hub_id, known)
                if self.link_state in ("unreachable", "auth_failed"):
                    _LOGGER.info("Harmony Editor add-on is reachable again")
                if raw is not None:
                    try:
                        bundle = await self.async_process_raw(raw)
                    except BundleRejected as exc:
                        if exc.rev is None:
                            raise
                        known = exc.rev  # wait for the next apply rather than re-pulling this one
                    else:
                        known = bundle.rev if bundle else known
                elif loop.time() - started < 0.5:
                    await asyncio.sleep(1)  # an add-on answering instantly with nothing
                if self.link_state in (
                    "unreachable",
                    "auth_failed",
                    "connecting",
                    "nothing_applied",
                ):
                    self.async_set_link_state("ok" if self.managed else "connecting")
                delay = const.LINK_BACKOFF_MIN
            except NothingApplied as exc:
                self._note_link_failure(exc)
                known = ""
                await asyncio.sleep(delay)
            except AddonError as exc:
                self._note_link_failure(exc)
                await asyncio.sleep(delay)
                delay = min(delay * 2, const.LINK_BACKOFF_MAX)
            except Exception:
                _LOGGER.exception("Unexpected error in the add-on link; retrying")
                await asyncio.sleep(const.LINK_BACKOFF_MAX)

    def _apply_compiled(self, compiled: Compiled) -> None:
        changed = compiled != self.compiled
        self.compiled = compiled
        if changed:
            self._track_tiles()
            self._prune_registry()
            for cb in list(self._config_listeners):
                cb()
            self.async_update_listeners()

    async def async_sync_config(self) -> None:
        """Push set_config iff the hub's hash differs, then pages and tile texts. Never raises."""
        async with self._sync_lock:
            if not self.managed or not self.client.connected:
                return
            try:
                if await self._push_config_locked():
                    self._ack_later(self.rev, "ok")
                    await self._push_pages_locked()
                else:
                    self._ack_later(
                        self.rev, "error", f"the hub rejected it: {self.last_error}"[:200]
                    )
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
            self._warn_unknown_index()  # no new list is coming: the reported index stays unknown
            return False
        self.last_error = None
        self._clear_issue(ISSUE_REJECTED)
        self.client.config_hash = ack.get("config_hash")  # as the hub reports it
        # activity_list is not echoed to the sender: apply it ourselves
        self._set_activities(compiled.activities)
        return True

    def _set_activities(self, names: list[str]) -> None:
        self.activities = list(names)
        self._warn_unknown_index()

    def _warn_unknown_index(self) -> None:
        if self._current_idx is not None and self._current_idx >= len(self.activities):
            _LOGGER.warning(
                "The hub reports activity index %s but only %s activities are known (%s)",
                self._current_idx,
                len(self.activities),
                self.activities,
            )
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
        for cdev in self.compiled.devices.values():  # names come from the YAML
            reg = dreg.async_get_device_by_identifier(
                (DOMAIN, f"{self.hub_id}_dev_{cdev.id}"), entry_id
            )
            if reg and reg.name != cdev.name:
                dreg.async_update_device(reg.id, name=cdev.name)
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
                self._current_idx = None
                self.async_update_listeners()
                return
            # Stored even when beyond the activity list we know: after a reload that inserts an
            # activity the hub's re-report can precede the new list (_activity_name range-checks).
            self._last_idx = self._current_idx = idx
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
        elif mtype is MsgType.REMOTE_ATTACHED:
            # a fresh remote knows no pages: push them and the tile texts now
            self._pages_pending = False
            self.config_entry.async_create_background_task(
                self.hass, self.async_push_pages(), name=f"{DOMAIN} pages"
            )
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

    def resolve_device(self, device: str) -> str:
        """A device id, or one of a device's configured `aliases`, -> the device id.

        Lets automations written for the stock integration (`device: "51887071"`, the stock
        hub's numeric id) keep working after an entity swap. Anything unknown passes through
        unchanged, so the hub still rejects it with its own message.
        """
        if device == HUB_DEVICE or device in self.compiled.devices:
            return device
        for dev in self.compiled.devices.values():
            if device in dev.aliases:
                return dev.id
        return device

    async def async_set_device_state(
        self, device: str, power: bool | None = None, input: str | None | bool = False
    ) -> None:
        """Tell the hub a device changed by hand (no IR). `input=None` = unknown, False = leave."""
        args: dict[str, Any] = {"device": self.resolve_device(device)}
        if power is not None:
            args["power"] = power
        if input is not False:
            args["input"] = input
        await self.async_command(HUB_DEVICE, CMD_SET_DEVICE_STATE, args)

    async def async_device_command(self, device: str, command: str) -> None:
        """`remote.send_command device=...` / device buttons: the hub sends the IR and tracks it."""
        device = self.resolve_device(device)
        if device == HUB_DEVICE:
            await self.async_command(device, command)
            return
        await self.async_command(
            HUB_DEVICE, CMD_DEVICE_COMMAND, {"device": device, "command": command}
        )
