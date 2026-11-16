"""Home Assistant integration for custom-firmware Harmony hubs."""

from __future__ import annotations

from homeassistant.config_entries import ConfigEntry
from homeassistant.const import CONF_HOST, CONF_PORT, EVENT_HOMEASSISTANT_STOP, Platform
from homeassistant.core import Event, HomeAssistant
from homeassistant.exceptions import ConfigEntryAuthFailed, ConfigEntryError, ConfigEntryNotReady
from homeassistant.helpers.aiohttp_client import async_get_clientsession

from .api import HarmonyAuthError, HarmonyConnectionError, HarmonyHubClient, HarmonyVersionError
from .const import CLIENT_KWARGS, CONF_TOKEN
from .coordinator import HarmonyCoordinator

PLATFORMS = [Platform.REMOTE, Platform.SELECT, Platform.EVENT]

type HarmonyConfigEntry = ConfigEntry[HarmonyCoordinator]


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
    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)
    return True


async def async_unload_entry(hass: HomeAssistant, entry: HarmonyConfigEntry) -> bool:
    return await hass.config_entries.async_unload_platforms(entry, PLATFORMS)
