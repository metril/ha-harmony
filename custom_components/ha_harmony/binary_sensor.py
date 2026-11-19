"""Tracked power state of each configured device (from the hub's device_state pushes)."""

from __future__ import annotations

from homeassistant.components.binary_sensor import BinarySensorDeviceClass, BinarySensorEntity
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from . import HarmonyConfigEntry
from .base_entity import HarmonyEntity
from .coordinator import TOPIC_BT, HarmonyCoordinator, topic_device
from .entity_sync import setup_dynamic
from .harmony_core.bt_relay import addr_key

PARALLEL_UPDATES = 0  # push-based: commands go through the hub client, which pipelines them


async def async_setup_entry(
    hass: HomeAssistant,
    entry: HarmonyConfigEntry,
    async_add_entities: AddConfigEntryEntitiesCallback,
) -> None:
    def build(c: HarmonyCoordinator) -> list[HarmonyEntity]:
        ents: list[HarmonyEntity] = [HarmonyDevicePower(c, d) for d in c.compiled.devices]
        if c.btm.supported:  # a hub without Bluetooth has none, whatever the config lists
            ents += [HarmonyBtPeer(c, kind, addr, name) for kind, addr, name in c.btm.peers()]
        return ents

    setup_dynamic(entry, async_add_entities, build)


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


class HarmonyBtPeer(HarmonyEntity, BinarySensorEntity):
    """Whether a configured Bluetooth device (a host the hub types to) or remote (input) is connected to the hub.

    Keyed by address, never by slot: slots are config positions and move when the config is edited. A peer the config
    lists but the hub has not bonded is simply off; bonds the config does not list get no entity."""

    _attr_device_class = BinarySensorDeviceClass.CONNECTIVITY

    def __init__(self, coordinator: HarmonyCoordinator, kind: str, addr: str, name: str | None) -> None:
        super().__init__(coordinator, f"btconn_{kind}_{addr_key(addr)}", topics=(TOPIC_BT,))
        self._kind, self._addr = kind, addr
        self._attr_name = name or addr

    @property
    def is_on(self) -> bool | None:
        if self.coordinator.btm.state is None:
            return None  # nothing reported yet
        e = self.coordinator.btm.entry(self._kind, self._addr)
        return bool(e and e["connected"])

    @property
    def extra_state_attributes(self) -> dict[str, object]:
        e = self.coordinator.btm.entry(self._kind, self._addr)
        attrs: dict[str, object] = {"address": self._addr, "kind": self._kind, "bonded": e is not None}
        if e is not None:
            attrs["slot"] = e["slot"]
            if self._kind == "host":
                attrs["transport"] = e.get("transport")
            elif "battery" in e:
                attrs["battery"] = e["battery"]  # percent, only while the remote has reported one
        return attrs
