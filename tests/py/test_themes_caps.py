"""The coordinator builds each remote's pages for its theme caps and re-pushes when they change."""

import pytest
from test_ha_runtime import V2_INFO, YAML, clock, rig, settle  # noqa: F401  (fixtures)

pytestmark = pytest.mark.usefixtures("enable_custom_integrations", "fast_client")

PALETTE = {
    "red",
    "orange",
    "amber",
    "yellow",
    "lime",
    "green",
    "teal",
    "cyan",
    "blue",
    "indigo",
    "purple",
    "pink",
}
COLOUR_YAML = (
    YAML.replace("entity: light.a}", "entity: light.a, color: red, on_color: '#3355FF'}")
    + "theme: midnight\n"
)


async def _pushed_set(hass, hub, c, info):
    before = len(hub.pages())
    await hub.push("remote_info", info)
    n = len(c.compiled.pages_v2)
    await settle(hass, lambda: len(hub.pages()) - before >= n)
    await settle(hass, lambda: all(lk.task is None or lk.task.done() for lk in c.links.values()))
    return hub.pages()[before:]


async def test_caps_change_repushes_with_degraded_colours(hass, rig, clock, monkeypatch):  # noqa: F811
    hub, c = await rig(COLOUR_YAML)
    await hub.push("remote_attached", {})
    await settle(hass, clock.active)
    low = await _pushed_set(hass, hub, c, V2_INFO)  # caps 3: no colour support
    lamp = low[0].widgets[0]
    assert (lamp.color, lamp.on_color, low[0].theme) == (None, None, 0)
    assert "theme 'midnight' not shown" in " ".join(n["msg"] for n in c.theme_notices(1))
    sent = len(hub.pages())
    mid = await _pushed_set(hass, hub, c, {**V2_INFO, "caps": 7})  # the firmware was updated
    lamp = mid[0].widgets[0]
    assert lamp.color == "red" and lamp.on_color in PALETTE and mid[0].theme == 1
    assert len(hub.pages()) == sent + len(mid)
    assert any("custom colours snapped" in n["msg"] for n in c.theme_notices(1))
    # with bit 3 the free colour survives
    full = await _pushed_set(hass, hub, c, {**V2_INFO, "caps": 15, "gen": 1})
    assert full[0].widgets[0].on_color.startswith("#") and c.theme_notices(1) == []
