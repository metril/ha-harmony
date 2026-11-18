"""Shared entity base: stable ids from hub_id, availability from the connection."""

from __future__ import annotations

from collections.abc import Iterable

from homeassistant.core import callback
from homeassistant.helpers import device_registry as dr
from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.entity import Entity

from .const import DOMAIN
from .coordinator import TOPIC_AVAILABLE, HarmonyCoordinator


def hub_device_info(coordinator: HarmonyCoordinator) -> DeviceInfo:
    return DeviceInfo(
        identifiers={(DOMAIN, coordinator.hub_id)},
        name=f"Harmony hub {coordinator.hub_id}",
        manufacturer="Custom firmware",
        sw_version=coordinator.fw_version,
    )


class HarmonyEntity(Entity):
    """`device` puts the entity on a YAML device's sub-device, `remote` on a remote's.

    Entities subscribe to the coordinator topics they show (plus availability) and are written
    only when one of them fires, never on "anything changed"."""

    _attr_has_entity_name = True
    _attr_should_poll = False

    def __init__(
        self,
        coordinator: HarmonyCoordinator,
        key: str,
        *,
        device: str | None = None,
        remote: int | None = None,
        topics: Iterable[str] = (),
        track_available: bool = True,
    ) -> None:
        self.coordinator = coordinator
        self._topics = (*((TOPIC_AVAILABLE,) if track_available else ()), *topics)
        hub = coordinator.hub_id
        hub_dev = dr.async_get(coordinator.hass).async_get_device_by_identifier(
            (DOMAIN, hub), coordinator.config_entry.entry_id
        )
        via = {"via_device_id": hub_dev.id} if hub_dev else {}
        self._attr_unique_id = f"{hub}_{key}"
        if device is not None:
            self._attr_device_info = DeviceInfo(
                identifiers={(DOMAIN, f"{hub}_dev_{device}")},
                name=coordinator.compiled.devices[device].name,
                **via,
            )
        elif remote is not None:
            self._attr_device_info = DeviceInfo(
                identifiers={(DOMAIN, f"{hub}_remote_{remote}")},
                name=f"Remote {remote}",
                **via,
            )
        else:
            self._attr_device_info = hub_device_info(coordinator)

    async def async_added_to_hass(self) -> None:
        await super().async_added_to_hass()
        self.async_on_remove(
            self.coordinator.async_subscribe_topics(self._topics, self._coordinator_notified)
        )

    @callback
    def _coordinator_notified(self) -> None:
        self.async_write_ha_state()

    @property
    def available(self) -> bool:
        return self.coordinator.available
