"""Shared entity base: stable ids from hub_id, availability from the connection."""

from __future__ import annotations

from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from .const import DOMAIN
from .coordinator import HarmonyCoordinator


class HarmonyEntity(CoordinatorEntity[HarmonyCoordinator]):
    _attr_has_entity_name = True

    def __init__(self, coordinator: HarmonyCoordinator, key: str) -> None:
        super().__init__(coordinator)
        self._attr_unique_id = f"{coordinator.hub_id}_{key}"
        self._attr_device_info = DeviceInfo(
            identifiers={(DOMAIN, coordinator.hub_id)},
            name=f"Harmony hub {coordinator.hub_id}",
            manufacturer="Custom firmware",
            sw_version=coordinator.fw_version,
        )

    @property
    def available(self) -> bool:
        return self.coordinator.available
