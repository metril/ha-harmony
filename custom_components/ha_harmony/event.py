"""Key events from paired remotes (one event entity per remote)."""

from __future__ import annotations

from homeassistant.components.event import EventEntity
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from . import HarmonyConfigEntry
from .base_entity import HarmonyEntity
from .coordinator import TOPIC_CONFIG, HarmonyCoordinator


async def async_setup_entry(
    hass: HomeAssistant,
    entry: HarmonyConfigEntry,
    async_add_entities: AddConfigEntryEntitiesCallback,
) -> None:
    coordinator = entry.runtime_data
    async_add_entities(HarmonyKeyEvent(coordinator, r) for r in coordinator.remotes)


class HarmonyKeyEvent(HarmonyEntity, EventEntity):
    def __init__(self, coordinator: HarmonyCoordinator, remote: int) -> None:
        super().__init__(coordinator, f"key_{remote}", topics=(TOPIC_CONFIG,))
        self._remote = remote
        self._attr_name = f"Remote {remote} keys"

    @property
    def event_types(self) -> list[str]:
        return self.coordinator.event_types

    async def async_added_to_hass(self) -> None:
        await super().async_added_to_hass()
        self.async_on_remove(self.coordinator.async_add_key_listener(self._on_key))

    @callback
    def _on_key(self, remote: int, key: int, action: str) -> None:
        if remote != self._remote:
            return
        self._trigger_event(
            self.coordinator.key_name(key), {"key": key, "action": action, "remote": remote}
        )
        self.async_write_ha_state()
