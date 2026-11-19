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
)
from homeassistant.helpers import config_validation as cv
from homeassistant.helpers import device_registry as dr
from homeassistant.helpers import issue_registry as ir
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers.storage import Store
from homeassistant.helpers.typing import ConfigType

from .addon_link import AddonClient
from .api import HarmonyAuthError, HarmonyConnectionError, HarmonyHubClient, HarmonyVersionError
from .base_entity import hub_device_info
from .const import (
    CLIENT_KWARGS,
    CONF_ADDON_HOST,
    CONF_ADDON_PORT,
    CONF_ADDON_TOKEN,
    CONF_TOKEN,
    CONFIG_SUBDIR,
    DOMAIN,
    STORE_VERSION,
)
from .coordinator import (
    ISSUE_INVALID,
    ISSUE_OTA_DEV_KEY,
    ISSUE_OTA_ROLLBACK,
    ISSUE_REJECTED,
    ISSUE_SCHEMA,
    ISSUE_YAML,
    HarmonyCoordinator,
)
from .errors import invalid
from .harmony_core.config_model import validate_ir
from .ota import parse_image, path_allowed, read_image

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
    "install_firmware": vol.Schema(
        {**_TARGET, vol.Required("path"): cv.string, vol.Optional("allow_dev_key", default=False): cv.boolean}
    ),
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
            raise invalid("no_matching_hub", hub=wanted)
    elif len(entries) > 1 and call.service in ("send_ir", "set_device_state", "install_firmware"):
        raise invalid("several_hubs")
    if not entries:
        raise invalid("no_loaded_hub")
    return [e.runtime_data for e in entries]


async def _install_firmware(
    hass: HomeAssistant, coord: HarmonyCoordinator, path: str, allow_dev_key: bool
) -> None:
    """A signed image under <config>/ha_harmony/firmware (or allowlist_external_dirs). Never sends an
    unsigned file: the HOTA header is checked here for a friendly error, the hub's verify decides."""
    real = await hass.async_add_executor_job(
        path_allowed, hass.config.config_dir, hass.config.is_allowed_path, path
    )
    if real is None:
        raise invalid("ota_path_not_allowed", path=path)
    data = await hass.async_add_executor_job(read_image, real)
    await coord.async_install_firmware(parse_image(data), allow_dev_key=allow_dev_key)


async def async_setup(hass: HomeAssistant, config: ConfigType) -> bool:
    async def handle(call: ServiceCall) -> None:
        for coord in _coordinators(hass, call):
            if call.service == "reload_config":
                await coord.async_refresh_from_addon()
            elif call.service == "push_pages":
                await coord.async_push_pages()
            elif call.service == "install_firmware":
                await _install_firmware(hass, coord, call.data["path"], call.data["allow_dev_key"])
            elif call.service == "send_ir":
                try:  # no base dir: file references are refused (inline the codes)
                    action = validate_ir(call.data["ir"])
                except vol.Invalid as exc:
                    raise invalid("invalid_ir", error=exc.error_message) from exc
                await coord.async_send_ir(action)
            else:
                args: dict[str, Any] = {}
                if "power" in call.data:
                    args["power"] = call.data["power"]
                if "input" in call.data:
                    args["input"] = call.data["input"]
                if not args:
                    raise invalid("need_power_or_input")
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
    coordinator.note_ota_key()

    async def _stop(_: Event) -> None:
        await coordinator.async_shutdown()

    entry.async_on_unload(hass.bus.async_listen_once(EVENT_HOMEASSISTANT_STOP, _stop))
    entry.async_on_unload(coordinator.async_shutdown)
    # The hub device must exist before sub-devices reference it via_device.
    dr.async_get(hass).async_get_or_create(
        config_entry_id=entry.entry_id, **_device_kwargs(coordinator)
    )
    await _flag_leftover_yaml(hass, coordinator)
    await coordinator.async_load_cached()  # the remote keeps its config while the add-on is down
    if entry.data.get(CONF_ADDON_HOST):
        coordinator.async_start_link(
            AddonClient(
                async_get_clientsession(hass),
                entry.data[CONF_ADDON_HOST],
                entry.data[CONF_ADDON_PORT],
                entry.data[CONF_ADDON_TOKEN],
            )
        )
        await coordinator.async_wait_first_config()  # bounded; the loop's own (single) pull
    # the hub sync is never awaited here: entities come up and fill in as it completes
    coordinator.async_start_first_sync()
    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)
    return True


async def _flag_leftover_yaml(hass: HomeAssistant, coordinator: HarmonyCoordinator) -> None:
    """0.3.0 dropped the YAML source. Only look for the file (never read or delete it) and point
    the user at the add-on's import."""
    path = Path(hass.config.path(CONFIG_SUBDIR, f"{coordinator.hub_id}.yaml"))
    issue = f"{ISSUE_YAML}_{coordinator.hub_id}"
    if await hass.async_add_executor_job(path.is_file):
        ir.async_create_issue(
            hass,
            DOMAIN,
            issue,
            is_fixable=False,
            severity=ir.IssueSeverity.WARNING,
            translation_key=ISSUE_YAML,
            translation_placeholders={"hub": coordinator.hub_id, "path": str(path)},
        )
    else:
        ir.async_delete_issue(hass, DOMAIN, issue)


def _device_kwargs(coordinator: HarmonyCoordinator) -> dict[str, Any]:
    info = hub_device_info(coordinator)
    return {
        "identifiers": info["identifiers"],
        "name": info["name"],
        "manufacturer": info["manufacturer"],
        "sw_version": info["sw_version"],
    }


async def async_remove_entry(hass: HomeAssistant, entry: HarmonyConfigEntry) -> None:
    """The entry is deleted: drop its cached add-on bundle (`.storage`) and its repair issues."""
    hub = entry.unique_id or ""
    await Store(hass, STORE_VERSION, f"{DOMAIN}.bundle.{hub}").async_remove()
    for key in (ISSUE_INVALID, ISSUE_REJECTED, ISSUE_SCHEMA, ISSUE_YAML):
        ir.async_delete_issue(hass, DOMAIN, f"{key}_{hub}")
    ir.async_delete_issue(hass, DOMAIN, f"{ISSUE_OTA_DEV_KEY}_{hub}")
    prefix = f"{ISSUE_OTA_ROLLBACK}_{hub}_"
    for domain, issue_id in list(ir.async_get(hass).issues):
        if domain == DOMAIN and issue_id.startswith(prefix):
            ir.async_delete_issue(hass, DOMAIN, issue_id)


async def async_unload_entry(hass: HomeAssistant, entry: HarmonyConfigEntry) -> bool:
    return await hass.config_entries.async_unload_platforms(entry, PLATFORMS)
