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

from . import HarmonyConfigEntry, const
from .base_entity import HarmonyEntity
from .coordinator import (
    LINK_STATES,
    TOPIC_CONFIG,
    TOPIC_LINK,
    HarmonyCoordinator,
    topic_battery,
    topic_device,
    topic_remote,
)
from .entity_sync import setup_dynamic

PARALLEL_UPDATES = 0  # push-based: commands go through the hub client, which pipelines them


async def async_setup_entry(
    hass: HomeAssistant,
    entry: HarmonyConfigEntry,
    async_add_entities: AddConfigEntryEntitiesCallback,
) -> None:
    def build(c: HarmonyCoordinator) -> list[HarmonyEntity]:
        ents: list[HarmonyEntity] = [HarmonyDeviceInput(c, d) for d in c.compiled.devices]
        for r in c.remotes:
            ents += [HarmonyRemoteBattery(c, r), HarmonyRemoteVoltage(c, r), HarmonyPageFormat(c, r)]
        if c.has_addon_link:
            ents.append(HarmonyAddonLink(c))
        return ents

    setup_dynamic(entry, async_add_entities, build)


class HarmonyDeviceInput(HarmonyEntity, SensorEntity):
    _attr_device_class = SensorDeviceClass.ENUM
    _attr_translation_key = "device_input"

    def __init__(self, coordinator: HarmonyCoordinator, device: str) -> None:
        super().__init__(
            coordinator,
            f"dev_{device}_input",
            device=device,
            topics=(topic_device(device), TOPIC_CONFIG),
        )
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
        super().__init__(
            coordinator, f"battery_{remote}", remote=remote, topics=(topic_battery(remote),)
        )
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
        super().__init__(
            coordinator, f"voltage_{remote}", remote=remote, topics=(topic_battery(remote),)
        )
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
        super().__init__(coordinator, "addon_link", topics=(TOPIC_LINK,), track_available=False)
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
        return {
            "rev": c.rev,
            "applied_at": c.applied_at,
            "last_error": c.last_error,
            "config_state": c.config_state,  # accepted | pending | rejected (by the hub)
        }


class HarmonyPageFormat(HarmonyEntity, SensorEntity):
    """Which page format a remote speaks (from its `remote_info`) and what it can do."""

    _attr_device_class = SensorDeviceClass.ENUM
    _attr_entity_category = EntityCategory.DIAGNOSTIC
    _attr_translation_key = "page_format"

    def __init__(self, coordinator: HarmonyCoordinator, remote: int) -> None:
        super().__init__(
            coordinator, f"pagefmt_{remote}", remote=remote, topics=(topic_remote(remote),)
        )
        self._attr_options = ["unknown", "v1", "v2"]
        self._remote = remote

    @property
    def native_value(self) -> str:
        link = self.coordinator.links.get(self._remote)
        return "unknown" if link is None or link.info is None else f"v{link.page_fmt}"

    @property
    def extra_state_attributes(self) -> dict[str, object]:
        link = self.coordinator.links.get(self._remote)
        info = link.info if link else None
        if info is None:
            return {"pages_sent_as": "v1", "caps": [], "icons": None}
        names = {const.CAP_TILE_STATE: "tile_state", const.CAP_PAGING: "paging"}
        return {
            "pages_sent_as": f"v{link.page_fmt}",
            "page_fmt": info.page_fmt,
            "caps": [n for bit, n in names.items() if info.caps & bit],
            "icons": info.icons,
        }
