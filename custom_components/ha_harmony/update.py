"""Hub firmware update: the signed image bundled in the add-on, installed over the existing OTA path.

Pushed, never polled: the add-on announces its image on every channel (re)connect (coordinator `addon_fw`), the
hub reports its own version and build in hello_ack, and the OTA state changes arrive on TOPIC_OTA."""

from __future__ import annotations

import re
from typing import Any

from homeassistant.components.update import (
    UpdateDeviceClass,
    UpdateEntity,
    UpdateEntityFeature,
)
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from . import HarmonyConfigEntry
from .base_entity import HarmonyEntity
from .coordinator import TOPIC_OTA, HarmonyCoordinator
from .entity_sync import setup_dynamic

PARALLEL_UPDATES = 0  # push-based; one install at a time is enforced by the coordinator's OTA lock
_BUSY = ("uploading", "applying", "restarting")
_BUILD = re.compile(r"\((\d+)\)$")


async def async_setup_entry(
    hass: HomeAssistant,
    entry: HarmonyConfigEntry,
    async_add_entities: AddConfigEntryEntitiesCallback,
) -> None:
    setup_dynamic(entry, async_add_entities, lambda c: [HarmonyHubUpdate(c)])


def _label(version: str | None, build: int | None) -> str | None:
    if build is None:
        return None
    return f"{version or '?'} ({build})"


class HarmonyHubUpdate(HarmonyEntity, UpdateEntity):
    _attr_translation_key = "hub_firmware"
    _attr_device_class = UpdateDeviceClass.FIRMWARE
    _attr_supported_features = UpdateEntityFeature.INSTALL | UpdateEntityFeature.PROGRESS

    def __init__(self, coordinator: HarmonyCoordinator) -> None:
        super().__init__(coordinator, "firmware_image", topics=(TOPIC_OTA,))

    @property
    def installed_version(self) -> str | None:
        c = self.coordinator.client
        return _label(c.fw_version, c.fw_build)

    @property
    def latest_version(self) -> str | None:
        fw = self.coordinator.addon_fw
        return None if fw is None else _label(fw.version, fw.build)

    def version_is_newer(self, latest_version: str, installed_version: str) -> bool:
        """Builds decide, not version strings: a dev or re-spun build can carry any string."""
        a, b = _BUILD.search(latest_version), _BUILD.search(installed_version)
        return bool(a and b and int(a.group(1)) > int(b.group(1)))

    @property
    def available(self) -> bool:
        """Mid-install the hub reboots and drops off: this entity stays up (in_progress) until it returns on the
        new build or the coordinator gives up; every other hub entity follows the connection."""
        return self.in_progress or super().available

    @property
    def in_progress(self) -> bool:
        return self.coordinator.ota_state in _BUSY

    async def async_install(self, version: str | None, backup: bool, **kwargs: Any) -> None:
        await self.coordinator.async_install_addon_firmware()
