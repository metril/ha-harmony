"""Editor API completion: draft revisions, all validation errors, hub list, apply status."""

import json

import pytest
from addon_ws import connect, drain, get_bundle, live, recv
from test_addon import CONFIG, SMALL, H, clock, put_draft

from harmony_addon.app import create_app
from harmony_addon.auth import NoAuth
from harmony_addon.options import Options
from harmony_core.config_model import ConfigError, parse_model, validate_all


@pytest.fixture
async def client(aiohttp_client, tmp_path):
    return await aiohttp_client(
        create_app(tmp_path, options=Options("info"), clock=clock, auth=NoAuth())
    )


# ---------------------------------------------------------------- draft revisions
async def test_draft_rev_conflict_and_unconditional_writes(client):
    got = await (await client.get("/api/v1/config")).json()
    assert got["rev"] == 0
    r = await client.put(
        "/api/v1/config/draft", json={"config": CONFIG}, headers={**H, "If-Match": "0"}
    )
    assert r.status == 200 and (await r.json())["rev"] == 1
    r = await client.put(
        "/api/v1/config/draft", json={"config": {}}, headers={**H, "If-Match": '"0"'}
    )
    body = await r.json()
    assert r.status == 409 and body["error"]["code"] == "conflict" and body["error"]["rev"] == 1
    assert (await (await client.get("/api/v1/config")).json())["draft"]["config"] == CONFIG
    r = await client.put(
        "/api/v1/config/draft", json={"config": CONFIG}, headers={**H, "If-Match": 'W/"1"'}
    )
    assert r.status == 200 and (await r.json())["rev"] == 2
    assert (await put_draft(client)).status == 200  # no header = unconditional
    assert (await (await client.get("/api/v1/config")).json())["rev"] == 3


@pytest.mark.parametrize("val", ["abc", "-1", "1 2", "9" * 20, '"'])
async def test_if_match_garbage_is_400(client, val):
    r = await client.put(
        "/api/v1/config/draft", json={"config": CONFIG}, headers={**H, "If-Match": val}
    )
    assert r.status == 400


async def test_import_revert_and_apply_bump_the_rev(client):
    hy = {**H, "Content-Type": "text/yaml"}
    r = await client.post("/api/v1/config/import_yaml", data=SMALL, headers=hy)
    assert (await r.json())["rev"] == 1
    r = await client.post("/api/v1/config/import_yaml", data=SMALL, headers={**hy, "If-Match": "0"})
    assert r.status == 409 and (await r.json())["error"]["rev"] == 1
    r = await client.post("/api/v1/config/revert", json={"target": "applied"}, headers=H)
    assert (await r.json())["rev"] == 2
    await put_draft(client)
    r = await client.post("/api/v1/config/apply", json={}, headers=H)
    assert (await r.json())["rev"] == 4
    r = await client.put(
        "/api/v1/config/draft", json={"config": CONFIG}, headers={**H, "If-Match": "3"}
    )
    assert r.status == 409  # a stale writer cannot resurrect a draft over the applied state


async def test_state_files_from_before_revisions_still_load(client, tmp_path):
    await put_draft(client)
    f = tmp_path / "hubs" / "default.json"
    d = json.loads(f.read_text())
    del d["draft_rev"]
    f.write_text(json.dumps(d))
    assert (await (await client.get("/api/v1/config")).json())["rev"] == 0


# ---------------------------------------------------------------- all validation errors
BAD = {
    "devices": [
        {"id": "BAD ID"},
        {"id": "tv", "power": {"on": {"proto": "nec", "addr": 1, "cmd": 1}}},
        {"id": "ok", "commands": {"x": {"proto": "nec", "addr": 999, "cmd": 1}}},
    ],
    "activities": [{"id": "a", "name": "A", "devices": [{"device": "tv"}]}],
}


async def test_validate_reports_every_schema_error(client):
    r = await client.post("/api/v1/config/validate", json={"config": BAD}, headers=H)
    body = await r.json()
    assert body["ok"] is False and len(body["errors"]) >= 3
    assert ["devices", 0, "id"] in [e["path"] for e in body["errors"]]
    assert all(e["msg"] and isinstance(e["path"], list) for e in body["errors"])
    r = await client.post("/api/v1/config/validate?all=0", json={"config": BAD}, headers=H)
    assert len((await r.json())["errors"]) == 1  # first-error mode stays available


async def test_validate_reports_every_dangling_reference(client):
    cfg = {
        "devices": [
            {"id": "tv", "commands": {"v": {"proto": "nec", "addr": 4, "cmd": 1}}},
            {"id": "tv", "commands": {}},
        ],
        "activities": [
            {"id": "a", "name": "A", "devices": [{"device": "ghost"}, {"device": "tv"}]},
            {"id": "b", "name": "B", "devices": [{"device": "nope"}]},
        ],
        "keys": {"menu": {"activity": "zzz"}},
    }
    body = await (
        await client.post("/api/v1/config/validate", json={"config": cfg}, headers=H)
    ).json()
    msgs = " | ".join(e["msg"] for e in body["errors"])
    assert len(body["errors"]) >= 4
    assert "duplicate device" in msgs and "ghost" in msgs and "nope" in msgs and "zzz" in msgs
    r = await client.post("/api/v1/config/validate?all=0", json={"config": cfg}, headers=H)
    assert len((await r.json())["errors"]) == 1


async def test_apply_refusal_lists_all_errors(client):
    await put_draft(client, BAD)
    body = await (await client.post("/api/v1/config/apply", json={}, headers=H)).json()
    assert len(body["errors"]) >= 3


async def test_valid_config_still_validates(client):
    body = await (
        await client.post("/api/v1/config/validate", json={"config": CONFIG}, headers=H)
    ).json()
    assert body == {"ok": True, "errors": []}


def test_core_parse_model_stays_fail_fast_and_validate_all_is_a_superset():
    text = json.dumps(BAD)
    with pytest.raises(ConfigError) as ei:
        parse_model(text)
    errs = validate_all(text)
    assert len(errs) >= 3 and errs[0].path == ei.value.path and errs[0].msg == ei.value.msg
    assert validate_all(json.dumps(CONFIG)) == []
    assert validate_all("devices: [")[0].msg.startswith("invalid YAML")


# ---------------------------------------------------------------- hubs, ack, status
TOKEN = "ab" * 32


@pytest.fixture
async def feed(aiohttp_client, tmp_path):
    app = create_app(tmp_path, auth=NoAuth(), clock=clock, bundle_token=TOKEN, bundle_networks=None)
    return await aiohttp_client(app)


async def apply(c, hub, config=CONFIG):
    assert (
        await c.put(f"/api/v1/config/draft?hub={hub}", json={"config": config}, headers=H)
    ).status == 200
    r = await c.post(f"/api/v1/config/apply?hub={hub}", json={}, headers=H)
    assert r.status == 200
    return (await get_bundle(c, hub))["rev"]


async def hub_status(c, hub):
    """The `hub:<id>` snapshot of /live (what the removed config/status returned)."""
    ws = await live(c, f"hub:{hub}")
    try:
        return (await recv(ws, "snapshot"))["topics"][f"hub:{hub}"]
    finally:
        await ws.close()


async def test_hubs_lists_what_the_store_knows(feed):
    assert (await (await feed.get("/api/v1/hubs")).json())["hubs"] == []
    await put_draft_hub(feed, "b-hub")
    rev = await apply(feed, "a-hub")
    hubs = (await (await feed.get("/api/v1/hubs")).json())["hubs"]
    assert [h["id"] for h in hubs] == ["a-hub", "b-hub"]
    a, b = hubs
    assert a["has_applied"] and not a["has_draft"] and a["applied_rev"] == rev
    assert isinstance(a["applied_at"], int)
    assert b == {
        "id": "b-hub",
        "has_draft": True,
        "has_applied": False,
        "applied_rev": None,
        "applied_at": None,
    }


async def put_draft_hub(c, hub):
    await c.put(f"/api/v1/config/draft?hub={hub}", json={"config": CONFIG}, headers=H)


async def test_hubs_ignores_junk_files_and_corrupt_state(feed, tmp_path):
    await put_draft_hub(feed, "good")
    d = tmp_path / "hubs"
    (d / "bad name.json").write_text("{}")
    (d / "corrupt.json").write_text("not json")
    r = await feed.get("/api/v1/hubs")
    assert r.status == 409  # a corrupt state file is reported, not skipped silently
    (d / "corrupt.json").unlink()
    assert [h["id"] for h in (await (await feed.get("/api/v1/hubs")).json())["hubs"]] == ["good"]


async def test_ack_roundtrip_and_status(feed, tmp_path):
    st = await hub_status(feed, "h1")
    assert st == {
        "hub": "h1",
        "applied_rev": None,
        "acked_rev": None,
        "result": None,
        "detail": None,
        "acked_at": None,
        "connected": False,
        "connected_at": None,
        "remotes": [],
        "theme_notices": [],
        "now": st["now"],
    }
    rev = await apply(feed, "h1")
    ws = await connect(feed, "h1")
    await recv(ws, "bundle")
    st = await hub_status(feed, "h1")
    assert st["applied_rev"] == rev and st["acked_rev"] is None and st["connected"] is True
    assert isinstance(st["connected_at"], int)
    await ws.send_json({"t": "ack", "rev": rev, "result": "error", "detail": "hub said no"})
    assert await drain(ws) == []
    st = await hub_status(feed, "h1")
    assert (st["acked_rev"], st["result"], st["detail"]) == (rev, "error", "hub said no")
    assert isinstance(st["acked_at"], int)
    # persisted in the hub's store file: a fresh app on the same dir still knows it
    app2 = create_app(tmp_path, auth=NoAuth(), bundle_token=TOKEN, bundle_networks=None)
    from aiohttp.test_utils import TestClient, TestServer

    async with TestClient(TestServer(app2)) as c2:
        st2 = await hub_status(c2, "h1")
        assert st2["result"] == "error" and st2["acked_rev"] == rev
        assert st2["connected"] is False  # `connected` is per process lifetime
    await ws.send_json({"t": "ack", "rev": rev, "result": "ok"})
    assert await drain(ws) == []
    st = await hub_status(feed, "h1")
    assert st["result"] == "ok" and st["detail"] is None
    await ws.close()


@pytest.mark.parametrize(
    "frame",
    [
        {"rev": "R", "result": "maybe"},
        {"rev": "R", "result": "ok", "detail": "x" * 201},
        {"rev": "has space", "result": "ok"},
        {"rev": "r" * 65, "result": "ok"},
        {"rev": 5, "result": "ok"},
        {"rev": "R", "result": "ok", "detail": {"a": 1}},
        {"result": "ok"},
    ],
)
async def test_hostile_ack_frames_are_bad_frame_and_change_nothing(feed, frame):
    rev = await apply(feed, "h1")
    ws = await connect(feed, "h1")
    await recv(ws, "bundle")
    if frame.get("rev") == "R":
        frame = {**frame, "rev": rev}
    await ws.send_json({"t": "ack", **frame})
    assert (await recv(ws))["code"] == "bad_frame"
    assert (await hub_status(feed, "h1"))["acked_rev"] is None
    await ws.close()


async def test_ack_for_a_stale_rev_is_refused_and_unknown_hubs_are_not_created(feed):
    await apply(feed, "h1")
    ws = await connect(feed, "h1")
    await recv(ws, "bundle")
    await ws.send_json({"t": "ack", "rev": "old", "result": "ok"})
    assert (await recv(ws))["code"] == "stale_rev"
    ghost = await connect(feed, "ghost")  # nothing applied there
    await recv(ghost, "nothing_applied")
    await ghost.send_json({"t": "ack", "rev": "old", "result": "ok"})
    assert (await recv(ghost))["code"] == "stale_rev"
    assert [h["id"] for h in (await (await feed.get("/api/v1/hubs")).json())["hubs"]] == ["h1"]
    await ws.close()
    await ghost.close()


async def test_ack_markup_in_detail_is_stored_inert(feed):
    rev = await apply(feed, "h1")
    d = "<script>alert(1)</script>"
    ws = await connect(feed, "h1")
    await recv(ws, "bundle")
    await ws.send_json({"t": "ack", "rev": rev, "result": "error", "detail": d})
    assert await drain(ws) == []
    assert (await hub_status(feed, "h1"))["detail"] == d  # JSON data over a WebSocket, never markup
    await ws.close()


async def test_repeated_identical_acks_do_not_rewrite_the_store(feed, tmp_path):
    rev = await apply(feed, "h1")
    ws = await connect(feed, "h1")
    await recv(ws, "bundle")
    b = {"t": "ack", "rev": rev, "result": "ok"}
    await ws.send_json(b)
    await drain(ws)
    f = tmp_path / "hubs" / "h1.json"
    m = f.stat().st_mtime_ns
    for _ in range(3):
        await ws.send_json(b)
    await drain(ws)
    assert f.stat().st_mtime_ns == m
    await ws.close()


# ---------------------------------------------------------------- history rows
async def test_history_entry_returns_the_source_config_by_rev(client):
    NEWER = json.loads(json.dumps(CONFIG))
    NEWER["devices"][0]["settle_ms"] = 250
    await put_draft(client)
    await client.post("/api/v1/config/apply", json={}, headers=H)
    await client.put("/api/v1/config/draft", json={"config": NEWER}, headers=H)
    await client.post("/api/v1/config/apply", json={}, headers=H)
    meta = await (await client.get("/api/v1/config")).json()
    old, cur = meta["history"][0], meta["applied"]
    r = await client.get("/api/v1/config/history", params={"rev": str(old["rev"])})
    body = await r.json()
    assert r.status == 200 and body["rev"] == old["rev"] and body["config"] == CONFIG
    assert set(body) == {"hub", "rev", "at", "hash", "config"}
    r = await client.get("/api/v1/config/history", params={"rev": str(cur["rev"])})
    assert (await r.json())["config"] == NEWER
    r = await client.get("/api/v1/config/history", params={"rev": "99"})
    assert r.status == 404 and (await r.json())["error"]["code"] == "unknown_rev"
    for bad in ({}, {"rev": "x"}, {"rev": "-1"}):
        assert (await client.get("/api/v1/config/history", params=bad)).status == 400


async def test_ha_actions_in_validate_and_keymap_and_hostile_ones_refused(client):
    ok = {"keys": {"up": {"ha": {"service": "light.toggle", "target": {"entity_id": "light.a"}}}}}
    r = await client.post("/api/v1/config/keymap", json={"config": ok}, headers=H)
    rows = {x["key"]: x for x in (await r.json())["default"]}
    assert rows["up"]["action"] == {
        "kind": "ha",
        "service": "light.toggle",
        "entity": "light.a",
        "label": "light.toggle light.a",
    }
    bad = {"keys": {"up": {"ha": {"service": "shell_command.x", "target": {"entity_id": "light.a"}}}}}
    r = await client.post("/api/v1/config/validate", json={"config": bad}, headers=H)
    body = await r.json()
    assert any("not allowed" in e["msg"] for e in body["errors"])
    r = await client.post("/api/v1/config/keymap", json={"config": bad}, headers=H)
    assert r.status == 422
