"""Shared entity base: stable ids from hub_id, availability from the connection."""

from __future__ import annotations

from homeassistant.helpers import device_registry as dr
from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from .const import DOMAIN
from .coordinator import HarmonyCoordinator


def hub_device_info(coordinator: HarmonyCoordinator) -> DeviceInfo:
    return DeviceInfo(
        identifiers={(DOMAIN, coordinator.hub_id)},
        name=f"Harmony hub {coordinator.hub_id}",
        manufacturer="Custom firmware",
        sw_version=coordinator.fw_version,
    )


class HarmonyEntity(CoordinatorEntity[HarmonyCoordinator]):
    """`device` puts the entity on a YAML device's sub-device, `remote` on a remote's."""

    _attr_has_entity_name = True

    def __init__(
        self,
        coordinator: HarmonyCoordinator,
        key: str,
        *,
        device: str | None = None,
        remote: int | None = None,
    ) -> None:
        super().__init__(coordinator)
        hub = coordinator.hub_id
        hub_dev = dr.async_get(coordinator.hass).async_get_device_by_identifier(
            (DOMAIN, hub), coordinator.config_entry.entry_id
        )
        via = {"via_device_id": hub_dev.id} if hub_dev else {}
        self._attr_unique_id = f"{hub}_{key}"
        if device is not None:
            self._attr_device_info = DeviceInfo(
                identifiers={(DOMAIN, f"{hub}_dev_{device}")},
                name=coordinator.compiled.devices[device].name,
                **via,
            )
        elif remote is not None:
            self._attr_device_info = DeviceInfo(
                identifiers={(DOMAIN, f"{hub}_remote_{remote}")},
                name=f"Remote {remote}",
                **via,
            )
        else:
            self._attr_device_info = hub_device_info(coordinator)

    @property
    def available(self) -> bool:
        return self.coordinator.available
