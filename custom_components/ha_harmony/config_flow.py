"""Config flow: manual setup, zeroconf discovery, reauth."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

import voluptuous as vol
from homeassistant.config_entries import ConfigFlow, ConfigFlowResult
from homeassistant.const import CONF_HOST, CONF_PORT
from homeassistant.core import HomeAssistant
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers.service_info.zeroconf import ZeroconfServiceInfo

from .api import HarmonyAuthError, HarmonyConnectionError, HarmonyHubClient, HarmonyVersionError
from .const import CONF_TOKEN, DEFAULT_PORT, DOMAIN


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
