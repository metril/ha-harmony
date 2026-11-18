"""Config flow: manual setup, zeroconf discovery, add-on (Supervisor) discovery, reauth."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

import voluptuous as vol
from homeassistant.config_entries import ConfigFlow, ConfigFlowResult
from homeassistant.const import CONF_HOST, CONF_PORT
from homeassistant.core import HomeAssistant
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers.service_info.hassio import HassioServiceInfo
from homeassistant.helpers.service_info.zeroconf import ZeroconfServiceInfo

from .addon_link import AddonAuthError, AddonClient, AddonError, BundleRejected, NothingApplied
from .api import HarmonyAuthError, HarmonyConnectionError, HarmonyHubClient, HarmonyVersionError
from .const import (
    CONF_ADDON_HOST,
    CONF_ADDON_PORT,
    CONF_ADDON_TOKEN,
    CONF_TOKEN,
    DEFAULT_PORT,
    DOMAIN,
)

HASSIO_UNIQUE_ID = "hassio_ha_harmony_editor"


async def _probe(hass: HomeAssistant, host: str, port: int, token: str) -> str:
    """Connect once and return the hub id. Raises Harmony*Error."""
    client = HarmonyHubClient(host, port, token, async_get_clientsession(hass))
    try:
        await client.async_connect()
        return client.hub_id or ""
    finally:
        await client.async_close()


def _error_key(exc: Exception) -> str:
    if isinstance(exc, HarmonyAuthError):
        return "invalid_auth"
    if isinstance(exc, HarmonyVersionError):
        return "unsupported_version"
    if isinstance(exc, HarmonyConnectionError):
        return "cannot_connect"
    return "unknown"


class HarmonyConfigFlow(ConfigFlow, domain=DOMAIN):
    VERSION = 1

    def __init__(self) -> None:
        self._host: str | None = None
        self._port: int = DEFAULT_PORT
        self._addon: dict[str, Any] = {}

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
        await self.async_set_unique_id(hub_id)
        self._abort_if_unique_id_configured(
            updates={CONF_HOST: discovery_info.host, CONF_PORT: discovery_info.port}
        )
        self._host = discovery_info.host
        self._port = discovery_info.port or DEFAULT_PORT
        self.context["title_placeholders"] = {"name": hub_id}
        return await self.async_step_user()

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
        cfg = discovery_info.config
        try:
            addon = {
                CONF_ADDON_HOST: str(cfg["host"]),
                CONF_ADDON_PORT: int(cfg["port"]),
                CONF_ADDON_TOKEN: str(cfg["token"]),
            }
        except KeyError, TypeError, ValueError:
            return self.async_abort(reason="invalid_discovery")
        await self.async_set_unique_id(HASSIO_UNIQUE_ID, raise_on_progress=False)
        entries = self._async_current_entries()
        if not entries:
            return self.async_abort(reason="no_hub")
        self._addon = addon
        if any(e.data.get(CONF_ADDON_HOST) for e in entries):
            # already linked once: a new host/token (add-on restarted or reinstalled) needs no ask
            self._link_entries(entries)
            return self.async_abort(reason="addon_updated")
        self.context["title_placeholders"] = {"name": discovery_info.name}
        return await self.async_step_hassio_confirm()

    async def async_step_hassio_confirm(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        if user_input is None:
            return self.async_show_form(step_id="hassio_confirm")
        entries = self._async_current_entries()
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
            if all(entry.data.get(k) == v for k, v in self._addon.items()):
                continue
            self.hass.config_entries.async_update_entry(entry, data={**entry.data, **self._addon})
            self.hass.config_entries.async_schedule_reload(entry.entry_id)
