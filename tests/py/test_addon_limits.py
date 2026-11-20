"""The editor shows the caps of the hub that is linked: its integration reports them in its hello."""

import json

from addon_ws import connect, drain
from test_addon import H, clock

from harmony_addon.app import create_app
from harmony_addon.auth import NoAuth
from harmony_core.hub_limits import CURRENT

TOKEN = "ab" * 32


def _many_commands(n):
    return {
        "devices": [
            {"id": f"d{d}", "commands": {f"c{i}": {"proto": "nec", "addr": d, "cmd": i % 256} for i in range(n // 3)}}
            for d in range(3)
        ]
    }


async def hello(c, hub, limits):
    """The integration's hello with the hub's limits (a JSON text stands for a garbled report)."""
    ws = await connect(c, hub, limits=json.loads(limits) if isinstance(limits, str) and limits.startswith("{") else limits)
    await drain(ws)
    await ws.close()


def _app(tmp_path):
    return create_app(tmp_path, auth=NoAuth(), clock=clock, bundle_token=TOKEN, bundle_networks=None)


async def test_editor_shows_the_caps_of_the_hub_that_is_linked(aiohttp_client, tmp_path):
    c = await aiohttp_client(_app(tmp_path))
    cfg = _many_commands(900)

    async def prev(hub):
        r = await c.post(f"/api/v1/config/preview?hub={hub}", json={"config": cfg}, headers=H)
        return await r.json()

    tight = {**CURRENT.to_wire(), "budget": 128 * 1024}
    # a hub the add-on knows nothing about is checked against the newest firmware's limits
    assert (await prev("hub-a"))["ok"]
    # its integration says hello and reports a smaller budget
    await hello(c, "hub-a", tight)
    body = await prev("hub-a")
    assert not body["ok"] and "KiB of hub memory" in body["errors"][0]["msg"]
    await hello(c, "hub-a", CURRENT.to_wire())
    body = await prev("hub-a")
    assert body["ok"], body["errors"]
    u = body["usage"]
    assert u["commands"] == 900
    assert u["config_budget"] == CURRENT.budget and 0 < u["config_bytes"] < u["config_budget"]
    assert u["config_overhead"] == CURRENT.slack_b
    # another hub is unaffected; a hello without credible limits is ignored (the previous limits stay)
    await hello(c, "hub-b", tight)
    assert not (await prev("hub-b"))["ok"]
    await hello(c, "hub-b", "{}")
    assert not (await prev("hub-b"))["ok"]
    await hello(c, "hub-a", "not json")
    assert (await prev("hub-a"))["ok"]


async def test_reported_limits_survive_an_add_on_restart(aiohttp_client, tmp_path):
    c = await aiohttp_client(_app(tmp_path))
    await hello(c, "hub-a", CURRENT.to_wire())
    c2 = await aiohttp_client(_app(tmp_path))
    r = await c2.post("/api/v1/config/preview?hub=hub-a", json={"config": _many_commands(900)}, headers=H)
    assert (await r.json())["ok"]


async def test_apply_uses_the_same_limits(aiohttp_client, tmp_path):
    c = await aiohttp_client(_app(tmp_path))
    cfg = _many_commands(900)
    await c.put("/api/v1/config/draft?hub=hub-a", json={"config": cfg}, headers=H)
    await hello(c, "hub-a", {**CURRENT.to_wire(), "budget": 128 * 1024})
    r = await c.post("/api/v1/config/apply?hub=hub-a", json={}, headers=H)
    assert r.status == 422
    await hello(c, "hub-a", CURRENT.to_wire())
    r = await c.post("/api/v1/config/apply?hub=hub-a", json={}, headers=H)
    assert r.status == 200
