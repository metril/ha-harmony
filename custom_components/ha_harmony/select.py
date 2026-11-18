"""Activity select: options are the hub's activities plus Off."""

from __future__ import annotations

from homeassistant.components.select import SelectEntity
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from . import HarmonyConfigEntry
from .base_entity import HarmonyEntity
from .const import OFF_OPTION
from .coordinator import TOPIC_ACTIVITY, HarmonyCoordinator

PARALLEL_UPDATES = 0  # push-based: commands go through the hub client, which pipelines them


async def async_setup_entry(
    hass: HomeAssistant,
    entry: HarmonyConfigEntry,
    async_add_entities: AddConfigEntryEntitiesCallback,
) -> None:
    async_add_entities([HarmonyActivitySelect(entry.runtime_data)])


class HarmonyActivitySelect(HarmonyEntity, SelectEntity):
    _attr_translation_key = "activity"

    def __init__(self, coordinator: HarmonyCoordinator) -> None:
        super().__init__(coordinator, "activity", topics=(TOPIC_ACTIVITY,))

    @property
    def _off_label(self) -> str:
        """The power-off option; relabelled when an activity is itself named "Off"."""
        if OFF_OPTION in self.coordinator.activities:
            return f"{OFF_OPTION} (power off)"
        return OFF_OPTION

    @property
    def options(self) -> list[str]:
        return [*self.coordinator.activities, self._off_label]

    @property
    def current_option(self) -> str | None:
        return self.coordinator.current_activity or self._off_label

    async def async_select_option(self, option: str) -> None:
        if option == self._off_label:
            await self.coordinator.async_power_off()
        else:
            await self.coordinator.async_start_activity(option)
