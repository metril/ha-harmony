"""Tracked power state of each configured device (from the hub's device_state pushes)."""

from __future__ import annotations

from homeassistant.components.binary_sensor import BinarySensorDeviceClass, BinarySensorEntity
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from . import HarmonyConfigEntry
from .base_entity import HarmonyEntity
from .coordinator import HarmonyCoordinator, topic_device
from .entity_sync import setup_dynamic

PARALLEL_UPDATES = 0  # push-based: commands go through the hub client, which pipelines them


async def async_setup_entry(
    hass: HomeAssistant,
    entry: HarmonyConfigEntry,
    async_add_entities: AddConfigEntryEntitiesCallback,
) -> None:
    setup_dynamic(
        entry, async_add_entities, lambda c: [HarmonyDevicePower(c, d) for d in c.compiled.devices]
    )


class HarmonyDevicePower(HarmonyEntity, BinarySensorEntity):
    _attr_device_class = BinarySensorDeviceClass.POWER
    _attr_translation_key = "device_power"

    def __init__(self, coordinator: HarmonyCoordinator, device: str) -> None:
        super().__init__(
            coordinator, f"dev_{device}_power", device=device, topics=(topic_device(device),)
        )
        self._device = device

    @property
    def is_on(self) -> bool | None:
        st = self.coordinator.device_states.get(self._device)
        return st.power if st else None
