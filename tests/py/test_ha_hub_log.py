"""The integration relays the hub's hub_log pushes: HA logger `custom_components.ha_harmony.hub_log` (INFO / WARNING, tagged
with the hub id) and a `hub_log` frame to the add-on. A scripted hub (no binary) and a fake add-on."""

import logging

import pytest
from conftest import HUB_ID
from test_ha_bluetooth import BtHub, _unload_before_the_servers_close, boot, until  # noqa: F401

import custom_components.ha_harmony  # noqa: F401  (before the HA plugin shadows it)

pytestmark = pytest.mark.usefixtures("enable_custom_integrations", "fast_client")


async def test_hub_lines_go_to_the_ha_logger_and_the_add_on(hass, fake_hub, fake_addon, caplog):
    bt = BtHub(bt=None)
    _, c, _ = await boot(hass, fake_hub, fake_addon, bt, yaml="devices: []\n")
    with caplog.at_level(logging.INFO, logger="custom_components.ha_harmony.hub_log"):
        await bt.push("hub_log", {"level": "info", "msg": "hub: build 7", "ts": 10})
        await bt.push("hub_log", {"level": "warning", "msg": "hub: RF send failed", "ts": 11, "dropped": 2})
        await until(hass, lambda: len(fake_addon.of("hub_log")) == 2)
    recs = [r for r in caplog.records if r.name == "custom_components.ha_harmony.hub_log"]
    assert [(r.levelno, r.getMessage()) for r in recs] == [
        (logging.INFO, f"[{HUB_ID}] hub: build 7"),
        (logging.WARNING, f"[{HUB_ID}] hub: RF send failed"),
    ]
    first, second = fake_addon.of("hub_log")
    assert (first["level"], first["msg"], first["ts"]) == ("info", "hub: build 7", 10)
    assert "dropped" not in first and second["dropped"] == 2 and second["level"] == "warning"
    assert c.available
