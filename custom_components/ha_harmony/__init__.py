"""Home Assistant integration for custom-firmware Harmony hubs."""

from __future__ import annotations

from pathlib import Path
from typing import Any

import voluptuous as vol
from homeassistant.config_entries import ConfigEntry, ConfigEntryState
from homeassistant.const import CONF_HOST, CONF_PORT, EVENT_HOMEASSISTANT_STOP, Platform
from homeassistant.core import Event, HomeAssistant, ServiceCall
from homeassistant.exceptions import (
    ConfigEntryAuthFailed,
    ConfigEntryError,
    ConfigEntryNotReady,
    ServiceValidationError,
)
from homeassistant.helpers import config_validation as cv
from homeassistant.helpers import device_registry as dr
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers.typing import ConfigType

from .api import HarmonyAuthError, HarmonyConnectionError, HarmonyHubClient, HarmonyVersionError
from .base_entity import hub_device_info
from .config_model import validate_ir
from .const import CLIENT_KWARGS, CONF_TOKEN, CONFIG_SUBDIR, DOMAIN
from .coordinator import HarmonyCoordinator

PLATFORMS = [
    Platform.BINARY_SENSOR,
    Platform.BUTTON,
    Platform.EVENT,
    Platform.REMOTE,
    Platform.SELECT,
    Platform.SENSOR,
]

type HarmonyConfigEntry = ConfigEntry[HarmonyCoordinator]

CONFIG_SCHEMA = cv.config_entry_only_config_schema(DOMAIN)

_TARGET = {vol.Optional("config_entry"): cv.string}
SERVICE_SCHEMAS: dict[str, vol.Schema] = {
    "reload_config": vol.Schema(_TARGET),
    "push_pages": vol.Schema(_TARGET),
    "send_ir": vol.Schema({**_TARGET, vol.Required("ir"): dict}),
    "set_device_state": vol.Schema(
        {
            **_TARGET,
            vol.Required("device"): cv.string,
            vol.Optional("power"): cv.boolean,
            vol.Optional("input"): vol.Any(None, cv.string),
        }
    ),
}


def _coordinators(hass: HomeAssistant, call: ServiceCall) -> list[HarmonyCoordinator]:
    entries = [
        e for e in hass.config_entries.async_entries(DOMAIN) if e.state is ConfigEntryState.LOADED
    ]
    if (wanted := call.data.get("config_entry")) is not None:
        entries = [e for e in entries if wanted in (e.entry_id, e.unique_id)]
        if not entries:
            raise ServiceValidationError(f"No loaded ha_harmony entry matches {wanted!r}")
    elif len(entries) > 1 and call.service in ("send_ir", "set_device_state"):
        raise ServiceValidationError("Several hubs are configured: pass `config_entry`")
    if not entries:
        raise ServiceValidationError("No loaded ha_harmony hub")
    return [e.runtime_data for e in entries]


async def async_setup(hass: HomeAssistant, config: ConfigType) -> bool:
    async def handle(call: ServiceCall) -> None:
        for coord in _coordinators(hass, call):
            if call.service == "reload_config":
                await coord.async_reload_config()
            elif call.service == "push_pages":
                await coord.async_push_pages()
            elif call.service == "send_ir":
                base = Path(hass.config.path(CONFIG_SUBDIR))
                try:
                    action = await hass.async_add_executor_job(validate_ir, call.data["ir"], base)
                except vol.Invalid as exc:
                    raise ServiceValidationError(f"Invalid IR action: {exc.error_message}") from exc
                await coord.async_send_ir(action)
            else:
                args: dict[str, Any] = {}
                if "power" in call.data:
                    args["power"] = call.data["power"]
                if "input" in call.data:
                    args["input"] = call.data["input"]
                if not args:
                    raise ServiceValidationError("Give `power` and/or `input`")
                await coord.async_set_device_state(call.data["device"], **args)

    for name, schema in SERVICE_SCHEMAS.items():
        hass.services.async_register(DOMAIN, name, handle, schema=schema)
    return True


async def async_setup_entry(hass: HomeAssistant, entry: HarmonyConfigEntry) -> bool:
    client = HarmonyHubClient(
        entry.data[CONF_HOST],
        entry.data[CONF_PORT],
        entry.data[CONF_TOKEN],
        async_get_clientsession(hass),
        **CLIENT_KWARGS,
    )
    try:
        await client.async_connect()
    except HarmonyAuthError as exc:
        raise ConfigEntryAuthFailed("The hub rejected the saved token") from exc
    except HarmonyVersionError as exc:
        # Permanent: no retry loop. The user must align hub firmware and integration.
        raise ConfigEntryError(
            f"Hub and integration use incompatible protocol versions: {exc}"
        ) from exc
    except HarmonyConnectionError as exc:
        raise ConfigEntryNotReady(str(exc)) from exc

    if client.hub_id != entry.unique_id:
        await client.async_close()
        raise ConfigEntryError("Host now answers as a different hub; re-add the integration")

    coordinator = HarmonyCoordinator(hass, entry, client)
    entry.runtime_data = coordinator

    async def _stop(_: Event) -> None:
        await coordinator.async_shutdown()

    entry.async_on_unload(hass.bus.async_listen_once(EVENT_HOMEASSISTANT_STOP, _stop))
    entry.async_on_unload(coordinator.async_shutdown)
    # The hub device must exist before sub-devices reference it via_device.
    dr.async_get(hass).async_get_or_create(
        config_entry_id=entry.entry_id, **_device_kwargs(coordinator)
    )
    await coordinator.async_load_config()  # a bad/missing file never blocks setup
    await coordinator.async_sync_config()
    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)
    return True


def _device_kwargs(coordinator: HarmonyCoordinator) -> dict[str, Any]:
    info = hub_device_info(coordinator)
    return {
        "identifiers": info["identifiers"],
        "name": info["name"],
        "manufacturer": info["manufacturer"],
        "sw_version": info["sw_version"],
    }


async def async_unload_entry(hass: HomeAssistant, entry: HarmonyConfigEntry) -> bool:
    return await hass.config_entries.async_unload_platforms(entry, PLATFORMS)
