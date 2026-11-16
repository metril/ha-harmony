"""Add entities for new YAML devices/commands after a config reload (removal is registry-based)."""

from __future__ import annotations

from collections.abc import Callable, Iterable

from homeassistant.helpers.entity import Entity
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from . import HarmonyConfigEntry
from .coordinator import HarmonyCoordinator


def setup_dynamic(
    entry: HarmonyConfigEntry,
    add: AddConfigEntryEntitiesCallback,
    build: Callable[[HarmonyCoordinator], Iterable[Entity]],
) -> None:
    """Add `build(coordinator)` now and again after each reload, skipping known unique ids."""
    coordinator = entry.runtime_data
    known: set[str] = set()

    def refresh() -> None:
        new = [e for e in build(coordinator) if e.unique_id not in known]
        known.update(e.unique_id for e in new if e.unique_id)
        if new:
            add(new)
        # a deleted-then-re-added entity must be creatable again
        known.intersection_update(coordinator.expected_unique_ids())

    refresh()
    entry.async_on_unload(coordinator.async_add_config_listener(refresh))
