"""Config flow: manual setup, zeroconf discovery, add-on (Supervisor) discovery, reauth."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

import voluptuous as vol
from homeassistant.config_entries import (
    SOURCE_IGNORE,
    ConfigEntry,
    ConfigFlow,
    ConfigFlowResult,
    OptionsFlow,
)
from homeassistant.const import CONF_HOST, CONF_PORT
from homeassistant.core import HomeAssistant
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers.service_info.hassio import HassioServiceInfo
from homeassistant.helpers.service_info.zeroconf import ZeroconfServiceInfo

from .addon_link import AddonAuthError, AddonClient, AddonError, BundleRejected, NothingApplied
from .api import HarmonyAuthError, HarmonyConnectionError, HarmonyHubClient, HarmonyVersionError
from .const import (
    ADDON_SLUG,
    CONF_ADDON_HOST,
    CONF_ADDON_PORT,
    CONF_ADDON_SLUG,
    CONF_ADDON_TOKEN,
    CONF_ADDON_UUID,
    CONF_ALL_COMMAND_BUTTONS,
    CONF_CAN_RENAME,
    CONF_HOLD_EVENTS,
    CONF_HUB_NAME,
    CONF_HW_ID,
    CONF_ID_PINNED,
    CONF_TOKEN,
    DEFAULT_PORT,
    DOMAIN,
)
from .rename import RenameError, async_rename_hub

CONF_HUB_ID = "hub_id"  # reconfigure form field: the hub's new id
HASSIO_UNIQUE_ID = "hassio_ha_harmony_editor"


async def _probe(hass: HomeAssistant, host: str, port: int, token: str) -> str:
    """Connect once and return the hub id. Raises Harmony*Error."""
    client = HarmonyHubClient(host, port, token, async_get_clientsession(hass))
    try:
        await client.async_connect()
        return client.hub_id or ""
    finally:
        await client.async_close()


async def _connect(hass: HomeAssistant, host: str, port: int, token: str) -> HarmonyHubClient:
    """A connected client (the caller closes it). Raises Harmony*Error."""
    client = HarmonyHubClient(host, port, token, async_get_clientsession(hass))
    await client.async_connect()
    return client


def _error_key(exc: Exception) -> str:
    if isinstance(exc, HarmonyAuthError):
        return "invalid_auth"
    if isinstance(exc, HarmonyVersionError):
        return "unsupported_version"
    if isinstance(exc, HarmonyConnectionError):
        return "cannot_connect"
    return "unknown"


class HarmonyOptionsFlow(OptionsFlow):
    async def async_step_init(self, user_input: dict[str, Any] | None = None) -> ConfigFlowResult:
        if user_input is not None:
            return self.async_create_entry(data=user_input)
        opts = self.config_entry.options
        return self.async_show_form(
            step_id="init",
            data_schema=vol.Schema(
                {
                    vol.Required(CONF_HOLD_EVENTS, default=opts.get(CONF_HOLD_EVENTS, False)): bool,
                    vol.Required(
                        CONF_ALL_COMMAND_BUTTONS, default=opts.get(CONF_ALL_COMMAND_BUTTONS, False)
                    ): bool,
                }
            ),
        )


class HarmonyConfigFlow(ConfigFlow, domain=DOMAIN):
    VERSION = 1

    @staticmethod
    def async_get_options_flow(config_entry: ConfigEntry) -> HarmonyOptionsFlow:
        return HarmonyOptionsFlow()

    def __init__(self) -> None:
        self._host: str | None = None
        self._port: int = DEFAULT_PORT
        self._addon: dict[str, Any] = {}
        self._hw: str | None = None  # the discovered hub's stable hardware id (TXT hw)
        self._moved_entry_id: str | None = None

    async def async_step_user(self, user_input: dict[str, Any] | None = None) -> ConfigFlowResult:
        errors: dict[str, str] = {}
        if user_input is not None:
            host, port = user_input[CONF_HOST], user_input[CONF_PORT]
            try:
                hub_id = await _probe(self.hass, host, port, user_input[CONF_TOKEN])
            except (HarmonyAuthError, HarmonyVersionError, HarmonyConnectionError) as exc:
                errors["base"] = _error_key(exc)
            else:
                await self.async_set_unique_id(hub_id)
                self._abort_if_unique_id_configured(updates={CONF_HOST: host, CONF_PORT: port})
                return self.async_create_entry(
                    title=f"Harmony hub {hub_id}",
                    data={CONF_HOST: host, CONF_PORT: port, CONF_TOKEN: user_input[CONF_TOKEN]},
                )
        schema = vol.Schema(
            {
                vol.Required(CONF_HOST, default=self._host or vol.UNDEFINED): str,
                vol.Required(CONF_PORT, default=self._port): int,
                vol.Required(CONF_TOKEN): str,
            }
        )
        return self.async_show_form(step_id="user", data_schema=schema, errors=errors)

    async def async_step_zeroconf(self, discovery_info: ZeroconfServiceInfo) -> ConfigFlowResult:
        hub_id = discovery_info.properties.get("hub_id") or discovery_info.name.split(".")[0]
        self._hw = discovery_info.properties.get("hw") or None
        await self.async_set_unique_id(hub_id)
        host, port = discovery_info.host, discovery_info.port or DEFAULT_PORT
        self.context["title_placeholders"] = {"name": hub_id}
        entry = self.hass.config_entries.async_entry_for_domain_unique_id(DOMAIN, hub_id)
        if entry is None and self._hw:
            # not under this id: the same hub renamed on the device? Its hardware id stays
            entry = next(
                (
                    e
                    for e in self.hass.config_entries.async_entries(DOMAIN)
                    if e.data.get(CONF_HW_ID) == self._hw
                ),
                None,
            )
        if entry is not None:
            # Never retarget a configured hub from an unauthenticated announcement: anyone on the
            # LAN can answer mDNS, and the next connection would send them the hub token (`hello`).
            # The user has to confirm the new address (the old one stays in use meanwhile).
            if entry.source == SOURCE_IGNORE or (
                entry.data.get(CONF_HOST) == host and entry.data.get(CONF_PORT) == port
            ):
                return self.async_abort(reason="already_configured")
            self._host, self._port = host, port
            self._moved_entry_id = entry.entry_id
            return await self.async_step_hub_moved()
        self._host, self._port = host, port
        return await self.async_step_user()

    async def async_step_hub_moved(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        """"Hub <id> moved to <host>?" The saved token is only sent after the user says yes."""
        entry = self.hass.config_entries.async_get_entry(self._moved_entry_id or "")
        if entry is None or self._host is None:
            return self.async_abort(reason="unknown")
        placeholders = {
            "name": entry.unique_id or "",
            "old_host": f"{entry.data.get(CONF_HOST)}:{entry.data.get(CONF_PORT)}",
            "new_host": f"{self._host}:{self._port}",
        }
        if user_input is None:
            return self.async_show_form(
                step_id="hub_moved", description_placeholders=placeholders
            )
        try:
            hub_id = await _probe(self.hass, self._host, self._port, entry.data[CONF_TOKEN])
        except (HarmonyAuthError, HarmonyVersionError, HarmonyConnectionError):
            return self.async_abort(reason="cannot_connect")
        renamed = (
            hub_id != entry.unique_id
            and hub_id == self.unique_id
            and self._hw is not None
            and entry.data.get(CONF_HW_ID) == self._hw
        )  # the same hub under a new id: setup follows it (its hardware id is checked there too)
        if hub_id != entry.unique_id and not renamed:
            return self.async_abort(reason="wrong_hub")
        return self.async_update_reload_and_abort(
            entry,
            data_updates={CONF_HOST: self._host, CONF_PORT: self._port},
            reason="hub_moved_updated",
        )

    async def async_step_reconfigure(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        """Change the hub's address and token, and (firmware permitting) its id and display name.

        A rename is sent to the hub and then moved through Home Assistant (entry, devices, entity
        unique_ids, cache, add-on key) as one transaction; if any step fails the hub is set back."""
        entry = self._get_reconfigure_entry()
        errors: dict[str, str] = {}
        stored_name = entry.data.get(CONF_HUB_NAME, "")
        can_rename = entry.data.get(CONF_CAN_RENAME) is not False  # unknown (never connected): offer it
        pinned = entry.data.get(CONF_ID_PINNED) is True
        detail = ""
        if user_input is not None:
            conn = {k: user_input[k] for k in (CONF_HOST, CONF_PORT, CONF_TOKEN)}
            new_id = str(user_input.get(CONF_HUB_ID, entry.unique_id)).strip()
            name = str(user_input.get(CONF_HUB_NAME, stored_name)).strip()
            try:
                client = await _connect(self.hass, conn[CONF_HOST], conn[CONF_PORT], conn[CONF_TOKEN])
            except (HarmonyAuthError, HarmonyVersionError, HarmonyConnectionError) as exc:
                errors["base"] = _error_key(exc)
            else:
                try:
                    if client.hub_id != entry.unique_id:
                        if not (client.hw_id and client.hw_id == entry.data.get(CONF_HW_ID)):
                            return self.async_abort(reason="wrong_hub")
                        # the same hub under another id (renamed on the device): save the connection, setup follows it
                        asked = (new_id, name) != (entry.unique_id, stored_name)
                        return self.async_update_reload_and_abort(
                            entry,
                            data_updates=conn,
                            reason="hub_renamed_not_applied" if asked else "reconfigure_successful",
                        )
                    if new_id == entry.unique_id and name == stored_name:
                        return self.async_update_reload_and_abort(entry, data_updates=conn)
                    try:
                        await async_rename_hub(self.hass, entry, client, new_id, name, conn)
                    except RenameError as exc:
                        errors["base"] = exc.key
                        detail = exc.detail
                    else:
                        return self.async_abort(reason="reconfigure_successful")
                finally:
                    await client.async_close()
        current = user_input or entry.data
        fields: dict[Any, Any] = {
            vol.Required(CONF_HOST, default=current[CONF_HOST]): str,
            vol.Required(CONF_PORT, default=current[CONF_PORT]): int,
            vol.Required(CONF_TOKEN, default=current[CONF_TOKEN]): str,
        }
        note = ""
        if not can_rename:
            note = "Renaming needs newer hub firmware, so the hub id and name are shown but cannot be changed here."
        else:
            if not pinned:
                fields[vol.Required(CONF_HUB_ID, default=current.get(CONF_HUB_ID, entry.unique_id))] = str
            else:
                note = "The hub id is pinned by hub_id in hub.conf on the device and cannot be changed here."
            fields[vol.Optional(CONF_HUB_NAME, default=current.get(CONF_HUB_NAME, stored_name))] = str
        return self.async_show_form(
            step_id="reconfigure",
            data_schema=vol.Schema(fields),
            errors=errors,
            description_placeholders={
                "hub_id": entry.unique_id or "",
                "hub_name": stored_name or "(none)",
                "note": note,
                "detail": detail,
            },
        )

    async def async_step_reauth(self, entry_data: Mapping[str, Any]) -> ConfigFlowResult:
        return await self.async_step_reauth_confirm()

    async def async_step_reauth_confirm(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        entry = self._get_reauth_entry()
        errors: dict[str, str] = {}
        if user_input is not None:
            try:
                hub_id = await _probe(
                    self.hass, entry.data[CONF_HOST], entry.data[CONF_PORT], user_input[CONF_TOKEN]
                )
            except (HarmonyAuthError, HarmonyVersionError, HarmonyConnectionError) as exc:
                errors["base"] = _error_key(exc)
            else:
                if hub_id != entry.unique_id:
                    return self.async_abort(reason="wrong_hub")
                return self.async_update_reload_and_abort(
                    entry, data_updates={CONF_TOKEN: user_input[CONF_TOKEN]}
                )
        return self.async_show_form(
            step_id="reauth_confirm",
            data_schema=vol.Schema({vol.Required(CONF_TOKEN): str}),
            errors=errors,
        )

    # -- Harmony Editor add-on (Supervisor discovery). The add-on serves every hub, so this flow
    # never creates an entry: it stores the add-on's address and token on the existing hub entries.
    async def async_step_hassio(self, discovery_info: HassioServiceInfo) -> ConfigFlowResult:
        slug = discovery_info.slug
        if not isinstance(slug, str) or not (slug == ADDON_SLUG or slug.endswith(f"_{ADDON_SLUG}")):
            return self.async_abort(reason="not_our_addon")  # only our add-on may offer a token
        cfg = discovery_info.config
        try:
            addon = {
                CONF_ADDON_HOST: str(cfg["host"]),
                CONF_ADDON_PORT: int(cfg["port"]),
                CONF_ADDON_TOKEN: str(cfg["token"]),
                CONF_ADDON_SLUG: slug,
                CONF_ADDON_UUID: str(discovery_info.uuid),
            }
        except KeyError, TypeError, ValueError:
            return self.async_abort(reason="invalid_discovery")
        await self.async_set_unique_id(HASSIO_UNIQUE_ID, raise_on_progress=False)
        self._abort_if_unique_id_configured()  # an ignored discovery stays ignored
        entries = self._async_current_entries(include_ignore=False)
        if not entries:
            return self.async_abort(reason="no_hub")
        self._addon = addon
        linked = [e for e in entries if e.data.get(CONF_ADDON_HOST)]
        if linked and all(e.data.get(CONF_ADDON_SLUG) == slug for e in linked):
            # same add-on again (restart/reinstall): a new host/token needs no ask
            self._link_entries(entries)
            return self.async_abort(reason="addon_updated")
        self.context["title_placeholders"] = {"name": discovery_info.name}
        return await self.async_step_hassio_confirm()

    async def async_step_hassio_confirm(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        if user_input is None:  # show what is being linked: the add-on's slug and where it answers
            return self.async_show_form(
                step_id="hassio_confirm",
                description_placeholders={
                    "slug": self._addon[CONF_ADDON_SLUG],
                    "host": f"{self._addon[CONF_ADDON_HOST]}:{self._addon[CONF_ADDON_PORT]}",
                },
            )
        entries = self._async_current_entries(include_ignore=False)
        if not entries:
            return self.async_abort(reason="no_hub")
        a = self._addon
        client = AddonClient(
            async_get_clientsession(self.hass),
            a[CONF_ADDON_HOST],
            a[CONF_ADDON_PORT],
            a[CONF_ADDON_TOKEN],
        )
        try:  # "nothing applied yet" still proves host and token
            await client.fetch(entries[0].unique_id or "")
        except AddonAuthError:
            return self.async_abort(reason="invalid_auth")
        except NothingApplied, BundleRejected:
            pass
        except AddonError:
            return self.async_abort(reason="cannot_connect")
        self._link_entries(entries)
        return self.async_abort(reason="addon_linked")

    def _link_entries(self, entries: list[Any]) -> None:
        for entry in entries:
            if all(entry.data.get(k) == v for k, v in self._addon.items() if k != CONF_ADDON_UUID):
                continue
            self.hass.config_entries.async_update_entry(entry, data={**entry.data, **self._addon})
            self.hass.config_entries.async_schedule_reload(entry.entry_id)
