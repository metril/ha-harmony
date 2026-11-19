"""The editor shows the caps of the hub that is linked: its integration reports them on every poll."""

import json

from test_addon import H, clock

from harmony_addon.app import create_app
from harmony_addon.auth import NoAuth
from harmony_core.hub_limits import CURRENT, LIMITS_HEADER

TOKEN = "ab" * 32
BEARER = {"Authorization": f"Bearer {TOKEN}"}


def _many_commands(n):
    return {
        "devices": [
            {"id": f"d{d}", "commands": {f"c{i}": {"proto": "nec", "addr": d, "cmd": i % 256} for i in range(n // 3)}}
            for d in range(3)
        ]
    }


def _app(tmp_path):
    return create_app(tmp_path, auth=NoAuth(), clock=clock, bundle_token=TOKEN, bundle_networks=None)


async def test_editor_shows_the_caps_of_the_hub_that_is_linked(aiohttp_client, tmp_path):
    c = await aiohttp_client(_app(tmp_path))
    cfg = _many_commands(900)

    async def prev(hub):
        r = await c.post(f"/api/v1/config/preview?hub={hub}", json={"config": cfg}, headers=H)
        return await r.json()

    # a hub the add-on knows nothing about: the fixed limits of old firmware
    body = await prev("hub-a")
    assert not body["ok"] and "too many commands: 900 (the hub allows at most 256)" in body["errors"][0]["msg"]
    # its integration polls and reports the hub's limits (a current hub): one memory budget instead
    await c.get("/api/v1/bundle?hub=hub-a", headers={**BEARER, LIMITS_HEADER: json.dumps(CURRENT.to_wire())})
    body = await prev("hub-a")
    assert body["ok"], body["errors"]
    u = body["usage"]
    assert u["commands"] == 900 and u["commands_max"] is None and u["json_max"] is None
    assert u["config_budget"] == CURRENT.budget and 0 < u["config_bytes"] < u["config_budget"]
    # another hub still gets the old numbers; a hub that reports old firmware ({}) too
    assert not (await prev("hub-b"))["ok"]
    await c.get("/api/v1/bundle?hub=hub-b", headers={**BEARER, LIMITS_HEADER: "{}"})
    assert not (await prev("hub-b"))["ok"]
    # junk in the header is ignored (the previous limits stay)
    await c.get("/api/v1/bundle?hub=hub-a", headers={**BEARER, LIMITS_HEADER: "not json"})
    assert (await prev("hub-a"))["ok"]


async def test_reported_limits_survive_an_add_on_restart(aiohttp_client, tmp_path):
    c = await aiohttp_client(_app(tmp_path))
    await c.get("/api/v1/bundle?hub=hub-a", headers={**BEARER, LIMITS_HEADER: json.dumps(CURRENT.to_wire())})
    c2 = await aiohttp_client(_app(tmp_path))
    r = await c2.post("/api/v1/config/preview?hub=hub-a", json={"config": _many_commands(900)}, headers=H)
    assert (await r.json())["ok"]


async def test_apply_uses_the_same_limits(aiohttp_client, tmp_path):
    c = await aiohttp_client(_app(tmp_path))
    cfg = _many_commands(900)
    await c.put("/api/v1/config/draft?hub=hub-a", json={"config": cfg}, headers=H)
    r = await c.post("/api/v1/config/apply?hub=hub-a", json={}, headers=H)
    assert r.status == 422  # old firmware assumed
    await c.get("/api/v1/bundle?hub=hub-a", headers={**BEARER, LIMITS_HEADER: json.dumps(CURRENT.to_wire())})
    r = await c.post("/api/v1/config/apply?hub=hub-a", json={}, headers=H)
    assert r.status == 200
