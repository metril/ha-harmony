"""One button per configured device command (plus power_on/power_off)."""

from __future__ import annotations

from homeassistant.components.button import ButtonEntity
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from . import HarmonyConfigEntry
from .base_entity import HarmonyEntity
from .coordinator import HarmonyCoordinator
from .entity_sync import setup_dynamic


async def async_setup_entry(
    hass: HomeAssistant,
    entry: HarmonyConfigEntry,
    async_add_entities: AddConfigEntryEntitiesCallback,
) -> None:
    setup_dynamic(
        entry,
        async_add_entities,
        lambda c: [
            HarmonyCommandButton(c, dev, cmd)
            for dev in c.compiled.devices
            for cmd in c.device_commands(dev)
        ],
    )


class HarmonyCommandButton(HarmonyEntity, ButtonEntity):
    def __init__(self, coordinator: HarmonyCoordinator, device: str, command: str) -> None:
        super().__init__(
            coordinator, f"cmd_{device}:{command}", device=device
        )  # ids never contain ":"
        self._device, self._command = device, command
        self._attr_name = command.replace("_", " ").capitalize()

    async def async_press(self) -> None:
        await self.coordinator.async_device_command(self._device, self._command)
