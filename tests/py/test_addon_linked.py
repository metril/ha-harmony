"""Linked hub ids (which ids integrations poll for) and adopting a `default` store."""

import json

import pytest
from test_addon import CONFIG, H

from harmony_addon.app import create_app
from harmony_addon.auth import NoAuth
from harmony_addon.options import Options

TOKEN = "ab" * 32
BEARER = {"Authorization": f"Bearer {TOKEN}"}
NOW = [1000.0]


def make(tmp_path):
    return create_app(
        tmp_path,
        options=Options("info"),
        clock=lambda: NOW[0],
        auth=NoAuth(),
        bundle_token=TOKEN,
        bundle_networks=None,
        wait_seconds=0.2,
    )


@pytest.fixture
async def c(aiohttp_client, tmp_path):
    return await aiohttp_client(make(tmp_path))


async def apply(c, hub):
    q = f"?hub={hub}"
    r = await c.put(f"/api/v1/config/draft{q}", json={"config": CONFIG}, headers=H)
    assert r.status == 200
    r = await c.post(f"/api/v1/config/apply{q}", json={}, headers=H)
    assert r.status == 200


async def poll(c, hub, path="bundle"):
    return await c.get(f"/api/v1/{path}?hub={hub}", headers=BEARER)


async def hubs(c):
    return await (await c.get("/api/v1/hubs")).json()


async def test_poll_records_hub_even_on_404_and_status_reports_it(c):
    assert (await hubs(c))["linked"] == []
    assert (await poll(c, "living-room-hub")).status == 404
    NOW[0] = 1005.0
    assert (await poll(c, "living-room-hub", "bundle/wait")).status == 204
    body = await hubs(c)
    assert body["linked"] == [{"id": "living-room-hub", "last_seen": 1005}]
    assert body["now"] == 1005
    st = await (await c.get("/api/v1/config/status?hub=living-room-hub")).json()
    assert st["hub_last_seen"] == 1005 and st["linked_hubs"] == body["linked"] and st["now"] == 1005
    st = await (await c.get("/api/v1/config/status?hub=other")).json()
    assert st["hub_last_seen"] is None


async def test_unauthenticated_or_invalid_ids_are_not_recorded(c):
    assert (await c.get("/api/v1/bundle?hub=evil", headers={})).status == 401
    assert (await c.get("/api/v1/bundle?hub=../x", headers=BEARER)).status == 400
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
    r = await poll(c, "living-room-hub")
    assert r.status == 200  # servable at once, no re-import
    assert (await r.json())["hub_id"] == "living-room-hub"
    assert not (tmp_path / "hubs" / "default.json").exists()
    assert (tmp_path / "hubs" / "living-room-hub.json").read_bytes() == before
    assert (await hubs(c))["adopt"] is None
    # idempotent: polling again changes nothing
    assert (await poll(c, "living-room-hub")).status == 200
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
    assert (await poll(c, "hub-b")).status == 200
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


async def test_adopted_waiter_gets_the_bundle(c):
    await apply(c, "default")
    r = await poll(c, "living-room-hub", "bundle/wait")
    assert r.status == 200 and (await r.json())["hub_id"] == "living-room-hub"


async def test_no_automatic_move_while_something_polls_for_default(c):
    await apply(c, "default")
    await poll(c, "default")
    await poll(c, "hub-a")
    assert [h["id"] for h in (await hubs(c))["hubs"]] == ["default"]
    assert (await hubs(c))["adopt"] == {"from": "default", "to": ["hub-a"]}


async def test_ack_and_other_requests_do_not_record_a_hub(c):
    await apply(c, "living-room-hub")
    r = await poll(c, "living-room-hub")
    rev = (await r.json())["rev"]
    body = {"hub": "living-room-hub", "rev": rev, "result": "ok"}
    assert (await c.post("/api/v1/bundle/ack", json=body, headers=BEARER)).status == 200
    assert (await c.get("/api/v1/bundle", headers=BEARER)).status == 404  # no hub param: not a poll
    assert [e["id"] for e in (await hubs(c))["linked"]] == ["living-room-hub"]


async def test_old_linked_file_version_is_ignored(aiohttp_client, tmp_path):
    (tmp_path / "linked_hubs.json").write_text('{"v": 1, "hubs": {"default": 5}}')
    c = await aiohttp_client(make(tmp_path))
    assert (await hubs(c))["linked"] == []


async def test_poll_reports_the_remotes_and_status_exposes_their_notices(c):
    report = [
        {"remote": 1, "caps": 7, "tier": "palette", "notices": ["theme 'dusk' replaced by its base preset 'midnight'"]},
        {"remote": 2, "caps": 15, "tier": "custom", "notices": []},
    ]
    assert (await poll(c, "hub-a")).status == 404
    st = await (await c.get("/api/v1/config/status?hub=hub-a")).json()
    assert st["remotes"] == [] and st["theme_notices"] == []
    hdr = {**BEARER, "X-Hub-Remotes": json.dumps(report)}
    assert (await c.get("/api/v1/bundle?hub=hub-a", headers=hdr)).status == 404
    st = await (await c.get("/api/v1/config/status?hub=hub-a")).json()
    assert st["remotes"] == [{**r} for r in report]
    assert st["theme_notices"] == ["theme 'dusk' replaced by its base preset 'midnight'"]
    other = await (await c.get("/api/v1/config/status?hub=hub-b")).json()
    assert other["remotes"] == [] and other["theme_notices"] == []  # per hub
    # garbage and oversized reports are ignored and keep the last good one
    for bad in ("not json", json.dumps({"a": 1}), json.dumps([{"remote": "1", "caps": 7, "tier": "x"}]), "[" + "1," * 3000 + "1]"):
        await c.get("/api/v1/bundle?hub=hub-a", headers={**BEARER, "X-Hub-Remotes": bad})
    st = await (await c.get("/api/v1/config/status?hub=hub-a")).json()
    assert st["remotes"] == report and len(st["theme_notices"]) == 1
    # a later poll with nothing wrong replaces it (the remote was updated: no notices left)
    hdr = {**BEARER, "X-Hub-Remotes": json.dumps([{"remote": 1, "caps": 15, "tier": "custom", "notices": []}])}
    await c.get("/api/v1/bundle?hub=hub-a", headers=hdr)
    st = await (await c.get("/api/v1/config/status?hub=hub-a")).json()
    assert st["theme_notices"] == [] and st["remotes"][0]["tier"] == "custom"
