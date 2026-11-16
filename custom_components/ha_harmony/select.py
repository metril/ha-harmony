"""Activity select: options are the hub's activities plus Off."""

from __future__ import annotations

from homeassistant.components.select import SelectEntity
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from . import HarmonyConfigEntry
from .base_entity import HarmonyEntity
from .const import OFF_OPTION
from .coordinator import HarmonyCoordinator


async def async_setup_entry(
    hass: HomeAssistant,
    entry: HarmonyConfigEntry,
    async_add_entities: AddConfigEntryEntitiesCallback,
) -> None:
    async_add_entities([HarmonyActivitySelect(entry.runtime_data)])


class HarmonyActivitySelect(HarmonyEntity, SelectEntity):
    _attr_translation_key = "activity"
    _attr_name = "Activity"

    def __init__(self, coordinator: HarmonyCoordinator) -> None:
        super().__init__(coordinator, "activity")

    @property
    def options(self) -> list[str]:
        return [*self.coordinator.activities, OFF_OPTION]

    @property
    def current_option(self) -> str | None:
        return self.coordinator.current_activity or OFF_OPTION

    async def async_select_option(self, option: str) -> None:
        if option == OFF_OPTION:
            await self.coordinator.async_power_off()
        else:
            await self.coordinator.async_start_activity(option)
