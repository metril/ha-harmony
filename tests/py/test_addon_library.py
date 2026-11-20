"""The config library: named configs, autosaved draft, immutable numbered revisions, v1 -> v2 migration."""

import json

import pytest
from cfgapi import apply_draft, ensure, put_apply, put_draft
from test_addon import CONFIG, H, clock

from harmony_addon.app import create_app
from harmony_addon.auth import NoAuth
from harmony_addon.library import Library, NotFound, migrate_v1
from harmony_addon.store import RevConflict, Store, StoreError


@pytest.fixture
def lib(tmp_path):
    return Library(tmp_path)


@pytest.fixture
async def client(aiohttp_client, tmp_path):
    return await aiohttp_client(create_app(tmp_path, auth=NoAuth(), clock=clock))


# ---------------------------------------------------------------- Library
def test_create_slugs_unique_ids_and_rejects_bad_names(lib):
    a = lib.create("Living Room!", {"a": 1}, 1)
    b = lib.create("Living Room!", None, 2)
    assert (a["id"], b["id"]) == ("living-room", "living-room-2")
    assert lib.create("???", None, 3)["id"] == "config"
    for bad in ("", "   ", "x" * 81, "a\x01b", 5):
        with pytest.raises(StoreError):
            lib.create(bad)
    assert [c["id"] for c in lib.list()] == ["config", "living-room", "living-room-2"]


def test_draft_if_rev_and_revisions_are_immutable(lib):
    lib.create("A", None, 1)
    m = lib.set_draft("a", {"v": 1}, 2, if_rev=0)
    assert m["draft_rev"] == 1
    with pytest.raises(RevConflict) as e:
        lib.set_draft("a", {"v": 2}, 3, if_rev=0)
    assert e.value.rev == 1
    _, row = lib.save_rev("a", "first", 4)
    assert row["rev"] == 1 and row["parent"] is None
    meta = lib.get("a")
    assert meta["draft"]["base_rev"] == 1 and Library.summary(meta)["dirty"] is False
    lib.set_draft("a", {"v": 2}, 5)
    assert Library.summary(lib.get("a"))["dirty"] is True
    _, row2 = lib.save_rev("a", "second", 6)
    assert row2["rev"] == 2 and row2["parent"] == 1
    lib.set_draft("a", {"v": 3}, 7)
    assert lib.get_rev("a", 1)["config"] == {"v": 1}  # a later edit never touches a revision
    assert lib.get_rev("a", 2)["config"] == {"v": 2}
    with pytest.raises(NotFound):
        lib.get_rev("a", 3)


def test_save_rev_needs_a_draft_and_reuse_skips_unchanged(lib):
    lib.create("A", None, 1)
    with pytest.raises(StoreError):
        lib.save_rev("a", "", 2)
    lib.set_draft("a", {"v": 1}, 2)
    lib.save_rev("a", "", 3)
    _, row = lib.save_rev("a", "", 4, reuse=True)
    assert row["rev"] == 1 and len(lib.get("a")["revs"]) == 1


def test_restore_duplicate_rename_delete(lib):
    lib.create("A", {"v": 1}, 1)
    lib.save_rev("a", "r1", 2)
    lib.set_draft("a", {"v": 2}, 3)
    m = lib.restore("a", 1, 4)
    assert m["draft"]["config"] == {"v": 1} and m["draft"]["base_rev"] == 1
    d = lib.duplicate("a", 1, "Copy", 5)
    assert d["id"] == "copy" and d["draft"]["config"] == {"v": 1} and d["revs"] == []
    lib.set_draft("a", {"v": 9}, 6)
    assert lib.duplicate("a", None, "Copy")["draft"]["config"] == {"v": 9}
    assert lib.rename("a", "  Better   Name ", 7)["name"] == "Better Name"
    assert lib.get("a")["id"] == "a"
    lib.delete("a")
    with pytest.raises(NotFound):
        lib.get("a")
    with pytest.raises(NotFound):
        lib.get("../x")


# ---------------------------------------------------------------- routes
async def test_crud_and_summary_shapes(client):
    r = await client.post("/api/v1/configs", json={"name": "Den"}, headers=H)
    body = await r.json()
    assert r.status == 201 and body["id"] == "den" and body["draft"] is None and body["revs"] == []
    await client.put("/api/v1/configs/den/draft", json={"config": CONFIG}, headers=H)
    r = await client.post("/api/v1/configs/den/revs", json={"note": "first"}, headers=H)
    saved = await r.json()
    assert r.status == 201 and saved["rev"]["rev"] == 1 and saved["rev"]["note"] == "first"
    r = await client.post(
        "/api/v1/configs", json={"name": "Den 2", "from": {"id": "den", "rev": 1}}, headers=H
    )
    assert (await r.json())["draft"]["config"] == CONFIG
    listing = await (await client.get("/api/v1/configs")).json()
    row = next(c for c in listing["configs"] if c["id"] == "den")
    assert set(row) == {
        "id", "name", "created", "updated", "latest_rev", "rev_count", "draft_rev", "has_draft",
        "draft_updated", "base_rev", "dirty", "hubs",
    }  # fmt: skip
    assert row["latest_rev"] == 1 and row["rev_count"] == 1 and row["dirty"] is False
    r = await client.patch("/api/v1/configs/den", json={"name": "Den Renamed"}, headers=H)
    assert (await r.json())["name"] == "Den Renamed"
    assert (await client.delete("/api/v1/configs/den", json={}, headers=H)).status == 200
    assert (await client.get("/api/v1/configs/den")).status == 404
    assert (await client.delete("/api/v1/configs/den", json={}, headers=H)).status == 404
    r = await client.post("/api/v1/configs", json={"name": "X", "from": {"id": "nope"}}, headers=H)
    assert r.status == 404


async def test_any_revision_applies_to_any_hub_and_deleting_the_config_keeps_the_hub(client):
    NEWER = {**CONFIG, "devices": [{**CONFIG["devices"][0], "settle_ms": 321}]}
    await put_apply(client, CONFIG, "hub-a")  # rev 1
    await put_apply(client, NEWER, "hub-b")  # rev 2 (draft changed)
    r = await apply_draft(client, "hub-b", rev=1)  # an old revision, to another hub
    assert r.status == 200 and (await r.json())["applied"]["rev"] == 1
    cfg = await (await client.get("/api/v1/configs/main")).json()
    assert sorted((h["hub"], h["rev"]) for h in cfg["hubs"]) == [("hub-a", 1), ("hub-b", 1)]
    assert [x["id"] for x in (await (await client.get("/api/v1/hubs")).json())["hubs"]] == [
        "hub-a",
        "hub-b",
    ]
    assert (await client.delete("/api/v1/configs/main", json={}, headers=H)).status == 200
    hub = await (await client.get("/api/v1/hubs/hub-b")).json()
    assert hub["config_exists"] is False and hub["applied"]["config_id"] == "main"
    assert hub["history"][0]["rev"] == 2
    # the hub still serves its snapshot and can revert to the previous one
    assert (await client.post("/api/v1/hubs/hub-b/revert", json={}, headers=H)).status == 200
    assert (await client.get("/api/v1/hubs/hub-a")).status == 200


async def test_apply_reuses_unchanged_draft_revision_and_unknown_config_is_404(client):
    assert (await apply_draft(client, cid="ghost")).status == 404
    await ensure(client)
    assert (await apply_draft(client)).status == 409  # no draft
    await put_draft(client, CONFIG)
    assert (await apply_draft(client)).status == 200
    assert (await apply_draft(client, "other")).status == 200
    assert (await (await client.get("/api/v1/configs/main")).json())["rev_count"] == 1


async def test_restore_route_and_live_configs_topic(client):
    from addon_ws import live, recv

    await put_apply(client, CONFIG)
    lv = await live(client, "configs")
    snap = await recv(lv, "snapshot")
    assert [c["id"] for c in snap["topics"]["configs"]["configs"]] == ["main"]
    await put_draft(client, {**CONFIG, "keys": {}})
    d = await recv(lv, "configs")
    assert d["configs"][0]["dirty"] is True
    r = await client.post("/api/v1/configs/main/restore", json={"rev": 1}, headers=H)
    assert r.status == 200 and (await r.json())["dirty"] is False
    assert (await client.post("/api/v1/configs/main/restore", json={"rev": 9}, headers=H)).status == 404
    await lv.close()


# ---------------------------------------------------------------- migration
def _v1(draft=True):
    def row(rev, n):
        return {"rev": rev, "at": 100 + rev, "hash": f"h{rev}", "config": {"n": n}}

    return {
        "v": 1,
        "next_rev": 4,
        "draft_rev": 7,
        "ack": {"rev": "abc", "result": "ok", "at": 5},
        "draft": {"config": {"n": "draft"}, "updated": 200} if draft else None,
        "applied": row(3, 3),
        "history": [row(2, 2), row(1, 1)],  # newest first
    }


def test_v1_hub_file_migrates_to_a_library_config(tmp_path):
    (tmp_path / "hubs").mkdir()
    f = tmp_path / "hubs" / "ha-harmony-abc.json"
    f.write_text(json.dumps(_v1()))
    lib = Library(tmp_path)
    assert migrate_v1(tmp_path, lib) == ["ha-harmony-abc"]
    meta = lib.get("ha-harmony-abc")
    assert meta["name"] == "ha-harmony-abc" and [r["rev"] for r in meta["revs"]] == [1, 2, 3]
    assert [lib.get_rev("ha-harmony-abc", n)["config"] for n in (1, 2, 3)] == [
        {"n": 1},
        {"n": 2},
        {"n": 3},
    ]
    assert meta["draft"]["config"] == {"n": "draft"} and meta["draft"]["base_rev"] == 3
    assert meta["draft_rev"] == 7 and meta["next_rev"] == 4
    state = Store(tmp_path).load("ha-harmony-abc")
    assert state["v"] == 2 and state["ack"]["result"] == "ok"
    a = state["applied"]
    assert (a["config_id"], a["rev"], a["hash"], a["config"]) == ("ha-harmony-abc", 3, "h3", {"n": 3})
    assert [h["rev"] for h in state["history"]] == [2, 1]
    # idempotent: a second run changes nothing
    before = f.read_bytes()
    assert migrate_v1(tmp_path, lib) == [] and f.read_bytes() == before
    assert lib.ids() == ["ha-harmony-abc"]


def test_interrupted_migration_reruns_cleanly(tmp_path):
    (tmp_path / "hubs").mkdir()
    (tmp_path / "hubs" / "h1.json").write_text(json.dumps(_v1(draft=False)))
    lib = Library(tmp_path)
    lib.create("h1", None, 1)["id"]  # stands in for a half-done run
    m = lib._meta("h1")
    m["migrated_from"] = "h1"
    lib._save(m)
    migrate_v1(tmp_path, lib)
    assert lib.ids() == ["h1"] and len(lib.get("h1")["revs"]) == 3 and lib.get("h1")["draft"] is None


async def test_app_start_migrates_and_serves_the_hub(aiohttp_client, tmp_path):
    cfg = json.loads(json.dumps(CONFIG))
    (tmp_path / "hubs").mkdir()
    v1 = {
        "v": 1, "next_rev": 2, "draft_rev": 0, "ack": None, "draft": None, "history": [],
        "applied": {"rev": 1, "at": 9, "hash": "hh", "config": cfg},
    }  # fmt: skip
    (tmp_path / "hubs" / "old-hub.json").write_text(json.dumps(v1))
    c = await aiohttp_client(create_app(tmp_path, auth=NoAuth(), clock=clock))
    hub = await (await c.get("/api/v1/hubs/old-hub")).json()
    assert hub["applied"]["config_id"] == "old-hub" and hub["config_exists"] is True
    got = await (await c.get("/api/v1/configs/old-hub")).json()
    assert got["revs"][0]["rev"] == 1 and got["hubs"] == [{"hub": "old-hub", "rev": 1}]


def test_crash_mid_migration_leaves_exactly_one_complete_config(tmp_path, monkeypatch):
    import harmony_addon.library as L

    (tmp_path / "hubs").mkdir()
    (tmp_path / "hubs" / "h1.json").write_text(json.dumps(_v1(draft=False)))
    lib = Library(tmp_path)
    real, calls = L.atomic_write, []

    def flaky(path, data):
        if path.parent.name == "revs":
            calls.append(path)
            if len(calls) == 2:
                raise OSError("crash")
        return real(path, data)

    monkeypatch.setattr(L, "atomic_write", flaky)
    with pytest.raises(OSError):
        migrate_v1(tmp_path, lib)
    monkeypatch.setattr(L, "atomic_write", real)
    assert migrate_v1(tmp_path, lib) == ["h1"]
    assert lib.ids() == ["h1"] and len(lib.get("h1")["revs"]) == 3
