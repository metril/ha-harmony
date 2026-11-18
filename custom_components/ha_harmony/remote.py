"""Remote entity: start/stop activities and forward device commands."""

from __future__ import annotations

from collections.abc import Iterable
from typing import Any

from homeassistant.components.remote import (
    ATTR_ACTIVITY,
    ATTR_DELAY_SECS,
    ATTR_DEVICE,
    ATTR_HOLD_SECS,
    ATTR_NUM_REPEATS,
    DEFAULT_DELAY_SECS,
    RemoteEntity,
    RemoteEntityFeature,
)
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from . import HarmonyConfigEntry
from .base_entity import HarmonyEntity
from .const import HOLD_REPEAT, HUB_DEVICE
from .coordinator import TOPIC_ACTIVITY, HarmonyCoordinator
from .errors import ha_error

PARALLEL_UPDATES = 0  # push-based: commands go through the hub client, which pipelines them


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
        super().__init__(coordinator, "remote", topics=(TOPIC_ACTIVITY,))

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
            raise ha_error("no_activities")
        await coord.async_start_activity(activity)

    async def async_turn_off(self, **kwargs: Any) -> None:
        await self.coordinator.async_power_off()

    async def async_send_command(self, command: Iterable[str], **kwargs: Any) -> None:
        """The standard arguments: `num_repeats` runs the whole list that many times, `delay_secs`
        separates consecutive sends (default 0.4 s), `hold_secs` keeps a command "held" by resending
        it every HOLD_REPEAT seconds (the cadence of a held key) until that time has passed."""
        coord = self.coordinator
        device = kwargs.get(ATTR_DEVICE) or HUB_DEVICE
        repeats = max(int(kwargs.get(ATTR_NUM_REPEATS) or 1), 1)
        delay = float(kwargs.get(ATTR_DELAY_SECS, DEFAULT_DELAY_SECS) or 0)
        hold = float(kwargs.get(ATTR_HOLD_SECS) or 0)
        first = True
        for _ in range(repeats):
            for cmd in command:
                if not first and delay > 0:
                    await coord.async_sleep(delay)
                first = False
                await coord.async_device_command(device, cmd)
                held = 0.0
                while held + HOLD_REPEAT <= hold:
                    await coord.async_sleep(HOLD_REPEAT)
                    held += HOLD_REPEAT
                    await coord.async_device_command(device, cmd)
