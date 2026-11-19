"""Push coordinator: hub state lives here, entities subscribe to changes.

Also owns the config lifecycle: the applied config comes from the Harmony Editor add-on (pulled,
re-validated and compiled here, cached in `.storage`) -> push `set_config` (only when the hub's
config hash differs) -> push pages -> keep tiles in sync with their bound entities.
"""

from __future__ import annotations

import asyncio
import base64
import contextlib
import logging
import time
from collections.abc import Callable, Iterable
from dataclasses import dataclass, field
from typing import Any

from homeassistant.config_entries import ConfigEntry
from homeassistant.core import Context, Event, HomeAssistant, State, callback
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
    COMMAND_BUTTON_LIMIT,
    CONF_ALL_COMMAND_BUTTONS,
    DEFAULT_REMOTE_IDS,
    DOMAIN,
    HUB_DEVICE,
    KEY_ACTIONS,
    KEY_NAMES,
    UNKNOWN_KEY,
)
from .errors import ha_error, invalid
from .harmony_core.compiler import (
    Compiled,
    HaStep,
    RemotePages,
    compile_config,
    config_stats,
    fit_error,
)
from .harmony_core.config_model import parse_model
from .harmony_core.ha_actions import HaCall, allowed
from .harmony_core.harmony_proto import MsgType
from .harmony_core.hub_limits import bt_fit_error
from .ota import CHUNK, TARGET_HUB, UPLOAD_ATTEMPTS, Image, encode_chunk
from .tile_format import tile_state, tile_text

_LOGGER = logging.getLogger(__name__)

__all__ = ["HarmonyCoordinator", "tile_text"]

TOPIC_AVAILABLE = "available"  # the hub connection went up or down
TOPIC_ACTIVITY = "activity"  # the running activity or the activity list
TOPIC_CONFIG = "config"  # a new applied config (devices, key names)
TOPIC_OTA = "ota"  # firmware update state or progress
TOPIC_LINK = "link"  # the add-on link state, revision or last error


def topic_device(device: str) -> str:
    return f"device:{device}"


def topic_battery(remote: int) -> str:
    return f"battery:{remote}"


def topic_remote(remote: int) -> str:
    return f"remote:{remote}"  # what the remote announced (page format, caps)


KeyListener = Callable[[int, int, str], None]
ISSUE_INVALID = "config_invalid"
ISSUE_REJECTED = "config_rejected"
ISSUE_SCHEMA = "bundle_schema"
ISSUE_YAML = "yaml_unused"
ISSUE_OTA_DEV_KEY = "ota_dev_key"  # + _<hub>: the hub trusts the public dev key
ISSUE_OTA_ROLLBACK = "ota_rolled_back"  # + _<hub>_<build>: the hub restored its previous firmware

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


@dataclass
class RemoteInfo:
    """What a remote announced (`remote_info`): page format, capability bits, icon font reach."""

    page_fmt: int
    caps: int
    icons: int
    gen: int = 0  # generation of the v2 page set the remote holds (0 and pages 0: none)
    pages: int = 0  # how many pages of it


@dataclass
class RemoteLink:
    """Per-remote page and tile bookkeeping (see docs/ha-config.md, "Which pages a remote gets")."""

    info: RemoteInfo | None = None
    epoch: int = 0  # bumped at every link-up: the remote may have lost its page store
    pushed: tuple[int, int, int, int] | None = None  # (config gen, format, epoch, theme caps) of the last complete set
    inflight: tuple[int, int, int, int] | None = None
    hub_theme: bool = True  # the hub understands theme_push (False once it answered unknown_type: bit 3 is then masked off)
    theme_sent: int | None = None  # gen of the custom theme the hub accepted for this remote's current set
    waiting: bool = False  # waiting for remote_info
    attached: bool = True  # False after "no remote" until the remote shows signs of life
    pending: bool = False  # a push failed for want of a remote: redo when it is back
    push_ms: float | None = None  # duration of the last complete page push (diagnostics)
    task: asyncio.Task[None] | None = None
    refused: bool = False  # the hub refused a message for a transient reason (not "no remote"): retry with backoff
    retries: int = 0  # consecutive re-pushes of an unconfirmed set; drives the backoff, reset on confirmation
    retry_task: asyncio.Task[None] | None = None  # the pending delayed re-push (cancellable, one per remote)
    retry_want: tuple[int, int, int, int] | None = None  # the set that re-push is for
    info_event: asyncio.Event = field(default_factory=asyncio.Event)

    @property
    def page_fmt(self) -> int:
        return 2 if self.info is not None and self.info.page_fmt >= 2 else 1

    @property
    def page_caps(self) -> int:
        """The theme caps the pages are built for: 0 for v1 or an unknown remote. Bit 3 counts only
        while a theme can be sent (this integration and the hub both speak theme_push); otherwise a
        user theme is its base preset and free colours are snapped (bit 2 behaviour)."""
        if self.page_fmt < 2 or self.info is None:
            return 0
        custom = const.THEME_TRANSPORT and self.hub_theme
        mask = const.CAP_THEME_COLOR | (const.CAP_THEME_CUSTOM if custom else 0)
        return self.info.caps & mask

    @property
    def tile_state(self) -> bool:
        return self.info is not None and bool(self.info.caps & const.CAP_TILE_STATE)


# Which entity states mean "on" for an HA device (docs/ha-config.md, "Home Assistant devices").
# domain -> (states that are on, states that are off); any other state of those domains is unknown.
_ON_OFF_STATES: dict[str, tuple[frozenset[str], frozenset[str]]] = {
    "cover": (frozenset({"open", "opening"}), frozenset({"closed", "closing"})),
    "lock": (frozenset({"unlocked", "unlocking", "open", "opening"}), frozenset({"locked", "locking"})),
}
_OFF_STATES = {"media_player": frozenset({"off", "standby"})}  # on = anything else known
_DEFAULT_OFF = frozenset({"off"})  # light, switch, fan, remote, climate (hvac mode off), ...


def _ha_device_state(state: State | None) -> DeviceState:
    """Power (on / off / None = unknown) and the input (`source` attribute) of an HA device."""
    if state is None or state.state in ("unavailable", "unknown"):
        return DeviceState()
    source = state.attributes.get("source")
    inp = source if isinstance(source, str) else None
    domain = state.entity_id.split(".", 1)[0]
    if domain in _ON_OFF_STATES:
        on, off = _ON_OFF_STATES[domain]
        power = True if state.state in on else False if state.state in off else None
    else:
        power = state.state not in _OFF_STATES.get(domain, _DEFAULT_OFF)
    return DeviceState(power, inp)


async def _sleep_hook(seconds: float) -> None:
    """The coordinator's only timer for waiting on the remote and HA settle times; tests swap it
    for a manual clock."""
    await asyncio.sleep(seconds)


def _empty_compiled() -> Compiled:
    return compile_config(parse_model(""))


class HarmonyCoordinator(DataUpdateCoordinator[None]):
    """Holds activities/state; updates arrive by push, never by polling."""

    config_entry: ConfigEntry

    def __init__(self, hass: HomeAssistant, entry: ConfigEntry, client: HarmonyHubClient) -> None:
        super().__init__(hass, _LOGGER, config_entry=entry, name=DOMAIN)
        self.client = client
        self.hub_id: str = client.hub_id or ""
        self.hub_name: str = client.hub_name
        self.fw_version = client.fw_version
        self.activities: list[str] = list(client.activities)
        self.device_states: dict[str, DeviceState] = {}
        self.battery: dict[int, tuple[int, int]] = {}  # remote -> (percent, millivolts)
        self.remotes: tuple[int, ...] = DEFAULT_REMOTE_IDS
        self._hub_theme = True  # the hub understands theme_push (until it says unknown_type)
        self.links: dict[int, RemoteLink] = {r: RemoteLink() for r in self.remotes}
        self._cfg_gen = 0  # bumped whenever the compiled config changes
        self._accepted = False  # the hub is known to run `compiled` (it reported the matching hash)
        self._rejected = False  # the hub refused `pending`; `compiled` stays the runtime table source
        self._known_rev: str | None = None  # last add-on rev handled (applied or rejected)
        self._sleep = lambda seconds: _sleep_hook(seconds)
        # tracked by hub index, never by name: a rename keeps the index, a reorder gets a new
        # state_update from the hub
        self._current_idx: int | None = None
        self._last_idx: int | None = None
        self.available = True
        # `compiled` is the config the hub RUNS (last accepted): dispatch, activity ids, HA steps,
        # tile bindings and the entities that describe the config all read it. `pending` is the
        # newest config; it becomes `compiled` only when the hub reports its hash.
        self.compiled: Compiled = _empty_compiled()
        self.pending: Compiled = self.compiled
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
        self.first_sync: asyncio.Task[None] | None = None
        self._first_apply = asyncio.Event()  # the link loop's first pull has been handled
        self.last_error: str | None = None
        self.timings: dict[str, float] = {}  # last compile / set_config round trip, in ms (diagnostics)
        # firmware update (ota.py): idle | uploading | applying | restarting | installed | failed | rolled_back
        self.ota_state = "idle"
        self.ota_progress = 0  # percent of the image the hub holds
        self.ota_build: int | None = None
        self.ota_error: str | None = None
        self.ota_reason: str | None = None  # why the hub rolled back (its boot hook's text)
        self._ota_lock = asyncio.Lock()
        self._rolled_back: set[int] = set()  # builds the hub reported rolling back
        self.page_pushes = 0  # page_push requests sent
        self.theme_pushes = 0  # theme_push requests sent
        self.config_pushes = 0  # set_config requests sent (observable by tests)
        self.tile_updates = 0
        self._topic_subs: dict[str, list[Callable[[], None]]] = {}
        self._key_listeners: list[KeyListener] = []
        self._config_listeners: list[Callable[[], None]] = []
        self._sync_lock = asyncio.Lock()
        self._tile_unsub: Callable[[], None] | None = None
        self._tile_dirty: set[str] = set()
        self._renamed: dict[str, str] = {}  # config entity id -> its id after a rename in HA
        self._tile_timer: asyncio.TimerHandle | None = None
        self._flush_task: asyncio.Task[None] | None = None
        self._tile_last: dict[tuple[int, int, int], tuple[bool, tuple[int, str]]] = {}
        self._ha_dev_unsub: Callable[[], None] | None = None
        self._ha_task: asyncio.Task[None] | None = None
        self._calls: dict[tuple[str, tuple[str, ...]], int] = {}  # key-started calls in flight
        self._released: set[int] = set()  # keys whose last event was a release: no more repeats
        self._act_known = False  # the first state_update after HA starts is only a baseline
        self._warned: set[tuple[str, ...]] = set()
        self._unsubs = [
            client.async_subscribe(self._on_push),
            client.async_subscribe_state(self._on_state),
        ]
        self._unsubs.append(
            hass.bus.async_listen(er.EVENT_ENTITY_REGISTRY_UPDATED, self._on_registry_update)
        )
        self.async_set_updated_data(None)

    async def _async_update_data(self) -> None:  # push only
        return None

    async def async_shutdown(self) -> None:
        for unsub in self._unsubs:
            unsub()
        self._unsubs = []
        self._untrack_tiles()
        for t in [self._ha_task, self._flush_task, *(lk.task for lk in self.links.values()),
                  *(lk.retry_task for lk in self.links.values())]:
            if t is not None and not t.done():
                t.cancel()
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

    async def async_sleep(self, seconds: float) -> None:
        await self._sleep(seconds)

    def key_name(self, code: int) -> str:
        return self.compiled.key_names.get(code) or KEY_NAMES.get(code, UNKNOWN_KEY)

    @property
    def event_types(self) -> list[str]:
        return self.compiled.event_types

    def async_subscribe_topics(
        self, topics: Iterable[str], cb: Callable[[], None]
    ) -> Callable[[], None]:
        """Entities listen to what they show (TOPIC_* below), never to "anything changed"."""
        topics = tuple(topics)
        for t in topics:
            self._topic_subs.setdefault(t, []).append(cb)

        def unsub() -> None:
            for t in topics:
                subs = self._topic_subs.get(t)
                if subs and cb in subs:
                    subs.remove(cb)

        return unsub

    @callback
    def _notify(self, *topics: str) -> None:
        seen: set[Callable[[], None]] = set()
        for t in topics:
            for cb in list(self._topic_subs.get(t, ())):
                if cb not in seen:  # an entity on two of the topics is written once
                    seen.add(cb)
                    cb()

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
        if self.has_addon_link:
            ids.add(f"{h}_addon_link")
        ids.add(f"{h}_firmware")
        for r in self.remotes:
            ids |= {f"{h}_key_{r}", f"{h}_battery_{r}", f"{h}_voltage_{r}", f"{h}_pagefmt_{r}"}
        for dev in self.compiled.devices.values():
            ids |= {f"{h}_dev_{dev.id}_power", f"{h}_dev_{dev.id}_input"}
            ids |= {f"{h}_cmd_{dev.id}:{c}" for c in self.button_commands(dev.id)}
        return ids

    def device_commands(self, device: str) -> list[str]:
        """Every command the device answers to: the configured ones plus power_on/power_off. All of
        them stay reachable through `remote.send_command` whatever buttons exist."""
        dev = self.compiled.devices[device]
        extra = ["power_on", "power_off"] if dev.power else []
        return [*dev.commands, *extra]

    def button_commands(self, device: str) -> list[str]:
        """Commands that get a button entity. Up to COMMAND_BUTTON_LIMIT per device (or with the
        `all_command_buttons` option) all of them; beyond it only those that already have a registry
        entry, so buttons an install already uses keep working and stay as they are."""
        cmds = self.device_commands(device)
        if len(cmds) <= COMMAND_BUTTON_LIMIT or self.config_entry.options.get(CONF_ALL_COMMAND_BUTTONS):
            return cmds
        reg = er.async_get(self.hass)
        h = self.hub_id
        return [
            c for c in cmds if reg.async_get_entity_id("button", DOMAIN, f"{h}_cmd_{device}:{c}")
        ]

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
        """Configured (entry data), not merely started: the cache loads before the link starts."""
        return self._link is not None or bool(self.config_entry.data.get(const.CONF_ADDON_HOST))

    def async_set_link_state(self, state: str) -> None:
        if state != self.link_state:
            self.link_state = state
            self._notify(TOPIC_LINK)

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
                compile_source, data.get("config"), self.hub_id, self.client.limits
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
            t0 = time.monotonic()
            try:
                bundle = await self.hass.async_add_executor_job(parse_bundle, raw, self.hub_id, self.client.limits)
                self.timings["compile_ms"] = round((time.monotonic() - t0) * 1000, 1)
            except BundleRejected as exc:
                self._reject(exc)
                raise
            self._clear_issue(ISSUE_INVALID)
            self._clear_issue(ISSUE_SCHEMA)
            self.last_error = None
            changed = bundle.rev != self.rev or not self.managed
            self.rev, self.applied_at, self.managed = bundle.rev, bundle.applied_at, True
            self._known_rev = bundle.rev
            self._notify(TOPIC_LINK)
            if changed:
                await self._store.async_save(
                    {"rev": bundle.rev, "applied_at": bundle.applied_at, "config": bundle.source}
                )
            self._apply_compiled(bundle.compiled)
            self.async_set_link_state("ok")
            self._first_apply.set()
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
        self._notify(TOPIC_LINK)
        self._first_apply.set()
        self._set_issue(ISSUE_SCHEMA if exc.schema else ISSUE_INVALID, str(exc))
        self.async_set_link_state("rejected")

    def async_start_link(self, client: AddonClient) -> None:
        self._link = client
        client.limits_provider = lambda: self.client.limits
        client.remotes_provider = self._remotes_report
        client.identity = self._link_identity()
        self.link_state = "connecting"
        self.config_entry.async_create_background_task(
            self.hass, self._link_loop(client), f"{DOMAIN} add-on link {self.hub_id}"
        )

    def _link_identity(self) -> dict[str, str]:
        """Which hub this is, for the add-on's polls: the hardware id, the id before a rename (until the
        add-on has answered once) and the display name."""
        data = self.config_entry.data
        out: dict[str, str] = {}
        if data.get(const.CONF_HW_ID):
            out["hw"] = data[const.CONF_HW_ID]
        if data.get(const.CONF_PREV_ID):
            out["prev"] = data[const.CONF_PREV_ID]
        out["name"] = data.get(const.CONF_HUB_NAME, "")
        return out

    def _prev_id_seen(self) -> None:
        """The add-on answered a poll that carried the previous id: it has moved (or refused) the store. An add-on
        that ignores the rename headers (no adopt answer) has seen nothing: the previous id stays."""
        if self._link is None or self._link.last_adopt is None:
            return
        data = self.config_entry.data
        if data.get(const.CONF_PREV_ID):
            self.hass.config_entries.async_update_entry(
                self.config_entry, data={k: v for k, v in data.items() if k != const.CONF_PREV_ID}
            )
        if self._link is not None:
            self._link.identity.pop("prev", None)

    async def async_wait_first_config(self, timeout: float | None = None) -> None:
        """First run with nothing cached: give the link loop's first pull (the only one) a few seconds
        so entities exist when setup returns; never raises, never waits for the hub."""
        if self._link is None or self.managed:
            return
        with contextlib.suppress(TimeoutError):
            await asyncio.wait_for(self._first_apply.wait(), timeout or const.FIRST_APPLY_WAIT)

    async def async_refresh_from_addon(self) -> None:
        """Service entry point: pull now, apply, push what changed; raises with the reason."""
        if self._link is None:
            raise ha_error("not_linked")
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
            raise ha_error("config_rejected", error=self.last_error)

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
        self._first_apply.set()  # a first answer, even a failed one: setup need not wait longer
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
        self._known_rev = self.rev  # last rev handled (applied or rejected); "" = none applied
        delay = const.LINK_BACKOFF_MIN
        while True:
            started = loop.time()
            client.identity = self._link_identity()
            try:
                if self._known_rev is None:
                    raw: bytes | None = await client.fetch(self.hub_id)
                else:
                    raw = await client.wait(self.hub_id, self._known_rev)
                    if raw is None and not self._known_rev:
                        # the add-on holds the wait open when nothing is applied for this hub id, so
                        # a quiet poll says nothing: ask once per cycle (404 -> "nothing_applied")
                        raw = await client.fetch(self.hub_id)
                if self.link_state in ("unreachable", "auth_failed"):
                    _LOGGER.info("Harmony Editor add-on is reachable again")
                self._prev_id_seen()
                if raw is not None:
                    try:
                        bundle = await self.async_process_raw(raw)
                    except BundleRejected as exc:
                        if exc.rev is None:
                            raise
                        self._known_rev = exc.rev  # wait for the next apply, not re-pulling this one
                    else:
                        if bundle:
                            self._known_rev = bundle.rev
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
                self._prev_id_seen()
                self._note_link_failure(exc)
                self._known_rev = ""
                await asyncio.sleep(delay)
            except AddonError as exc:
                self._note_link_failure(exc)
                await asyncio.sleep(delay)
                delay = min(delay * 2, const.LINK_BACKOFF_MAX)
            except Exception:
                _LOGGER.exception("Unexpected error in the add-on link; retrying")
                self._first_apply.set()
                await asyncio.sleep(const.LINK_BACKOFF_MAX)

    def _apply_compiled(self, compiled: Compiled) -> None:
        """Record the newest config; it becomes the runtime one at once only when the hub already
        reports its hash (HA restart with the cached config), else when the hub accepts it."""
        self.pending = compiled
        self._rejected = False
        if self.client.config_hash == compiled.config_hash:
            self._promote()
        else:
            self._notify(TOPIC_LINK)

    @property
    def config_state(self) -> str:
        """`accepted`: the hub runs the newest config. `pending`: waiting for the hub to take it.
        `rejected`: the hub refused it and keeps running the previous one."""
        if self._accepted and self.compiled is self.pending:
            return "accepted"
        return "rejected" if self._rejected else "pending"

    def _promote(self) -> None:
        """Atomically make `pending` the runtime config (the hub runs it)."""
        compiled = self.pending
        changed = compiled != self.compiled
        self.compiled = compiled
        self._accepted = True
        self._rejected = False
        self._clear_issue(ISSUE_YAML)  # the add-on config is live: a leftover YAML file is harmless
        self._notify(TOPIC_LINK)
        if changed:
            self._cfg_gen += 1
            self._warned.clear()  # one warning per (service, entity) per config revision
            self._tile_last.clear()
            self._track_tiles()
            self._prune_registry()
            for cb in list(self._config_listeners):
                cb()
            self._notify(TOPIC_CONFIG)

    def async_start_first_sync(self) -> None:
        """Setup's hub sync, in the background (`first_sync`); entities fill in as it completes."""
        self.first_sync = self.config_entry.async_create_background_task(
            self.hass, self.async_sync_config(), f"{DOMAIN} first sync {self.hub_id}"
        )

    async def async_sync_config(self) -> None:
        """Push set_config iff the hub's hash differs, then pages and tile texts. Never raises."""
        async with self._sync_lock:
            if not self.managed or not self.client.connected:
                return
            try:
                if await self._push_config_locked():
                    self._ack_later(self.rev, "ok")
                    for remote in self.remotes:
                        self._request_push(remote, wait=True)
                else:
                    self._ack_later(
                        self.rev, "error", f"the hub rejected it: {self.last_error}"[:200]
                    )
            except (HarmonyConnectionError, HomeAssistantError) as exc:
                _LOGGER.warning("Could not sync the config to the hub (will retry): %s", exc)

    async def _push_config_locked(self) -> bool:
        """True when the hub runs the compiled config afterwards."""
        compiled = self.pending
        want = compiled.config_hash
        if self.client.config_hash == want:
            _LOGGER.debug("Hub already runs config %s; not pushing", want)
            self._promote()
            return True
        self.config_pushes += 1
        t0 = time.monotonic()
        too_big = None
        if self.client.limits is not None:  # compiled before the hub reported its limits? check now, not at the hub
            too_big = await self.hass.async_add_executor_job(
                lambda: fit_error(config_stats(compiled.hub_json, self.client.limits.max_pronto), self.client.limits)
            )
        too_big = too_big or bt_fit_error(compiled.hub_json, self.client.bt)
        try:
            if too_big:
                hint = " (this hub runs older firmware; updating it raises the limit)" if self.client.limits.legacy else ""
                ack = {"ok": False, "message": too_big + hint}
            else:
                ack = await self.client.async_set_config(compiled.hub_json)
        except HarmonyCommandError as exc:
            ack = {"ok": False, "message": exc.message}
        self.timings["set_config_ms"] = round((time.monotonic() - t0) * 1000, 1)
        if not ack.get("ok"):
            msg = str(ack.get("message") or "rejected without a reason")
            _LOGGER.error("The hub rejected the config: %s", msg)
            self.last_error = msg
            self._notify(TOPIC_LINK)
            self._set_issue(ISSUE_REJECTED, msg)
            self._rejected = True
            self._notify(TOPIC_LINK)
            self._warn_unknown_index()  # no new list is coming: the reported index stays unknown
            return False
        if self.last_error is not None:
            self.last_error = None
            self._notify(TOPIC_LINK)
        self._clear_issue(ISSUE_REJECTED)
        self.client.config_hash = ack.get("config_hash")  # as the hub reports it
        reported = self.client.config_hash
        if reported is None or reported == want:
            self._promote()
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
        self._notify(TOPIC_ACTIVITY)

    async def async_push_pages(self) -> None:
        """The `push_pages` service: resend the whole set to every remote and wait for it."""
        tasks = [self._request_push(r, wait=True, force=True) for r in self.remotes]
        await asyncio.gather(*(t for t in tasks if t), return_exceptions=True)

    # ------------------------------------------------------------ pages per remote
    def remote_format(self, remote: int) -> int:
        return self._rlink(remote).page_fmt

    def _rlink(self, remote: int) -> RemoteLink:
        return self.links.setdefault(remote, RemoteLink(hub_theme=self._hub_theme))

    @property
    def hub_accepted(self) -> bool:
        """The hub is known to run the compiled config: only then do pages of it make sense."""
        return self.config_state == "accepted"

    def _request_push(
        self, remote: int, *, wait: bool, force: bool = False
    ) -> asyncio.Task[None] | None:
        """(Re)start the page push of one remote. A no-op when the wanted (config gen, format,
        link epoch) is what is already pushed or in flight, or while the hub has not accepted the
        config. Otherwise an older push of the remote is cancelled first and has finished before
        the new one sends anything, so two sets never interleave."""
        link = self._rlink(remote)
        old = link.task
        running = old is not None and not old.done()
        if not force:
            if not self.hub_accepted:
                link.pending = True  # the sync that gets the config accepted requests the push
                return old if running else None
            if running and link.waiting:
                return old  # it reads the format and generation when remote_info arrives
            want = (self._cfg_gen, link.page_fmt, link.epoch, link.page_caps)
            if want in (link.pushed, link.inflight):
                return old if running else None
            if link.retry_task is not None and not link.retry_task.done():
                if link.retry_want == want:
                    return old if running else None  # backing off: the retry task pushes when it is due
                self._retry_reset(link)  # another set (new config, caps, epoch): no reason to wait
        if running:
            old.cancel()
        link.task = self.config_entry.async_create_background_task(
            self.hass, self._push_remote(remote, wait, force, old), name=f"{DOMAIN} pages {remote}"
        )
        return link.task

    async def _push_remote(
        self, remote: int, wait: bool, force: bool, old: asyncio.Task[None] | None
    ) -> None:
        link = self._rlink(remote)
        if old is not None:
            await asyncio.gather(old, return_exceptions=True)
        if wait and link.info is None:
            link.waiting = True
            try:
                await self._wait_info(link)
            finally:
                link.waiting = False
        if not self.hub_accepted:
            link.pending = True
            return
        want = (self._cfg_gen, link.page_fmt, link.epoch, link.page_caps)
        if not force and link.pushed == want:
            return
        if not force and self._remote_holds_set(link):
            # the remote kept its page store (a HA<->hub reconnect, a hub restart): nothing to send
            # but the tile values that changed meanwhile
            link.pushed, link.pending = want, False
            self._retry_reset(link)
            await self._send_tiles({self._cur(t.entity_id) for t in self.compiled.tiles}, (remote,))
            return
        link.inflight = want
        t0 = time.monotonic()
        try:
            link.refused = False
            ok = await self._push_set(remote, link, want[1])
            rp = self.compiled.for_caps(link.page_caps) if want[1] >= 2 else None
            themed = rp is not None and rp.theme_wire is not None
            if ok and themed and not await self._await_theme_ack(link, rp):
                _LOGGER.warning("Remote %s did not report the custom theme set; sending it once more", remote)
                ok = await self._push_set(remote, link, want[1])  # the hub gave up on it (the remote was away)
                if ok and not await self._await_theme_ack(link, rp):
                    # never mark a set pushed that the remote did not confirm: stay pending, try again later
                    _LOGGER.warning("Remote %s still does not hold the custom theme set; will retry", remote)
                    link.pending = True
                    self._schedule_retry(remote, want)
                    return
            if ok:
                link.pushed, link.pending = want, False
                if themed:  # the remote confirmed the set; otherwise only its remote_info does
                    self._retry_reset(link)
                link.push_ms = round((time.monotonic() - t0) * 1000, 1)
                await self._send_all_tiles(remote)
            elif link.refused:
                link.pending = True
                self._schedule_retry(remote, want)
        finally:
            link.inflight = None

    def _retry_reset(self, link: RemoteLink) -> None:
        link.retries = 0
        rt, link.retry_task = link.retry_task, None
        if rt is not None and not rt.done() and rt is not asyncio.current_task():
            rt.cancel()

    def _schedule_retry(self, remote: int, want: tuple[int, int, int, int], immediate: bool = False) -> None:
        """Push the set again later: bounded exponential backoff (PUSH_RETRY_BASE doubling up to
        PUSH_RETRY_MAX), the first one at once when `immediate`. One pending retry per remote, cancelled
        on unload; it does not hold up any other remote."""
        link = self._rlink(remote)
        if link.retry_task is not None and not link.retry_task.done() and link.retry_want == want:
            return
        n = link.retries
        link.retries += 1
        delay = 0.0 if immediate and n == 0 else min(const.PUSH_RETRY_BASE * 2 ** min(n, 16), const.PUSH_RETRY_MAX)
        link.retry_want = want
        link.retry_task = self.config_entry.async_create_background_task(
            self.hass, self._retry_push(remote, want, delay), name=f"{DOMAIN} repush {remote}"
        )

    async def _retry_push(self, remote: int, want: tuple[int, int, int, int], delay: float) -> None:
        link = self._rlink(remote)
        if delay:
            await self._sleep(delay)
        link.retry_task = None
        if link.pushed == want:
            link.pushed = None  # the remote did not keep it: send it again
        self._request_push(remote, wait=False)

    def _remote_holds_set(self, link: RemoteLink) -> bool:
        """The remote reported exactly the v2 set we would send: same format, generation and count."""
        c, info = self.compiled, link.info
        if info is None or link.page_fmt != 2 or not c.pages_v2:
            return False
        rp = c.for_caps(link.page_caps)
        return info.gen == rp.gen and info.pages == len(rp.pages_v2)

    def _remotes_report(self) -> list[dict[str, Any]]:
        """What the add-on's editor is told about each remote whose caps are known: its colour tier
        ("none", "palette", "custom") and the notices of what that costs the current config."""
        out: list[dict[str, Any]] = []
        for remote in self.remotes:
            info = self._rlink(remote).info
            if info is None:
                continue
            tier = (
                "custom" if info.caps & const.CAP_THEME_CUSTOM else "palette" if info.caps & const.CAP_THEME_COLOR else "none"
            )
            out.append(
                {
                    "remote": remote,
                    "caps": info.caps,
                    "tier": tier,
                    "notices": [n["msg"] for n in self.theme_notices(remote)],
                }
            )
        return out

    def theme_notices(self, remote: int) -> list[dict[str, Any]]:
        """What this remote's caps cost it in colours and themes (empty when nothing is degraded)."""
        link = self._rlink(remote)
        return self.compiled.for_caps(link.page_caps).notices if link.page_fmt == 2 else []

    async def _wait_info(self, link: RemoteLink) -> None:
        """Up to INFO_WAIT seconds for the remote's `remote_info`; never blocks the loop."""
        waiter = asyncio.ensure_future(link.info_event.wait())
        timer = asyncio.ensure_future(self._sleep(const.INFO_WAIT))
        try:
            await asyncio.wait({waiter, timer}, return_when=asyncio.FIRST_COMPLETED)
        finally:
            waiter.cancel()
            timer.cancel()

    async def _push_set(self, remote: int, link: RemoteLink, fmt: int) -> bool:
        """Send the custom theme (when the set has one), then every page of format `fmt`, one after the
        other (each command result is the backpressure). The theme goes first: the remote shows no
        page of a set until its theme is in. False when it did not complete (no remote, link down,
        newer config)."""
        gen = self._cfg_gen
        caps = link.page_caps
        rp = self.compiled.for_caps(caps) if fmt >= 2 else None
        link.theme_sent = None
        if rp is not None and rp.theme_wire is not None:
            ok = await self._send_theme(remote, link, rp)
            if ok is None:
                return False
            if ok is False:  # the hub cannot carry it: pages built for what it can (the base preset)
                self._request_push(remote, wait=False)
                return False
        pages = rp.pages_v2_b64 if rp is not None else self.compiled.encoded_pages_b64(1)
        for idx, b64 in enumerate(pages):
            if gen != self._cfg_gen:
                return False
            payload = {"remote": remote, "page": idx, "data": b64}
            self.page_pushes += 1
            try:
                await self.client.async_send(MsgType.PAGE_PUSH, payload)
            except HarmonyCommandError as exc:
                if exc.code == "no_remote":
                    link.pending, link.attached = True, False  # redone when the remote is alive
                    _LOGGER.debug("No remote attached; page %s not delivered yet", idx)
                    return False
                if exc.code != "too_big":
                    _LOGGER.warning("Hub refused page %s (%s); will retry", idx, exc)
                    link.pending, link.refused = True, True
                    return False
                _LOGGER.warning("Hub refused page %s: %s", idx, exc)
            except HarmonyConnectionError as exc:
                link.pending = True
                _LOGGER.debug("Page push to remote %s interrupted: %s", remote, exc)
                return False
        return True

    async def _send_theme(self, remote: int, link: RemoteLink, rp: RemotePages) -> bool | None:
        """theme_push of the set's custom theme. True: the hub took it. None: not delivered (no remote,
        link down): the push is redone later. False: this hub does not know theme_push (old firmware):
        bit 3 is masked off for it and the set is rebuilt without the custom theme."""
        assert rp.theme_wire is not None
        payload = {"remote": remote, "gen": rp.gen, "data": base64.b64encode(rp.theme_wire).decode()}
        self.theme_pushes += 1
        try:
            await self.client.async_send(MsgType.THEME_PUSH, payload)
        except HarmonyCommandError as exc:
            if exc.code == "unknown_type":
                _LOGGER.info(
                    "The hub does not know theme_push (older firmware); custom theme %r shows as %r",
                    self.compiled.theme_name,
                    self.compiled.theme_base,
                )
                self._set_hub_theme(False)
                return False
            if exc.code == "no_remote":
                link.pending, link.attached = True, False
                return None
            # busy or another transient refusal: the theme is not queued, so the set is not sent (the pages
            # would arrive without it) and is retried with backoff
            _LOGGER.warning("Hub refused the custom theme (%s); will retry", exc)
            link.pending, link.refused = True, True
            return None
        except HarmonyConnectionError as exc:
            link.pending = True
            _LOGGER.debug("Theme push to remote %s interrupted: %s", remote, exc)
            return None
        link.theme_sent = rp.gen
        return True

    def _set_hub_theme(self, ok: bool) -> None:
        for lk in self.links.values():
            lk.hub_theme = ok
        self._hub_theme = ok

    def _theme_acked(self, link: RemoteLink, rp: RemotePages) -> bool:
        """The remote reports the whole set (its pages and, for a custom theme, the theme) as held."""
        info = link.info
        return info is not None and info.gen == rp.gen and info.pages == len(rp.pages_v2)

    async def _await_theme_ack(self, link: RemoteLink, rp: RemotePages) -> bool:
        """Up to THEME_ACK_WAIT seconds for the remote_info that says the set is complete."""
        timer = asyncio.ensure_future(self._sleep(const.THEME_ACK_WAIT))
        try:
            while not self._theme_acked(link, rp):
                link.info_event.clear()
                waiter = asyncio.ensure_future(link.info_event.wait())
                try:
                    await asyncio.wait({waiter, timer}, return_when=asyncio.FIRST_COMPLETED)
                finally:
                    waiter.cancel()
                if timer.done():
                    break
        finally:
            timer.cancel()
        return self._theme_acked(link, rp)

    def theme_status(self, remote: int) -> dict[str, Any]:
        """The custom theme of this remote's set: name, preset id, whether the hub took it, whether the remote holds it."""
        link = self._rlink(remote)
        c = self.compiled
        rp = c.for_caps(link.page_caps) if link.page_fmt == 2 else None
        custom = rp is not None and rp.theme_wire is not None
        return {
            "name": c.theme_name,
            "id": c.theme_id,
            "sent": custom and link.theme_sent == rp.gen,
            "acked": custom and self._theme_acked(link, rp),
        }

    def _remote_seen(self) -> None:
        """A key or battery message proves a remote is attached: redo what failed for want of it."""
        for remote in self.remotes:
            link = self._rlink(remote)
            link.attached = True
            if link.pending and (link.task is None or link.task.done()):
                self._request_push(remote, wait=False)

    def _on_remote_attached(self) -> None:
        for remote in self.remotes:
            link = self._rlink(remote)
            link.epoch += 1  # the remote may have rebooted and lost its store
            link.info, link.pushed, link.attached = None, None, True
            self._retry_reset(link)
            link.info_event.clear()
            self._request_push(remote, wait=True)
            self._notify(topic_remote(remote))

    def _on_remote_info(self, data: dict[str, Any]) -> None:
        remote = data["remote"]
        link = self._rlink(remote)
        new = RemoteInfo(data["page_fmt"], data["caps"], data["icons"], data["gen"], data["pages"])
        changed = link.info != new
        link.info, link.attached = new, True
        link.info_event.set()
        # One push per link-up: a task still waiting for this message carries on by itself, and a
        # set already sent (or being sent) in this format and epoch is not repeated.
        if remote in self.remotes:
            want = (self._cfg_gen, link.page_fmt, link.epoch, link.page_caps)
            if self._remote_holds_set(link):
                self._retry_reset(link)
                self._request_push(remote, wait=False)
            elif (
                link.pushed == want
                and link.page_fmt == 2
                and self.compiled.pages_v2
                and self.hub_accepted
                and link.inflight is None
                and (link.task is None or link.task.done())
            ):
                # we think the remote holds the set but it says it does not (it rebooted without the
                # hub seeing the link drop, or the hub gave up on a page): push again, backing off
                self._schedule_retry(remote, want, immediate=new.gen == 0 and new.pages == 0)
            else:
                self._request_push(remote, wait=False)
        if changed:
            self._notify(topic_remote(remote))

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
        keep_remotes = {f"{self.hub_id}_remote_{r}" for r in self.remotes}
        for dev in dr.async_entries_for_config_entry(dreg, entry_id):  # a remote that is gone
            ids = {i for dom, i in dev.identifiers if dom == DOMAIN}
            if any("_remote_" in i for i in ids) and not ids & keep_remotes:
                dreg.async_update_device(dev.id, remove_config_entry_id=entry_id)

    # ------------------------------------------------------------ tiles
    def _cur(self, entity_id: str) -> str:
        """The entity id a configured one has now (tiles and HA devices follow a rename)."""
        return self._renamed.get(entity_id, entity_id)

    @callback
    def _on_registry_update(self, event: Event[Any]) -> None:
        data = event.data
        old, new = data.get("old_entity_id"), data.get("entity_id")
        if data.get("action") != "update" or not old or not new or old == new:
            return
        watched = {t.entity_id for t in self.compiled.tiles}
        watched |= {d.entity for d in self.compiled.ha_devices.values() if d.entity}
        moved = [cfg for cfg in watched if self._cur(cfg) == old]
        if not moved:
            return
        for cfg in moved:
            if new == cfg:
                self._renamed.pop(cfg, None)  # back to the configured name
            else:
                self._renamed[cfg] = new
        self._track_tiles()  # re-subscribe under the new ids
        self._tile_dirty.add(new)
        if self._tile_timer is None:
            self._tile_timer = self.hass.loop.call_later(const.TILE_DEBOUNCE, self._start_flush)

    def _untrack_tiles(self) -> None:
        if self._tile_unsub:
            self._tile_unsub()
            self._tile_unsub = None
        if self._ha_dev_unsub:
            self._ha_dev_unsub()
            self._ha_dev_unsub = None
        if self._tile_timer is not None:
            self._tile_timer.cancel()
            self._tile_timer = None
        self._tile_dirty.clear()

    def _track_tiles(self) -> None:
        """Subscribe to exactly the entities on pages (tiles) and behind HA devices."""
        self._untrack_tiles()
        entities = sorted({self._cur(t.entity_id) for t in self.compiled.tiles})
        if entities:
            self._tile_unsub = async_track_state_change_event(
                self.hass, entities, self._on_tile_entity
            )
        dev_of: dict[str, list[str]] = {}
        for dev in self.compiled.ha_devices.values():
            if dev.entity:
                cur = self._cur(dev.entity)
                dev_of.setdefault(cur, []).append(dev.id)
                self.device_states[dev.id] = _ha_device_state(self.hass.states.get(cur))
        if dev_of:

            @callback
            def on_dev(event: Event[EventStateChangedData]) -> None:
                # a playing media_player changes attributes every few seconds: only a change of
                # the derived power/input reaches the entities of that device
                new = _ha_device_state(event.data["new_state"])
                for dev_id in dev_of.get(event.data["entity_id"], ()):
                    if self.device_states.get(dev_id) != new:
                        self.device_states[dev_id] = new
                        self._notify(topic_device(dev_id))

            self._ha_dev_unsub = async_track_state_change_event(self.hass, list(dev_of), on_dev)

    @callback
    def _on_tile_entity(self, event: Event[EventStateChangedData]) -> None:
        """Bursts coalesce: the entity is only marked, one flush per TILE_DEBOUNCE sends the
        latest value of each marked entity."""
        self._tile_dirty.add(event.data["entity_id"])
        if self._tile_timer is None:
            self._tile_timer = self.hass.loop.call_later(const.TILE_DEBOUNCE, self._start_flush)

    @callback
    def _start_flush(self) -> None:
        self._tile_timer = None
        if self._flush_task is not None and not self._flush_task.done():
            self._tile_timer = self.hass.loop.call_later(const.TILE_DEBOUNCE, self._start_flush)
            return  # still sending the last batch; collect more
        entities, self._tile_dirty = self._tile_dirty, set()
        self._flush_task = self.config_entry.async_create_background_task(
            self.hass, self._send_tiles(entities, self.remotes), name=f"{DOMAIN} tiles"
        )

    async def _send_all_tiles(self, remote: int) -> None:
        """After a page push the remote knows no tile values: send every one again."""
        for key in [k for k in self._tile_last if k[0] == remote]:
            del self._tile_last[key]
        await self._send_tiles({self._cur(t.entity_id) for t in self.compiled.tiles}, (remote,))

    async def _send_tiles(self, entities: set[str], remotes: tuple[int, ...]) -> None:
        """Send each tile whose rendered (state, text) pair changed since the last send. Nothing is
        sent to a remote known to be away: the hub holds the latest per tile, and the page push
        that follows its return resends everything. A burst is pipelined (TILE_WINDOW requests in
        flight, each tile at most once per call so its order is kept) instead of one round trip
        per tile."""
        for remote in remotes:
            link = self._rlink(remote)
            if not link.attached or not self.client.connected:
                continue
            rich = link.tile_state
            todo: list[tuple[tuple[int, int, int], tuple[int, str], dict[str, Any], str]] = []
            for tile in self.compiled.tiles:
                cur = self._cur(tile.entity_id)
                if cur not in entities:
                    continue
                st = self.hass.states.get(cur)
                value = tile_state(st) if rich else (0, tile_text(st))
                key = (remote, tile.page, tile.widget)
                if self._tile_last.get(key) == (rich, value):
                    continue
                args: dict[str, Any] = {
                    "remote": remote,
                    "page": tile.page,
                    "tile": tile.widget,
                    "text": value[1],
                }
                if rich:
                    args["state"] = value[0]
                todo.append((key, value, args, tile.entity_id))
            if not todo:
                continue
            window = asyncio.Semaphore(const.TILE_WINDOW)
            stop = False

            async def one(
                key: tuple[int, int, int], value: tuple[int, str], args: dict[str, Any], entity: str,
                link: RemoteLink = link, rich: bool = rich, window: asyncio.Semaphore = window,
            ) -> None:
                nonlocal stop
                async with window:
                    if stop:
                        return
                    try:
                        await self.client.async_send(
                            MsgType.COMMAND,
                            {"device": HUB_DEVICE, "command": CMD_TILE_UPDATE, "args": args},
                        )
                    except HarmonyCommandError as exc:
                        _LOGGER.debug("tile_update for %s not delivered: %s", entity, exc)
                        if exc.code == "no_remote":
                            link.attached = False
                            stop = True
                    except HarmonyConnectionError as exc:
                        _LOGGER.debug("tile_update for %s failed: %s", entity, exc)
                        stop = True
                    else:
                        self._tile_last[key] = (rich, value)
                        self.tile_updates += 1

            await asyncio.gather(*(one(*item) for item in todo))
            if not self.client.connected:
                return

    # ------------------------------------------------------------ hub -> HA
    @callback
    def _on_state(self, connected: bool, err: HarmonyError | None) -> None:
        self.available = connected
        if connected:
            self.note_ota_key()
            if self.client.hub_id and self.client.hub_id != self.hub_id:
                # the hub answers under another id (renamed on the device): setup follows it by hardware id
                _LOGGER.info("Hub %s now answers as %s; reloading", self.hub_id, self.client.hub_id)
                self.hass.config_entries.async_schedule_reload(self.config_entry.entry_id)
                return
        if connected and self.ota_state == "restarting":
            self._set_ota("installed")  # the hub came back from the update
        if connected:
            # hello was re-sent by the client: refresh what the hub told us. The hub re-announces
            # its remotes (remote_info) right after hello_ack. The next state_update is compared
            # with the activity known before the drop: a change while disconnected still runs its
            # Home Assistant steps (only the very first report after HA starts is a baseline).
            self._accepted = False  # the hub may have rebooted: the resync confirms its config
            self._set_hub_theme(True)  # and may run other firmware: theme_push is tried again
            for link in self.links.values():
                link.epoch += 1
                link.info, link.pushed = None, None
                self._retry_reset(link)
                link.info_event.clear()
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
        self._notify(TOPIC_AVAILABLE)

    @callback
    def _on_push(self, mtype: MsgType, data: dict[str, Any]) -> None:
        if mtype is MsgType.STATE_UPDATE:
            idx = data["activity"]
            before = self.current_activity_id
            idx_before = (self._current_idx, self._last_idx)
            if not data["power"]:
                self._current_idx = None
            else:
                # Stored even when beyond the activity list we know: after a reload that inserts an
                # activity the hub's re-report can precede the new list (_activity_name range-checks).
                self._last_idx = self._current_idx = idx
            self._note_activity(before, self.current_activity_id)
            if idx_before != (self._current_idx, self._last_idx):
                self._notify(TOPIC_ACTIVITY)
        elif mtype is MsgType.KEY_EVENT:
            self._remote_seen()
            action = KEY_ACTIONS.get(data["action"], "press")
            self._dispatch_ha(data["key"], action)  # first: nothing else delays the service call
            for cb in list(self._key_listeners):
                cb(data["remote"], data["key"], action)
        elif mtype is MsgType.ACTIVITY_LIST:
            self.client.config_hash = data.get("config_hash")
            if self.managed and self.client.config_hash == self.pending.config_hash:
                self._promote()  # the hub runs the newest config (another sync, a replay)
            acts = data["activities"]
            self._set_activities([a for a in acts if isinstance(a, str)])
        elif mtype is MsgType.DEVICE_STATE:
            if data["device"] not in self.compiled.ha_devices:  # those follow their entity
                new = DeviceState(data["power"], data.get("input"))
                if self.device_states.get(data["device"]) != new:
                    self.device_states[data["device"]] = new
                    self._notify(topic_device(data["device"]))
        elif mtype is MsgType.OTA_STATUS:
            self._on_ota_status(data)
        elif mtype is MsgType.REMOTE_ATTACHED:
            self._on_remote_attached()
        elif mtype is MsgType.REMOTE_INFO:
            self._on_remote_info(data)
        elif mtype is MsgType.BATTERY:
            self._remote_seen()
            new_b = (data["percent"], data["millivolts"])
            if self.battery.get(data["remote"]) != new_b:
                self.battery[data["remote"]] = new_b
                self._notify(topic_battery(data["remote"]))

    # ------------------------------------------------------------ firmware update
    def _set_ota(self, state: str, *, progress: int | None = None, error: str | None = None) -> None:
        self.ota_state = state
        if progress is not None:
            self.ota_progress = progress
        self.ota_error = error
        self._notify(TOPIC_OTA)

    def _on_ota_status(self, data: dict[str, Any]) -> None:
        """A rolled_back status arrives after every hello while the hub holds the boot hook's record."""
        if data["state"] != "rolled_back":
            return
        build = data.get("build", 0)
        reason = data.get("reason", "")
        self._rolled_back.add(build)
        self.ota_reason = reason
        if self.ota_state not in ("uploading", "applying"):
            self.ota_build = build
            self._set_ota("rolled_back")
        ir.async_create_issue(
            self.hass,
            DOMAIN,
            f"{ISSUE_OTA_ROLLBACK}_{self.hub_id}_{build}",
            is_fixable=False,
            severity=ir.IssueSeverity.WARNING,
            translation_key=ISSUE_OTA_ROLLBACK,
            translation_placeholders={"hub": self.hub_id, "build": str(build), "reason": reason},
        )

    def _clear_ota_issues(self) -> None:
        reg = ir.async_get(self.hass)
        prefix = f"{ISSUE_OTA_ROLLBACK}_{self.hub_id}_"
        for domain, issue_id in list(reg.issues):
            if domain == DOMAIN and issue_id.startswith(prefix):
                ir.async_delete_issue(self.hass, DOMAIN, issue_id)

    async def _wait_connected(self, tries: int = 40) -> bool:
        for _ in range(tries):
            if self.client.connected:
                return True
            await self.async_sleep(0.5)
        return self.client.connected

    def note_ota_key(self) -> None:
        """Repair issue while the hub trusts the public dev key (anyone can sign for it)."""
        iid = f"{ISSUE_OTA_DEV_KEY}_{self.hub_id}"
        if self.client.ota_key == "dev":
            ir.async_create_issue(
                self.hass,
                DOMAIN,
                iid,
                is_fixable=False,
                severity=ir.IssueSeverity.WARNING,
                translation_key=ISSUE_OTA_DEV_KEY,
                translation_placeholders={"hub": self.hub_id},
            )
        else:
            ir.async_delete_issue(self.hass, DOMAIN, iid)

    async def async_install_firmware(self, image: Image, *, allow_dev_key: bool = False) -> None:
        """Upload a (client-checked) signed image; the hub verifies, applies and restarts itself.

        The upload (begin + chunks) is resumable and retried; ota_end is sent exactly once. Its reply can be
        lost (the hub verifies, copies and exits) or late, and resending would apply the image a second time
        and make the new build its own rollback copy, so after ota_end only the hub's reconnect and its
        reported build decide the outcome."""
        if self.client.ota_key == "dev" and not allow_dev_key:
            raise invalid("ota_dev_key")
        if self._ota_lock.locked():
            raise invalid("ota_in_progress")
        async with self._ota_lock:
            self.ota_build = image.build
            self._rolled_back.discard(image.build)
            self._set_ota("uploading", progress=0)
            attempts = 0
            while True:
                try:
                    await self._upload(image)
                    break
                except HarmonyConnectionError as exc:
                    attempts += 1
                    if attempts >= UPLOAD_ATTEMPTS or not await self._wait_connected():
                        self._set_ota("failed", error=str(exc))
                        raise ha_error("ota_failed", error=exc) from exc
                except HarmonyCommandError as exc:
                    self._set_ota("failed", error=f"{exc.code}: {exc.message}")
                    raise ha_error("ota_failed", error=f"{exc.code}: {exc.message}") from exc
            self._set_ota("applying", progress=100)
            try:
                await self.client.async_send(MsgType.OTA_END, {})
            except HarmonyCommandError as exc:  # the hub answered: it refused, nothing was applied
                self._set_ota("failed", error=f"{exc.code}: {exc.message}")
                raise ha_error("ota_failed", error=f"{exc.code}: {exc.message}") from exc
            except HarmonyConnectionError:
                pass  # outcome unknown: judged below by what the hub reports after it comes back
            if not await self._await_build(image.build):
                self._set_ota("failed", error="the hub did not report the new build")
                raise ha_error("ota_failed", error="the hub did not report the new build")
            self._clear_ota_issues()
            self._set_ota("installed", progress=100)

    async def _await_build(self, build: int, tries: int = 480) -> bool:
        """Wait (generously: apply + restart is a minute or two) for a connected hub reporting `build`;
        a rollback of that build reported meanwhile ends it."""
        for _ in range(tries):
            if self.client.connected and self.client.fw_build == build:
                return True
            if build in self._rolled_back:
                return False
            await self.async_sleep(0.5)
        return False

    async def _upload(self, image: Image) -> None:
        reply = await self.client.async_send(
            MsgType.OTA_BEGIN,
            {"target": TARGET_HUB, "size": image.size, "build": image.build, "sha": image.sha512},
        )
        off = reply["offset"]  # where the hub already holds the image (a resume)
        while off < image.size:
            await self.client.async_send(
                MsgType.OTA_CHUNK, {"offset": off, "data": encode_chunk(image, off)}
            )
            off = min(off + CHUNK, image.size)
            self._set_ota("uploading", progress=off * 100 // image.size)

    # ------------------------------------------------------------ Home Assistant actions
    @property
    def current_activity_id(self) -> str | None:
        ids = self.compiled.activity_ids
        idx = self._current_idx
        return ids[idx] if idx is not None and 0 <= idx < len(ids) else None

    @callback
    def _dispatch_ha(self, key: int, action: str) -> None:
        """A key press (or hold repeat) -> the HA service of the hub's current activity layer.
        Dict lookups only; the call is started in this very callback. Never raises."""
        try:
            if action == "release":
                self._released.add(key)
            elif action == "press":
                self._released.discard(key)
            elif key in self._released:
                return  # a stray hold after the release: repeats stop with the release
            tables = self.compiled.ha_keys
            table = tables.get(self.current_activity_id)
            if table is None:
                table = tables.get(None)
            call = table.get(key) if table else None
            if call is None:
                return
            # PRESS fires once; the remote sends HOLD every 400 ms while the key stays down, which
            # is the cadence of a `repeat: true` action. RELEASE does nothing.
            if action == "press" or (action == "hold" and call.repeat):
                self._fire(call, hold=action == "hold")
        except Exception:
            _LOGGER.exception("Home Assistant action for key %s failed", key)

    def _warn_once(self, key: tuple[str, ...], msg: str, *args: Any) -> None:
        if key not in self._warned:
            self._warned.add(key)
            _LOGGER.warning(msg, *args)

    def _call_problem(self, call: HaCall) -> tuple[str, dict[str, str]] | None:
        if not allowed(call.service):  # defence in depth: the bundle came from the add-on
            return "service_not_allowed", {"service": call.service}
        if not self.hass.services.has_service(call.domain, call.name):
            return "service_missing", {"service": call.service}
        return None

    def _warn_missing_entities(self, call: HaCall) -> None:
        for entity in call.entities:
            if self.hass.states.get(entity) is None:
                self._warn_once(
                    (call.service, entity), "%s targets %s, which does not exist", call.service, entity
                )

    @staticmethod
    def _call_data(call: HaCall) -> dict[str, Any]:
        return {**call.data, "entity_id": list(call.entities)}

    @callback
    def _fire(self, call: HaCall, *, hold: bool = False) -> None:
        """Start one service call now (a key event or an activity step): own context, not awaited
        by the caller, run to completion in its own task and only logged when it fails. A hold
        repeat is dropped while the same call is still running, so a slow service never queues."""
        if (problem := self._call_problem(call)) is not None:
            key, _ph = problem
            text = "Refusing the service %s: not allowed" if key == "service_not_allowed" else (
                "Service %s does not exist; skipping it"
            )
            self._warn_once((call.service,), text, call.service)
            return
        self._warn_missing_entities(call)
        flight = (call.service, call.entities)
        if hold and flight in self._calls:
            return
        self._calls[flight] = self._calls.get(flight, 0) + 1
        self.hass.async_create_task(self._run_call(call, flight), eager_start=True)

    async def _run_call(self, call: HaCall, flight: tuple[str, tuple[str, ...]]) -> None:
        try:
            await self.hass.services.async_call(
                call.domain, call.name, self._call_data(call), blocking=True, context=Context()
            )
        except Exception as exc:  # noqa: BLE001  (a key event or step never raises)
            self._warn_once((call.service, "error"), "%s failed: %s", call.service, exc)
        finally:
            left = self._calls.get(flight, 1) - 1
            if left > 0:
                self._calls[flight] = left
            else:
                self._calls.pop(flight, None)

    async def async_run_call(self, call: HaCall) -> None:
        """A call the user asked for (button, remote.send_command): awaited, and a failure raises."""
        if (problem := self._call_problem(call)) is not None:
            key, ph = problem
            raise invalid(key, **ph)
        self._warn_missing_entities(call)
        try:
            await self.hass.services.async_call(
                call.domain, call.name, self._call_data(call), blocking=True, context=Context()
            )
        except HomeAssistantError:
            raise
        except Exception as exc:
            raise ha_error("service_failed", service=call.service, error=exc) from exc

    # -- activities with Home Assistant steps
    def _ha_power(self, device: str) -> bool | None:
        st = self.device_states.get(device)
        return st.power if st else None

    def _note_activity(self, before: str | None, after: str | None) -> None:
        if not self._act_known:
            self._act_known = True
            return
        if before == after:
            return
        c = self.compiled
        if before not in c.ha_activities and after not in c.ha_activities:
            return
        if self._ha_task is not None and not self._ha_task.done():
            self._ha_task.cancel()
        self._ha_task = self.config_entry.async_create_background_task(
            self.hass, self._run_transition(before, after), name=f"{DOMAIN} activity steps"
        )

    def _transition_steps(self, before: str | None, after: str | None) -> list[HaStep]:
        """What Home Assistant does once the hub reports `after` running (or everything off):
        power off the HA devices `before` used that `after` does not, then (on Off) before's `off`
        steps, then after's power-on steps, inputs and extra steps (docs/ha-config.md)."""
        c = self.compiled
        steps: list[HaStep] = []
        new = c.ha_activities.get(after)
        keep = set(new.devices) if new else set()
        old = c.ha_activities.get(before)
        if old is not None:
            for dev in old.devices:
                off = c.ha_devices[dev].off
                if dev not in keep and off is not None:
                    steps.append(HaStep(off, dev, 0, "power_off"))
            if after is None:
                steps += old.off
        if new is not None:
            steps += new.start
        return steps

    async def _run_transition(self, before: str | None, after: str | None) -> None:
        """Sequential and cancellable: a newer transition replaces it."""
        ran_power: set[str | None] = set()
        for step in self._transition_steps(before, after):
            state = self._ha_power(step.device) if step.device else None
            if step.kind == "power_on" and state is True:
                continue  # already on: the entity is the truth
            if step.kind == "power_off" and state is False:
                continue
            if step.kind == "power_on":
                ran_power.add(step.device)
            if step.wait_ms and (step.kind != "input" or step.device in ran_power):
                await self._sleep(step.wait_ms / 1000)
            self._fire(step.call)

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
            raise ha_error("hub_rejected", error=exc.message) from exc
        except HarmonyConnectionError as exc:
            raise ha_error("hub_unreachable", error=exc) from exc
        if not res.get("ok"):
            raise ha_error("hub_failed", error=res.get("message", "unknown error"))

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
        if args["device"] in self.compiled.ha_devices:
            raise invalid("ha_device_state", device=device)
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
        if (ha := self.compiled.ha_devices.get(device)) is not None:
            call = ha.lookup(command)
            if call is None:
                raise invalid("unknown_command", device=device, command=command)
            await self.async_run_call(call)
            return
        await self.async_command(
            HUB_DEVICE, CMD_DEVICE_COMMAND, {"device": device, "command": command}
        )
