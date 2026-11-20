"""Linked hub ids (which ids have an integration channel) and adopting a `default` store."""

import json

import pytest
from addon_ws import BEARER, TOKEN, connect, drain, live, recv
from aiohttp import WSServerHandshakeError
from cfgapi import put_apply
from test_addon import CONFIG, H

from harmony_addon.app import create_app
from harmony_addon.auth import NoAuth
from harmony_addon.options import Options

NOW = [1000.0]


def make(tmp_path):
    return create_app(
        tmp_path,
        options=Options("info"),
        clock=lambda: NOW[0],
        auth=NoAuth(),
        bundle_token=TOKEN,
        bundle_networks=None,
        hello_timeout=0.3,
    )


@pytest.fixture
async def c(aiohttp_client, tmp_path):
    return await aiohttp_client(make(tmp_path))


async def apply(c, hub):
    r = await put_apply(c, CONFIG, hub)
    assert r.status == 200


async def hello(c, hub, **fields):
    """Connect and send hello; returns (socket, the frames the add-on answered with)."""
    ws = await connect(c, hub, **fields)
    return ws, await drain(ws)


async def poll(c, hub, **fields):
    """Connect, say hello, return the first answer, disconnect (the hub id stays linked)."""
    ws = await connect(c, hub, **fields)
    try:
        return await recv(ws)
    finally:
        await ws.close()


async def hubs(c):
    return await (await c.get("/api/v1/hubs")).json()


async def hub_status(c, hub):
    ws = await live(c, f"hub:{hub}")
    try:
        return (await recv(ws, "snapshot"))["topics"][f"hub:{hub}"]
    finally:
        await ws.close()


async def test_hello_records_the_hub_even_with_nothing_applied_and_status_reports_it(c):
    assert (await hubs(c))["linked"] == []
    ws, frames = await hello(c, "living-room-hub")
    assert frames == [{"t": "nothing_applied"}]
    body = await hubs(c)
    assert body["linked"] == [{"id": "living-room-hub", "last_seen": 1000}]
    st = await hub_status(c, "living-room-hub")
    assert st["connected"] is True and st["connected_at"] == 1000
    lv = await live(c, "hub:living-room-hub")
    await recv(lv, "snapshot")
    NOW[0] = 1005.0
    await ws.close()
    assert (await recv(lv, "hub"))["connected"] is False  # pushed when the socket closes
    await lv.close()
    assert (await hubs(c))["linked"] == [{"id": "living-room-hub", "last_seen": 1005}]
    assert (await hub_status(c, "other"))["connected"] is False
    assert (await hubs(c))["now"] == 1005


async def test_unauthenticated_or_invalid_ids_are_not_recorded(c):
    with pytest.raises(WSServerHandshakeError) as e:
        await c.ws_connect("/api/v1/channel?hub=evil", headers={})
    assert e.value.status == 401
    with pytest.raises(WSServerHandshakeError) as e:
        await c.ws_connect("/api/v1/channel?hub=../x", headers=BEARER)
    assert e.value.status == 400
    ws = await connect(c, "evil", hello=False)
    await ws.send_json({"t": "hello", "v": 1, "hub": "another"})  # names a different hub than the URL
    assert (await recv(ws))["code"] == "hub_mismatch"
    assert (await hubs(c))["linked"] == []


async def test_linked_hubs_survive_a_restart(aiohttp_client, tmp_path):
    c1 = await aiohttp_client(make(tmp_path))
    await poll(c1, "living-room-hub")
    saved = json.loads((tmp_path / "linked_hubs.json").read_text())["hubs"]
    assert saved == {"living-room-hub": int(NOW[0])}
    c2 = await aiohttp_client(make(tmp_path))
    assert [e["id"] for e in (await hubs(c2))["linked"]] == ["living-room-hub"]


async def test_damaged_linked_file_is_ignored(aiohttp_client, tmp_path):
    (tmp_path / "linked_hubs.json").write_text("{nope")
    c = await aiohttp_client(make(tmp_path))
    assert (await hubs(c))["linked"] == []


async def test_default_store_is_adopted_by_the_single_linked_hub(c, tmp_path):
    await apply(c, "default")
    before = (tmp_path / "hubs" / "default.json").read_bytes()
    first = await poll(c, "living-room-hub")
    assert first["t"] == "bundle"  # servable at once, no re-import
    assert first["body"]["hub_id"] == "living-room-hub"
    assert not (tmp_path / "hubs" / "default.json").exists()
    assert (tmp_path / "hubs" / "living-room-hub.json").read_bytes() == before
    assert (await hubs(c))["adopt"] is None
    assert (await poll(c, "living-room-hub"))["t"] == "bundle"  # idempotent
    assert [h["id"] for h in (await hubs(c))["hubs"]] == ["living-room-hub"]


async def test_adoption_happens_at_startup_for_a_known_linked_hub(aiohttp_client, tmp_path):
    c1 = await aiohttp_client(make(tmp_path))
    await poll(c1, "living-room-hub")
    await apply(c1, "default")  # created after the hub was already linked
    assert (tmp_path / "hubs" / "default.json").exists()
    c2 = await aiohttp_client(make(tmp_path))
    assert [h["id"] for h in (await hubs(c2))["hubs"]] == ["living-room-hub"]


async def test_nothing_to_adopt(c, tmp_path):
    await poll(c, "living-room-hub")
    assert (await hubs(c))["hubs"] == []
    assert not (tmp_path / "hubs").exists() or not list((tmp_path / "hubs").glob("*.json"))


async def test_hub_with_its_own_state_is_not_overwritten(c):
    await apply(c, "default")
    await apply(c, "living-room-hub")
    await poll(c, "living-room-hub")
    body = await hubs(c)
    assert [h["id"] for h in body["hubs"]] == ["default", "living-room-hub"]
    assert body["adopt"] is None  # the only target already has state


async def test_several_linked_hubs_offer_an_explicit_adoption(c):
    await apply(c, "default")
    await poll(c, "hub-a")  # single linked hub: adopted
    await apply(c, "default")  # a new default store appears
    await poll(c, "hub-b")  # now two linked hubs: no automatic move
    body = await hubs(c)
    assert body["adopt"] == {"from": "default", "to": ["hub-b"]}
    assert "default" in [h["id"] for h in body["hubs"]]
    r = await c.post("/api/v1/hubs/adopt", json={"to": "hub-b"}, headers=H)
    assert r.status == 200 and (await r.json()) == {"ok": True, "hub": "hub-b"}
    assert (await poll(c, "hub-b"))["t"] == "bundle"
    assert (await hubs(c))["adopt"] is None
    r = await c.post("/api/v1/hubs/adopt", json={"to": "hub-b"}, headers=H)
    assert r.status == 409  # it has a config now


async def test_explicit_adoption_errors(c):
    r = await c.post("/api/v1/hubs/adopt", json={"to": "nobody"}, headers=H)
    assert r.status == 409 and (await r.json())["error"]["code"] == "not_linked"
    await poll(c, "hub-a")
    r = await c.post("/api/v1/hubs/adopt", json={"to": "hub-a"}, headers=H)
    assert r.status == 404 and (await r.json())["error"]["code"] == "nothing_to_adopt"
    r = await c.post("/api/v1/hubs/adopt", json={"to": "hub-a", "x": 1}, headers=H)
    assert r.status == 400


async def test_an_explicit_adoption_pushes_the_bundle_to_the_connected_hub(c):
    await apply(c, "default")
    await poll(c, "hub-a")  # adopts the default store...
    await apply(c, "default")  # ...a second default store appears
    await poll(c, "hub-b")
    ws, frames = await hello(c, "hub-b")
    assert [f["t"] for f in frames] == ["nothing_applied"]
    r = await c.post("/api/v1/hubs/adopt", json={"to": "hub-b"}, headers=H)
    assert r.status == 200
    pushed = await recv(ws, "bundle")
    assert pushed["body"]["hub_id"] == "hub-b"
    await ws.close()


async def test_no_automatic_move_while_something_is_linked_as_default(c):
    await apply(c, "default")
    await poll(c, "default")
    await poll(c, "hub-a")
    assert [h["id"] for h in (await hubs(c))["hubs"]] == ["default"]
    assert (await hubs(c))["adopt"] == {"from": "default", "to": ["hub-a"]}


async def test_old_linked_file_version_is_ignored(aiohttp_client, tmp_path):
    (tmp_path / "linked_hubs.json").write_text('{"v": 1, "hubs": {"default": 5}}')
    c = await aiohttp_client(make(tmp_path))
    assert (await hubs(c))["linked"] == []


REPORT = [
    {"remote": 1, "caps": 7, "tier": "palette", "notices": ["theme 'dusk' replaced by its base preset 'midnight'"]},
    {"remote": 2, "caps": 15, "tier": "custom", "notices": []},
]


async def test_hello_and_identity_report_the_remotes_and_live_exposes_their_notices(c):
    ws, _ = await hello(c, "hub-a")
    st = await hub_status(c, "hub-a")
    assert st["remotes"] == [] and st["theme_notices"] == []
    lv = await live(c, "hub:hub-a")
    await recv(lv, "snapshot")
    await ws.send_json({"t": "identity", "remotes": REPORT})
    d = await recv(lv, "hub")  # pushed, not polled
    assert d["remotes"] == REPORT
    assert d["theme_notices"] == ["theme 'dusk' replaced by its base preset 'midnight'"]
    other = await hub_status(c, "hub-b")
    assert other["remotes"] == [] and other["theme_notices"] == []  # per hub
    # garbage and oversized reports are ignored and keep the last good one
    for bad in ("not a list", {"a": 1}, [{"remote": "1", "caps": 7, "tier": "x"}], [1] * 3000):
        await ws.send_json({"t": "identity", "remotes": bad})
    await drain(ws)
    assert (await hub_status(c, "hub-a"))["remotes"] == REPORT
    for bad_row in ({"remote": 256, "caps": 7}, {"remote": 1, "caps": -1}, {"remote": -1, "caps": 7}, {"remote": 1, "caps": 300}):
        await ws.send_json({"t": "identity", "remotes": [{"tier": "palette", "notices": [], **bad_row}]})
    await drain(ws)
    assert (await hub_status(c, "hub-a"))["remotes"] == REPORT  # out-of-range ids and caps are refused
    await ws.send_json({"t": "identity", "remotes": []})
    await drain(ws)
    st = await hub_status(c, "hub-a")
    assert st["remotes"] == [] and st["theme_notices"] == []  # an empty report clears it
    await ws.send_json({"t": "identity", "remotes": [{"remote": 1, "caps": 15, "tier": "custom", "notices": []}]})
    await drain(ws)
    st = await hub_status(c, "hub-a")
    assert st["theme_notices"] == [] and st["remotes"][0]["tier"] == "custom"
    await ws.close()
    await lv.close()
    ws2, _ = await hello(c, "hub-c", remotes=REPORT)  # hello carries them too
    assert (await hub_status(c, "hub-c"))["remotes"] == REPORT
    await ws2.close()
