"""Remote entity: start/stop activities and forward device commands."""

from __future__ import annotations

from collections.abc import Iterable
from typing import Any

from homeassistant.components.remote import (
    ATTR_ACTIVITY,
    ATTR_DEVICE,
    RemoteEntity,
    RemoteEntityFeature,
)
from homeassistant.core import HomeAssistant
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from . import HarmonyConfigEntry
from .base_entity import HarmonyEntity
from .const import HUB_DEVICE
from .coordinator import HarmonyCoordinator


async def async_setup_entry(
    hass: HomeAssistant,
    entry: HarmonyConfigEntry,
    async_add_entities: AddConfigEntryEntitiesCallback,
) -> None:
    async_add_entities([HarmonyRemote(entry.runtime_data)])


class HarmonyRemote(HarmonyEntity, RemoteEntity):
    _attr_name = None
    _attr_supported_features = RemoteEntityFeature.ACTIVITY

    def __init__(self, coordinator: HarmonyCoordinator) -> None:
        super().__init__(coordinator, "remote")

    @property
    def is_on(self) -> bool:
        return self.coordinator.current_activity is not None

    @property
    def current_activity(self) -> str | None:
        return self.coordinator.current_activity

    @property
    def activity_list(self) -> list[str]:
        return list(self.coordinator.activities)

    async def async_turn_on(self, **kwargs: Any) -> None:
        coord = self.coordinator
        activity = kwargs.get(ATTR_ACTIVITY) or coord.last_activity
        if activity is None and coord.activities:
            activity = coord.activities[0]
        if activity is None:
            raise HomeAssistantError("The hub has no activities configured")
        await coord.async_start_activity(activity)

    async def async_turn_off(self, **kwargs: Any) -> None:
        await self.coordinator.async_power_off()

    async def async_send_command(self, command: Iterable[str], **kwargs: Any) -> None:
        device = kwargs.get(ATTR_DEVICE) or HUB_DEVICE
        for cmd in command:
            await self.coordinator.async_command(device, cmd)
