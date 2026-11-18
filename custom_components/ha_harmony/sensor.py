"""Device input sensors and remote battery/voltage sensors."""

from __future__ import annotations

from homeassistant.components.sensor import (
    SensorDeviceClass,
    SensorEntity,
    SensorStateClass,
)
from homeassistant.const import PERCENTAGE, EntityCategory, UnitOfElectricPotential
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from . import HarmonyConfigEntry
from .base_entity import HarmonyEntity
from .coordinator import LINK_STATES, HarmonyCoordinator
from .entity_sync import setup_dynamic


async def async_setup_entry(
    hass: HomeAssistant,
    entry: HarmonyConfigEntry,
    async_add_entities: AddConfigEntryEntitiesCallback,
) -> None:
    def build(c: HarmonyCoordinator) -> list[HarmonyEntity]:
        ents: list[HarmonyEntity] = [HarmonyDeviceInput(c, d) for d in c.compiled.devices]
        for r in c.remotes:
            ents += [HarmonyRemoteBattery(c, r), HarmonyRemoteVoltage(c, r)]
        if c.has_addon_link:
            ents.append(HarmonyAddonLink(c))
        return ents

    setup_dynamic(entry, async_add_entities, build)


class HarmonyDeviceInput(HarmonyEntity, SensorEntity):
    _attr_device_class = SensorDeviceClass.ENUM
    _attr_name = "Input"

    def __init__(self, coordinator: HarmonyCoordinator, device: str) -> None:
        super().__init__(coordinator, f"dev_{device}_input", device=device)
        self._device = device

    @property
    def options(self) -> list[str]:  # read live: a reload can add inputs to this device
        dev = self.coordinator.compiled.devices.get(self._device)
        return list(dev.inputs) if dev else []

    @property
    def native_value(self) -> str | None:
        st = self.coordinator.device_states.get(self._device)
        return st.input if st and st.input in self.options else None


class HarmonyRemoteBattery(HarmonyEntity, SensorEntity):
    _attr_device_class = SensorDeviceClass.BATTERY
    _attr_state_class = SensorStateClass.MEASUREMENT
    _attr_native_unit_of_measurement = PERCENTAGE

    def __init__(self, coordinator: HarmonyCoordinator, remote: int) -> None:
        super().__init__(coordinator, f"battery_{remote}", remote=remote)
        self._remote = remote

    @property
    def native_value(self) -> int | None:
        b = self.coordinator.battery.get(self._remote)
        return b[0] if b else None


class HarmonyRemoteVoltage(HarmonyEntity, SensorEntity):
    _attr_device_class = SensorDeviceClass.VOLTAGE
    _attr_state_class = SensorStateClass.MEASUREMENT
    _attr_entity_category = EntityCategory.DIAGNOSTIC
    _attr_native_unit_of_measurement = UnitOfElectricPotential.MILLIVOLT

    def __init__(self, coordinator: HarmonyCoordinator, remote: int) -> None:
        super().__init__(coordinator, f"voltage_{remote}", remote=remote)
        self._remote = remote

    @property
    def native_value(self) -> int | None:
        b = self.coordinator.battery.get(self._remote)
        return b[1] if b else None


class HarmonyAddonLink(HarmonyEntity, SensorEntity):
    """Where the applied config comes from and whether the add-on is answering."""

    _attr_device_class = SensorDeviceClass.ENUM
    _attr_entity_category = EntityCategory.DIAGNOSTIC
    _attr_translation_key = "addon_link"

    def __init__(self, coordinator: HarmonyCoordinator) -> None:
        super().__init__(coordinator, "addon_link")
        self._attr_options = list(LINK_STATES)

    @property
    def available(self) -> bool:  # about the add-on, not the hub connection
        return True

    @property
    def native_value(self) -> str:
        return self.coordinator.link_state

    @property
    def extra_state_attributes(self) -> dict[str, object]:
        c = self.coordinator
        return {"rev": c.rev, "applied_at": c.applied_at, "last_error": c.last_error}
