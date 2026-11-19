"""Diagnostics: the hub link, the compiled config's sizes, per-remote page state and timings.

Tokens and addresses are redacted; the YAML source itself is never included.
"""

from __future__ import annotations

from typing import Any

from homeassistant.components.diagnostics import async_redact_data
from homeassistant.const import CONF_HOST
from homeassistant.core import HomeAssistant

from . import HarmonyConfigEntry
from .const import (
    CONF_ADDON_HOST,
    CONF_ADDON_TOKEN,
    CONF_ADDON_UUID,
    CONF_TOKEN,
)

TO_REDACT = {CONF_TOKEN, CONF_ADDON_TOKEN, CONF_ADDON_UUID, CONF_ADDON_HOST, CONF_HOST}
TO_REDACT_BT = {"addr", "name"}  # Bluetooth addresses and device names (bonds, the last pairing, scan candidates)


async def async_get_config_entry_diagnostics(
    hass: HomeAssistant, entry: HarmonyConfigEntry
) -> dict[str, Any]:
    c = entry.runtime_data
    comp = c.compiled
    remotes: dict[str, Any] = {}
    for r, link in c.links.items():
        info = link.info
        remotes[str(r)] = {
            "page_format": link.page_fmt,
            "announced": None
            if info is None
            else {
                "page_fmt": info.page_fmt,
                "caps": info.caps,
                "icons": info.icons,
                "stored_gen": info.gen,
                "stored_pages": info.pages,
            },
            "pushed_as": link.pushed,  # (config gen, format, link epoch, theme caps) of the last complete set
            "theme_notices": [n["msg"] for n in c.theme_notices(r)],
            "theme": c.theme_status(r),  # name, id, sent (the hub took it), acked (the remote holds the set)
            "pending": link.pending,
            "attached": link.attached,
            "last_push_ms": link.push_ms,
        }
    return {
        "entry": {
            "data": async_redact_data(dict(entry.data), TO_REDACT),
            "options": dict(entry.options),
        },
        "hub": {
            "id": c.hub_id,
            "fw_version": c.fw_version,
            "connected": c.available,
            "config_hash": c.client.config_hash,
            "compiled_hash": comp.config_hash,
            "config_accepted": c.hub_accepted,
            "config_state": c.config_state,
            "pending_hash": c.pending.config_hash,
            "activities": list(c.activities),
            "current_activity": c.current_activity,
        },
        "addon_link": {
            "linked": c.has_addon_link,
            "state": c.link_state,
            "rev": c.rev,
            "applied_at": c.applied_at,
            "last_error": c.last_error,
        },
        "config": {
            "managed": c.managed,
            "devices": len(comp.devices),
            "ha_devices": len(comp.ha_devices),
            "activities": len(comp.activities),
            "ha_activities": len(comp.ha_activities),
            "key_layers": len(comp.ha_keys),
            "tiles": len(comp.tiles),
            "pages_v1": len(comp.pages),
            "pages_v2": len(comp.pages_v2),
            "page_gen": comp.gen,
            "theme": comp.theme_name,
            "theme_id": comp.theme_id,
            "warnings": [w["msg"] for w in comp.warnings],
            "page_bytes_v2": sum(len(p) for p in comp.pages_v2),
        },
        "remotes": remotes,
        "bluetooth": {
            "supported": c.btm.supported,
            "hello_bt": c.client.bt,  # hello_ack.bt: limits only, no addresses
            "state": async_redact_data(c.btm.state, TO_REDACT_BT) if c.btm.state else None,
            "gen": c.btm.gen,
            "listening": c.btm.listening(),
            "relay_exchanges": c.btm.exchanges,
            "events_dropped": c.btm.events_dropped,
            "events_rate_dropped": c.btm.events_rate_dropped,
        },
        "counters": {
            "config_pushes": c.config_pushes,
            "page_pushes": c.page_pushes,
            "theme_pushes": c.theme_pushes,
            "tile_updates": c.tile_updates,
        },
        "timings_ms": dict(c.timings),
    }
