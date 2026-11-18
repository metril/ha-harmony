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
