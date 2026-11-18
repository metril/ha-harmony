"""Editor API completion: draft revisions, all validation errors, hub list, apply status."""

import json

import pytest
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
BEARER = {"Authorization": f"Bearer {TOKEN}"}
JH = {"Content-Type": "application/json"}


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
    return (await c.get(f"/api/v1/bundle?hub={hub}", headers=BEARER)).headers["ETag"].strip('"')


async def test_hubs_lists_what_the_store_knows(feed):
    assert (await (await feed.get("/api/v1/hubs")).json()) == {"hubs": []}
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
    st = await (await feed.get("/api/v1/config/status?hub=h1")).json()
    assert st == {
        "hub": "h1",
        "applied_rev": None,
        "acked_rev": None,
        "result": None,
        "detail": None,
        "acked_at": None,
        "linked": False,
        "last_seen": None,
    }
    rev = await apply(feed, "h1")
    st = await (await feed.get("/api/v1/config/status?hub=h1")).json()
    assert st["applied_rev"] == rev and st["acked_rev"] is None
    assert st["linked"] is True and isinstance(st["last_seen"], int)  # the bundle GET above
    r = await feed.post(
        "/api/v1/bundle/ack",
        json={"hub": "h1", "rev": rev, "result": "error", "detail": "hub said no"},
        headers={**BEARER, **JH},
    )
    assert r.status == 200
    st = await (await feed.get("/api/v1/config/status?hub=h1")).json()
    assert (st["acked_rev"], st["result"], st["detail"]) == (rev, "error", "hub said no")
    assert isinstance(st["acked_at"], int)
    # persisted in the hub's store file: a fresh app on the same dir still knows it
    app2 = create_app(tmp_path, auth=NoAuth(), bundle_token=TOKEN, bundle_networks=None)
    from aiohttp.test_utils import TestClient, TestServer

    async with TestClient(TestServer(app2)) as c2:
        st2 = await (await c2.get("/api/v1/config/status?hub=h1")).json()
        assert st2["result"] == "error" and st2["acked_rev"] == rev
        assert st2["linked"] is False  # `linked` is per process lifetime
    r = await feed.post(
        "/api/v1/bundle/ack",
        json={"hub": "h1", "rev": rev, "result": "ok"},
        headers={**BEARER, **JH},
    )
    st = await (await feed.get("/api/v1/config/status?hub=h1")).json()
    assert st["result"] == "ok" and st["detail"] is None


async def test_ack_needs_the_bearer_token_not_the_csrf_header(feed):
    rev = await apply(feed, "h1")
    body = {"hub": "h1", "rev": rev, "result": "ok"}
    assert (await feed.post("/api/v1/bundle/ack", json=body)).status == 401
    ing = {"X-Ingress-Path": "/api/hassio_ingress/t"}
    assert (
        await feed.post("/api/v1/bundle/ack", json=body, headers={**BEARER, **ing})
    ).status == 401
    r = await feed.post(
        "/api/v1/bundle/ack", data="{}", headers={**BEARER, "Content-Type": "text/plain"}
    )
    assert r.status == 403
    assert (await feed.post("/api/v1/bundle/ack", json=body, headers=BEARER)).status == 200


@pytest.mark.parametrize(
    "body",
    [
        {"hub": "h1", "rev": "R", "result": "maybe"},
        {"hub": "h1", "rev": "R", "result": "ok", "detail": "x" * 201},
        {"hub": "h1", "rev": "R", "result": "ok", "extra": 1},
        {"hub": "../x", "rev": "R", "result": "ok"},
        {"hub": "h1", "rev": "has space", "result": "ok"},
        {"hub": "h1", "rev": "r" * 65, "result": "ok"},
        {"hub": "h1", "rev": 5, "result": "ok"},
        {"hub": "h1", "rev": "R", "result": "ok", "detail": {"a": 1}},
        {"hub": "h1", "result": "ok"},
        [],
        "x",
        None,
    ],
)
async def test_hostile_ack_bodies_are_400_and_change_nothing(feed, body):
    rev = await apply(feed, "h1")
    if isinstance(body, dict) and body.get("rev") == "R":
        body = {**body, "rev": rev}
    r = await feed.post("/api/v1/bundle/ack", data=json.dumps(body), headers={**BEARER, **JH})
    assert r.status == 400
    st = await (await feed.get("/api/v1/config/status?hub=h1")).json()
    assert st["acked_rev"] is None


async def test_ack_for_a_stale_rev_or_unknown_hub_is_refused(feed):
    rev = await apply(feed, "h1")
    r = await feed.post(
        "/api/v1/bundle/ack", json={"hub": "h1", "rev": "old", "result": "ok"}, headers=BEARER
    )
    assert r.status == 409 and (await r.json())["error"]["code"] == "stale_rev"
    r = await feed.post(
        "/api/v1/bundle/ack", json={"hub": "ghost", "rev": rev, "result": "ok"}, headers=BEARER
    )
    assert r.status == 404
    assert [h["id"] for h in (await (await feed.get("/api/v1/hubs")).json())["hubs"]] == ["h1"]


async def test_ack_markup_in_detail_is_stored_inert(feed):
    rev = await apply(feed, "h1")
    d = "<script>alert(1)</script>"
    await feed.post(
        "/api/v1/bundle/ack",
        json={"hub": "h1", "rev": rev, "result": "error", "detail": d},
        headers=BEARER,
    )
    r = await feed.get("/api/v1/config/status?hub=h1")
    assert r.content_type == "application/json" and (await r.json())["detail"] == d
    assert "script-src 'self'" in r.headers["Content-Security-Policy"]


async def test_repeated_identical_acks_do_not_rewrite_the_store(feed, tmp_path):
    rev = await apply(feed, "h1")
    b = {"hub": "h1", "rev": rev, "result": "ok"}
    await feed.post("/api/v1/bundle/ack", json=b, headers=BEARER)
    f = tmp_path / "hubs" / "h1.json"
    m = f.stat().st_mtime_ns
    for _ in range(3):
        await feed.post("/api/v1/bundle/ack", json=b, headers=BEARER)
    assert f.stat().st_mtime_ns == m
