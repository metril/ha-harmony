"""Rename a hub (id and display name) without losing anything that refers to it.

The hub id is the config entry's unique_id, part of every entity unique_id and device identifier, the key
of the cached add-on config in `.storage`, and the key the add-on stores the config under. A rename moves
all of them in one transaction: every step returns its own undo, and a failure at any point runs the undos
of the steps already done in reverse (and sets the hub back to its old id), so it never ends half-migrated.
Entity ids are NOT touched: only unique_ids move, so history, customisations, dashboards and automations
keep working.
"""

from __future__ import annotations

import asyncio
import logging
import re
from collections.abc import Awaitable, Callable
from typing import Any

from homeassistant.config_entries import ConfigEntry, ConfigEntryState
from homeassistant.const import CONF_HOST, CONF_PORT
from homeassistant.core import HomeAssistant
from homeassistant.helpers import device_registry as dr
from homeassistant.helpers import entity_registry as er
from homeassistant.helpers import issue_registry as ir
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers.storage import Store

from .addon_link import AddonClient, AddonError, AddonOutdated
from .api import HarmonyCommandError, HarmonyConnectionError, HarmonyError, HarmonyHubClient
from .const import (
    CLIENT_KWARGS,
    CONF_ADDON_HOST,
    CONF_ADDON_PORT,
    CONF_ADDON_TOKEN,
    CONF_CAN_RENAME,
    CONF_HUB_NAME,
    CONF_HW_ID,
    CONF_ID_PINNED,
    CONF_PREV_ID,
    CONF_TOKEN,
    DOMAIN,
    HUB_NAME_MAX,
    STORE_VERSION,
)
from .coordinator import (
    ISSUE_ADDON_OUTDATED,
    ISSUE_INVALID,
    ISSUE_OTA_DEV_KEY,
    ISSUE_OTA_ROLLBACK,
    ISSUE_REJECTED,
    ISSUE_SCHEMA,
    ISSUE_YAML,
)

_LOGGER = logging.getLogger(__name__)

_ID_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9_-]{0,62}")  # no dot: not valid in the mDNS label
Undo = Callable[[], Awaitable[None]]


ISSUE_ADDON_STORE = (
    "addon_store_exists"  # + _<hub>: the add-on holds a config under the new id AND the old one
)


def entry_lock(hass: HomeAssistant, entry: ConfigEntry) -> asyncio.Lock:
    """One lock per entry: a rename and the setup-follow migration never run at the same time."""
    return hass.data.setdefault(f"{DOMAIN}_rename_locks", {}).setdefault(
        entry.entry_id, asyncio.Lock()
    )


class RenameError(Exception):
    """A rename was refused or failed; nothing has changed. `key` is a config-flow error key."""

    def __init__(self, key: str, detail: str = "") -> None:
        super().__init__(f"{key}: {detail}" if detail else key)
        self.key = key
        self.detail = detail


class AddonStoreExists(RenameError):
    """The add-on already holds a store under the new id (it never overwrites one)."""

    def __init__(self) -> None:
        super().__init__("addon_store_exists")


def valid_hub_id(hub_id: str) -> bool:
    """The rule for a NEW id: [A-Za-z0-9_-], 1-63 characters (an id already in use may still contain a dot), starting with a letter or digit (the add-on keys files by
    it), and not the add-on's placeholder `default`."""
    return _ID_RE.fullmatch(hub_id) is not None and hub_id != "default"


def valid_hub_name(name: str) -> bool:
    return len(name.encode()) <= HUB_NAME_MAX and not any(
        ord(c) < 0x20 or ord(c) == 0x7F for c in name
    )


def default_title(hub_id: str, name: str) -> str:
    return name or f"Harmony hub {hub_id}"


def _swap(identifier: str, old: str, new: str) -> str | None:
    """The new form of one of this hub's identifiers / unique_ids (`<id>`, `<id>_<rest>`), else None."""
    if identifier == old:
        return new
    if identifier.startswith(f"{old}_"):
        return f"{new}_{identifier[len(old) + 1 :]}"
    return None


# --------------------------------------------------------------------------------------------- steps
async def _copy_bundle_store(hass: HomeAssistant, old: str, new: str) -> Undo:
    """The cached add-on config follows the id (the old copy is deleted last, by `_finish`)."""
    data = await Store(hass, STORE_VERSION, f"{DOMAIN}.bundle.{old}").async_load()
    target = Store(hass, STORE_VERSION, f"{DOMAIN}.bundle.{new}")
    if data is None:
        return _noop
    await target.async_save(data)

    async def undo() -> None:
        await target.async_remove()

    return undo


async def _noop() -> None:
    return None


async def _move_devices(hass: HomeAssistant, entry: ConfigEntry, old: str, new: str) -> Undo:
    """Device identifiers: the same devices keep their areas and names, no duplicates appear."""
    reg = dr.async_get(hass)
    done: list[tuple[str, set[tuple[str, str]]]] = []

    async def undo() -> None:
        for device_id, before in reversed(done):
            reg.async_update_device(device_id, new_identifiers=before)

    try:
        for dev in dr.async_entries_for_config_entry(reg, entry.entry_id):
            moved = {
                (dom, _swap(i, old, new) or i) if dom == DOMAIN else (dom, i)
                for dom, i in dev.identifiers
            }
            if moved == dev.identifiers:
                continue
            reg.async_update_device(dev.id, new_identifiers=moved)
            done.append((dev.id, set(dev.identifiers)))
    except Exception:
        await undo()
        raise
    return undo


async def _move_entities(hass: HomeAssistant, entry: ConfigEntry, old: str, new: str) -> Undo:
    """Entity unique_ids through the registry helper; entity_ids stay as they are."""
    reg = er.async_get(hass)
    done: list[tuple[str, str, str]] = []  # (entity_id, old unique_id, new unique_id)

    def forward(ent: er.RegistryEntry) -> dict[str, Any] | None:
        target = _swap(ent.unique_id, old, new)
        if target is None or target == ent.unique_id:
            return None
        done.append((ent.entity_id, ent.unique_id, target))
        return {"new_unique_id": target}

    async def undo() -> None:
        for entity_id, before, target in reversed(done):
            cur = reg.async_get(entity_id)
            if cur is not None and cur.unique_id == target:  # only what really moved
                reg.async_update_entity(entity_id, new_unique_id=before)

    try:
        await er.async_migrate_entries(hass, entry.entry_id, forward)
    except Exception:
        await undo()
        raise
    return undo


async def _update_entry(
    hass: HomeAssistant, entry: ConfigEntry, new: str, title: str, data: dict[str, Any]
) -> Undo:
    before = (entry.unique_id, entry.title, dict(entry.data))
    hass.config_entries.async_update_entry(entry, unique_id=new, title=title, data=data)

    async def undo() -> None:
        hass.config_entries.async_update_entry(
            entry, unique_id=before[0], title=before[1], data=before[2]
        )

    return undo


def _addon_client(hass: HomeAssistant, data: dict[str, Any]) -> AddonClient | None:
    if not data.get(CONF_ADDON_HOST):
        return None
    return AddonClient(
        async_get_clientsession(hass),
        data[CONF_ADDON_HOST],
        data[CONF_ADDON_PORT],
        data[CONF_ADDON_TOKEN],
    )


async def _addon_move(
    hass: HomeAssistant,
    data: dict[str, Any],
    src: str,
    dst: str,
    hw_id: str | None,
    following: bool = False,
) -> Undo:
    """Ask the add-on to move the hub's store to `dst` now (it would also do it on the next hello, from
    the same fields). An unreachable add-on is not a failure (the hello carries the old id until it has
    seen it); an add-on that answers `exists` refuses to overwrite a store already under `dst`, and one that
    has no channel or never answers `adopt` is too old to move the store (the rename is refused)."""
    client = _addon_client(hass, data)
    if client is None:
        return _noop
    client.identity = {"prev": src, **({"hw": hw_id} if hw_id else {})}
    try:
        await client.hello_once(dst)
    except AddonOutdated:
        if following:
            return _noop
        raise RenameError("addon_too_old") from None
    except AddonError:
        return _noop
    if (
        client.last_adopt is None
    ):  # answered without `adopt`: an add-on that ignores the rename fields
        if following:
            return _noop
        raise RenameError("addon_too_old")
    if client.last_adopt == "exists":
        raise AddonStoreExists
    if client.last_adopt != "moved":
        return _noop

    async def undo() -> None:
        back = _addon_client(hass, data)
        if back is None:
            return
        back.identity = {"prev": dst, **({"hw": hw_id} if hw_id else {})}
        try:
            await back.hello_once(src)
        except AddonError:
            _LOGGER.warning("Could not move the add-on config of hub %s back to %s", dst, src)

    return undo


async def _finish(hass: HomeAssistant, old: str, new: str) -> None:
    """After a committed rename: drop what only the old id used. Best effort, never fails the rename."""
    try:
        await Store(hass, STORE_VERSION, f"{DOMAIN}.bundle.{old}").async_remove()
        clear_hub_issues(hass, old)
    except Exception:
        _LOGGER.debug("cleanup after renaming %s to %s failed", old, new, exc_info=True)


def clear_hub_issues(hass: HomeAssistant, hub: str) -> None:
    for key in (ISSUE_INVALID, ISSUE_REJECTED, ISSUE_SCHEMA, ISSUE_YAML):
        ir.async_delete_issue(hass, DOMAIN, f"{key}_{hub}")
    ir.async_delete_issue(hass, DOMAIN, f"{ISSUE_OTA_DEV_KEY}_{hub}")
    ir.async_delete_issue(hass, DOMAIN, f"{ISSUE_ADDON_STORE}_{hub}")
    ir.async_delete_issue(hass, DOMAIN, f"{ISSUE_ADDON_OUTDATED}_{hub}")
    prefix = f"{ISSUE_OTA_ROLLBACK}_{hub}_"
    for domain, issue_id in list(ir.async_get(hass).issues):
        if domain == DOMAIN and issue_id.startswith(prefix):
            ir.async_delete_issue(hass, DOMAIN, issue_id)


# ------------------------------------------------------------------------------------ transactions
async def async_migrate_entry(
    hass: HomeAssistant,
    entry: ConfigEntry,
    new_id: str,
    name: str,
    data_updates: dict[str, Any] | None = None,
    *,
    hw_id: str | None = None,
    following: bool = False,
) -> None:
    """Move everything of `entry` from its current hub id to `new_id` (Home Assistant's side only; the hub
    already answers to `new_id`). The entry must not be running. Raises RenameError after undoing every
    step already done. With `following` (the hub already renamed itself, nothing to roll back to) an add-on store
    under both ids is kept as it is and reported as a repair issue, and an add-on too old to move the store is
    left to move it later; neither fails."""
    old = entry.unique_id or ""
    conflict = False
    data = {**entry.data, **(data_updates or {}), CONF_HUB_NAME: name}
    if new_id != old:
        data[CONF_PREV_ID] = old
    undos: list[Undo] = []
    try:
        if new_id != old:
            undos.append(await _copy_bundle_store(hass, old, new_id))
            undos.append(await _move_devices(hass, entry, old, new_id))
            undos.append(await _move_entities(hass, entry, old, new_id))
        undos.append(await _update_entry(hass, entry, new_id, default_title(new_id, name), data))
        if new_id != old:
            try:
                undos.append(
                    await _addon_move(
                        hass, data, old, new_id, hw_id or data.get(CONF_HW_ID), following
                    )
                )
            except AddonStoreExists:
                if not following:
                    raise
                conflict = True
    except Exception as exc:
        for undo in reversed(undos):
            try:
                await undo()
            except Exception:
                _LOGGER.exception("could not undo a step of the hub rename")
        if isinstance(exc, RenameError):
            raise
        raise RenameError("migration_failed", str(exc)) from exc
    if new_id != old:
        await _finish(hass, old, new_id)
    if conflict:
        _LOGGER.warning(
            "The add-on has configs under both %s and %s; left as they are", old, new_id
        )
        ir.async_create_issue(
            hass,
            DOMAIN,
            f"{ISSUE_ADDON_STORE}_{new_id}",
            is_fixable=False,
            severity=ir.IssueSeverity.WARNING,
            translation_key=ISSUE_ADDON_STORE,
            translation_placeholders={"old": old, "new": new_id},
        )


async def async_rename_hub(
    hass: HomeAssistant,
    entry: ConfigEntry,
    client: HarmonyHubClient,
    new_id: str,
    name: str,
    data_updates: dict[str, Any] | None = None,
) -> None:
    """Rename the hub itself and then Home Assistant's side of it, as one operation. `client` is a connected
    client of the hub as the entry knows it. On any failure the hub is set back to its old id and name and
    everything here is as it was (RenameError); on success the entry is reloaded under the new id."""
    async with entry_lock(hass, entry):
        await _rename_locked(hass, entry, client, new_id, name, data_updates)


async def _rename_locked(
    hass: HomeAssistant,
    entry: ConfigEntry,
    client: HarmonyHubClient,
    new_id: str,
    name: str,
    data_updates: dict[str, Any] | None,
) -> None:
    old, old_name = entry.unique_id or "", entry.data.get(CONF_HUB_NAME, "")
    if new_id != old and not valid_hub_id(new_id):  # an id already in use keeps working as it is
        raise RenameError("invalid_hub_id")
    if not valid_hub_name(name):
        raise RenameError("invalid_hub_name")
    if not client.can_rename:
        raise RenameError("rename_unsupported")
    if not client.hw_id:  # without it a crash after the hub renamed could not be followed
        raise RenameError("no_hw_id")
    if new_id != old and hass.config_entries.async_entry_for_domain_unique_id(DOMAIN, new_id):
        raise RenameError("id_in_use")
    if new_id == old and name == old_name:
        return
    if entry.state is ConfigEntryState.SETUP_IN_PROGRESS:
        raise RenameError("rename_failed", "the integration is starting; try again in a moment")
    if (
        entry.data.get(CONF_HW_ID) != client.hw_id
    ):  # BEFORE the hub renames: always recoverable at next setup
        hass.config_entries.async_update_entry(entry, data={**entry.data, CONF_HW_ID: client.hw_id})
    unloaded = (
        entry.state.recoverable
    )  # LOADED, SETUP_RETRY, SETUP_ERROR: its coordinator or retry must not run
    if unloaded:  # ... while the registries move
        await hass.config_entries.async_unload(entry.entry_id)
    hub_touched = False
    try:
        try:
            await client.async_set_identity(new_id, name)
        except HarmonyCommandError as exc:
            raise RenameError(
                "id_pinned" if exc.code == "id_pinned" else "rename_failed", exc.message
            ) from exc
        except HarmonyConnectionError as exc:
            hub_touched = True  # the answer may have been lost after the hub applied it
            raise RenameError("rename_failed", str(exc)) from exc
        hub_touched = True
        await async_migrate_entry(hass, entry, new_id, name, data_updates, hw_id=client.hw_id)
    except RenameError:
        if hub_touched:
            await _hub_back(hass, entry, client, old, old_name, data_updates)
        if unloaded:
            hass.config_entries.async_schedule_reload(entry.entry_id)
        raise
    except Exception as exc:  # a bug must not leave the hub renamed on its own
        if hub_touched:
            await _hub_back(hass, entry, client, old, old_name, data_updates)
        if unloaded:
            hass.config_entries.async_schedule_reload(entry.entry_id)
        raise RenameError("migration_failed", str(exc)) from exc
    hass.config_entries.async_schedule_reload(entry.entry_id)


async def _hub_back(
    hass: HomeAssistant,
    entry: ConfigEntry,
    client: HarmonyHubClient,
    old: str,
    old_name: str,
    data_updates: dict[str, Any] | None,
) -> None:
    """Set the hub back to its old id and name (reconnecting once when the link is gone). If even that
    fails the hub answers under the new id and the entry follows it at its next start, by hardware id."""
    try:
        await client.async_set_identity(old, old_name)
        return
    except HarmonyError:
        pass
    data = {**entry.data, **(data_updates or {})}
    fresh = HarmonyHubClient(
        data[CONF_HOST],
        data[CONF_PORT],
        data[CONF_TOKEN],
        async_get_clientsession(hass),
        transient=True,
        **CLIENT_KWARGS,
    )
    try:
        await fresh.async_connect()
        await fresh.async_set_identity(old, old_name)
    except HarmonyError as exc:
        _LOGGER.error("Could not set the hub back to id %s after a failed rename: %s", old, exc)
    finally:
        await fresh.async_close()


def identity_data(entry: ConfigEntry, client: HarmonyHubClient) -> dict[str, Any]:
    """What the entry should remember of the hub's identity (compared with entry.data to see changes)."""
    out: dict[str, Any] = {
        CONF_CAN_RENAME: client.can_rename,
        CONF_ID_PINNED: client.id_pinned,
        CONF_HUB_NAME: client.hub_name,
    }
    if client.hw_id:
        out[CONF_HW_ID] = client.hw_id
    return out
