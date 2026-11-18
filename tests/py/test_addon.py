"""Editor add-on server: routes, store, and the security requirements (S1, S2, S3, S8)."""

import json
import logging
import os

import pytest
from aiohttp import FormData

from harmony_addon.app import create_app
from harmony_addon.auth import IngressAuth, NoAuth
from harmony_addon.options import Options
from harmony_addon.store import HISTORY_MAX, Store, StoreError, atomic_write
from harmony_core.config_model import load_source, parse_model

SMALL = """
devices:
  - id: tv
    power: {on: {proto: nec, addr: 4, cmd: 2}, off: {proto: nec, addr: 4, cmd: 3}}
    inputs: {hdmi1: {proto: nec, addr: 4, cmd: 9}}
    settle_ms: 100
    commands: {vol_up: {proto: nec, addr: 4, cmd: 16}}
activities:
  - id: watch
    name: Watch
    devices: [{device: tv, input: hdmi1}]
    keys: {select: {device: tv, command: vol_up}}
keys:
  menu: {activity: watch}
pages:
  - id: p
    cols: 2
    rows: 2
    widgets:
      - {type: button, id: b, x: 0, y: 0, label: Go, action: {activity: "off"}}
      - {type: tile, id: t, x: 1, y: 0, label: T, entity: sensor.temp}
"""
CONFIG = load_source(SMALL)
parse_model(SMALL)
H = {"X-HH-CSRF": "1"}
SECRET = "s3cr3t-hub-token-XYZ"
NOW = [1_000]


def clock():
    NOW[0] += 1
    return NOW[0]


@pytest.fixture
async def client(aiohttp_client, tmp_path):
    app = create_app(
        tmp_path, options=Options("10.0.0.5", 8099, SECRET, "info"), clock=clock, auth=NoAuth()
    )
    return await aiohttp_client(app)


async def put_draft(c, config=CONFIG):
    return await c.put("/api/v1/config/draft", json={"config": config}, headers=H)


# ---------------------------------------------------------------- routes
async def test_health_and_placeholder(client):
    r = await client.get("/api/v1/health")
    body = await r.json()
    assert r.status == 200 and body["ok"] and body["bundle_schema"] == 1
    assert body["hub"] == {"hub_host": "10.0.0.5", "hub_port": 8099, "hub_token_set": True}
    page = await (await client.get("/")).text()
    assert "not built yet" in page and "<script" not in page


async def test_draft_validate_preview_apply_revert(client):
    assert (await (await client.get("/api/v1/config")).json())["draft"] is None
    assert (await client.post("/api/v1/config/apply", json={}, headers=H)).status == 409
    assert (await put_draft(client)).status == 200
    got = await (await client.get("/api/v1/config")).json()
    assert got["draft"]["config"] == CONFIG and got["applied"] is None

    v = await (await client.post("/api/v1/config/validate", json={}, headers=H)).json()
    assert v == {"ok": True, "errors": []}
    bad = {**CONFIG, "activities": [{"id": "x"}]}
    v = await (await client.post("/api/v1/config/validate", json={"config": bad}, headers=H)).json()
    assert v["ok"] is False and v["errors"][0]["path"]

    p = await (await client.post("/api/v1/config/preview", json={}, headers=H)).json()
    assert p["ok"] and p["pages"][0]["widgets"] and p["usage"]["json_bytes"] > 0
    assert p["usage"]["pool_max"] == 16384

    a = await (await client.post("/api/v1/config/apply", json={}, headers=H)).json()
    assert a["applied"]["rev"] == 1 and a["applied"]["hash"] == p["hash"]
    got = await (await client.get("/api/v1/config?full=1")).json()
    assert got["draft"] is None and got["applied"]["config"] == CONFIG

    await put_draft(client, {**CONFIG, "activities": []})
    r = await client.post("/api/v1/config/revert", json={"target": "applied"}, headers=H)
    assert (await r.json())["applied"]["rev"] == 1
    assert (await (await client.get("/api/v1/config")).json())["draft"] is None


async def test_apply_refuses_invalid_draft(client):
    await put_draft(client, {"devices": [{"id": "BAD ID"}]})
    r = await client.post("/api/v1/config/apply", json={}, headers=H)
    body = await r.json()
    assert r.status == 422 and body["ok"] is False and body["errors"]
    assert (await (await client.get("/api/v1/config")).json())["applied"] is None


async def test_history_cap_and_previous_revert(client):
    for i in range(HISTORY_MAX + 3):
        await put_draft(
            client, {**CONFIG, "devices": [{**CONFIG["devices"][0], "settle_ms": 100 + i}]}
        )
        r = await client.post("/api/v1/config/apply", json={}, headers=H)
        assert r.status == 200, await r.text()
    meta = await (await client.get("/api/v1/config")).json()
    assert len(meta["history"]) == HISTORY_MAX
    assert [h["rev"] for h in meta["history"]] == list(range(HISTORY_MAX + 2, 2, -1))
    r = await client.post("/api/v1/config/revert", json={"target": "previous"}, headers=H)
    meta = await r.json()
    assert meta["applied"]["rev"] == HISTORY_MAX + 2 and len(meta["history"]) == HISTORY_MAX - 1


async def test_revert_previous_without_history(client):
    r = await client.post("/api/v1/config/revert", json={"target": "previous"}, headers=H)
    assert r.status == 409


async def test_yaml_roundtrip_through_core(client):
    hy = {**H, "Content-Type": "text/yaml"}
    r = await client.post("/api/v1/config/import_yaml", data=SMALL, headers=hy)
    assert r.status == 200, await r.text()
    out = await client.get("/api/v1/config/export_yaml")
    assert out.content_type == "text/yaml"
    assert parse_model(await out.text()) == parse_model(SMALL)
    assert load_source(await out.text()) == CONFIG
    got = await (await client.get("/api/v1/config")).json()
    assert got["draft"]["config"] == CONFIG


async def test_import_yaml_rejects_ir_file_refs_and_bad_yaml(client):
    hy = {**H, "Content-Type": "application/yaml"}
    text = SMALL.replace("{proto: nec, addr: 4, cmd: 16}", "{flipper: {file: tv.ir, signal: Vol}}")
    r = await client.post("/api/v1/config/import_yaml", data=text, headers=hy)
    body = await r.json()
    assert r.status == 422 and body["error"]["files"] == ["flipper:tv.ir"]
    r = await client.post("/api/v1/config/import_yaml", data="devices: [", headers=hy)
    assert r.status == 422
    r = await client.post("/api/v1/config/import_yaml", data="a: &x [1]\nb: *x", headers=hy)
    assert r.status == 400


# ---------------------------------------------------------------- S1
ROUTES = [
    ("GET", "/"),
    ("GET", "/nope"),
    ("GET", "/api/v1/health"),
    ("GET", "/api/v1/config"),
    ("PUT", "/api/v1/config/draft"),
    ("POST", "/api/v1/config/validate"),
    ("POST", "/api/v1/config/preview"),
    ("POST", "/api/v1/config/apply"),
    ("POST", "/api/v1/config/revert"),
    ("POST", "/api/v1/config/import_yaml"),
    ("GET", "/api/v1/config/export_yaml"),
]


async def test_s1_non_ingress_peer_is_forbidden_everywhere(aiohttp_client, tmp_path):
    c = await aiohttp_client(create_app(tmp_path))  # default IngressAuth; peer is 127.0.0.1
    for method, path in ROUTES:
        r = await c.request(method, path, headers={**H, "X-Ingress-Path": "/x"}, json={})
        assert r.status == 403, (method, path)
    # every registered route is covered by the list above
    app_paths = {res.canonical for res in c.app.router.resources()}
    assert app_paths <= {p for _, p in ROUTES}


async def test_s1_ingress_peer_needs_header_for_ui(aiohttp_client, tmp_path):
    c = await aiohttp_client(create_app(tmp_path, auth=IngressAuth(peers={"127.0.0.1"})))
    assert (await c.get("/")).status == 403
    assert (await c.get("/", headers={"X-Ingress-Path": "/api/hassio_ingress/t"})).status == 200
    assert (await c.get("/api/v1/health")).status == 200


def test_s1_default_peer_is_the_supervisor_gateway():
    assert IngressAuth().peers == {"172.30.32.2"}


# ---------------------------------------------------------------- S2
MUT = [r for r in ROUTES if r[0] in ("POST", "PUT")]


async def test_s2_content_type_and_csrf_header_required(client):
    for method, path in MUT:
        ctype = "text/yaml" if path.endswith("import_yaml") else "application/json"
        # form post (what a cross-site <form> can send): wrong type, no custom header
        form = FormData()
        form.add_field("config", "x")
        r = await client.request(method, path, data=form)
        assert r.status == 403, (method, path)
        # right type but no custom header
        r = await client.request(method, path, data="{}", headers={"Content-Type": ctype})
        assert r.status == 403, (method, path)
        # custom header but wrong type
        r = await client.request(
            method, path, data="{}", headers={**H, "Content-Type": "text/plain"}
        )
        assert r.status == 403, (method, path)
        # right everything, cross-site markers
        for extra in (
            {"Origin": "https://evil.example"},
            {"Sec-Fetch-Site": "cross-site"},
            {"Sec-Fetch-Site": "same-site"},
        ):
            r = await client.request(
                method, path, data="{}", headers={**H, "Content-Type": ctype, **extra}
            )
            assert r.status == 403, (method, path, extra)


async def test_s2_json_type_not_accepted_for_yaml_and_vice_versa(client):
    r = await client.post(
        "/api/v1/config/import_yaml", data="{}", headers={**H, "Content-Type": "application/json"}
    )
    assert r.status == 403
    r = await client.post(
        "/api/v1/config/apply", data="{}", headers={**H, "Content-Type": "text/yaml"}
    )
    assert r.status == 403


async def test_s2_same_origin_is_accepted(client):
    host = f"{client.host}:{client.port}"
    r = await client.post(
        "/api/v1/config/apply",
        json={},
        headers={**H, "Origin": f"http://{host}", "Sec-Fetch-Site": "same-origin"},
    )
    assert r.status == 409  # passed CSRF; just no draft yet


# ---------------------------------------------------------------- S3
async def test_s3_unknown_keys_and_bad_bodies_are_400(client):
    for path, body in [
        ("/api/v1/config/validate", {"extra": 1}),
        ("/api/v1/config/apply", {"x": 1}),
        ("/api/v1/config/revert", {"target": "nope"}),
        ("/api/v1/config/revert", {}),
        ("/api/v1/config/validate", {"config": []}),
    ]:
        r = await client.post(path, json=body, headers=H)
        assert r.status == 400, (path, body)
    r = await client.put("/api/v1/config/draft", json={"config": {}, "x": 1}, headers=H)
    assert r.status == 400
    r = await client.post(
        "/api/v1/config/apply", data="{nope", headers={**H, "Content-Type": "application/json"}
    )
    assert r.status == 400
    for q in ("?hub=../x", "?bogus=1", "?hub=A"):
        assert (await client.get("/api/v1/config" + q)).status == 400


async def test_s3_caps(client):
    r = await client.put(
        "/api/v1/config/draft", json={"config": {f"k{i}": 1 for i in range(17)}}, headers=H
    )
    assert r.status == 400
    deep = cur = {}
    for _ in range(30):
        cur["a"] = {}
        cur = cur["a"]
    assert (
        await client.put("/api/v1/config/draft", json={"config": deep}, headers=H)
    ).status == 400
    wide = {"a": list(range(70_000))}
    assert (
        await client.put("/api/v1/config/draft", json={"config": wide}, headers=H)
    ).status == 400
    big = json.dumps({"config": {"a": "x" * 1_100_000}})
    r = await client.put(
        "/api/v1/config/draft", data=big, headers={**H, "Content-Type": "application/json"}
    )
    assert r.status == 413
    assert (await r.json())["error"]["msg"]


# ---------------------------------------------------------------- headers, S8, escaping
async def test_security_headers(client):
    for path in ("/", "/api/v1/health", "/missing"):
        r = await client.get(path)
        assert "default-src 'none'" in r.headers["Content-Security-Policy"]
        assert r.headers["X-Content-Type-Options"] == "nosniff"
        assert r.headers["Referrer-Policy"] == "no-referrer"
    assert (await client.get("/api/v1/health")).headers["Cache-Control"] == "no-store"
    assert (await client.get("/api/v1/nope")).headers["Cache-Control"] == "no-store"


async def test_s8_token_never_in_responses_or_logs(client, caplog):
    caplog.set_level(logging.DEBUG)
    seen = []
    for method, path in ROUTES:
        r = await client.request(
            method, path, headers={**H, "Content-Type": "application/json"}, data="{}"
        )
        seen.append(await r.text() + str(dict(r.headers)))
    await put_draft(client)
    seen.append(await (await client.get("/api/v1/config?full=1")).text())
    seen.append(await (await client.get("/")).text())
    assert not any(SECRET in s for s in seen)
    assert SECRET not in caplog.text
    assert SECRET not in repr(Options("h", 1, SECRET, "info"))


async def test_errors_do_not_echo_raw_input(client):
    evil = "<script>alert(1)</script>"
    r = await client.get("/api/v1/config", params={evil: "1"})
    assert r.status == 400 and evil not in await r.text()
    r = await client.get("/api/v1/config", params={"hub": evil})
    assert evil not in await r.text()
    r = await client.post("/api/v1/config/validate", json={evil: 1}, headers=H)
    assert evil not in await r.text()
    r = await client.get(f"/{evil}")
    assert evil not in await r.text() and r.content_type == "application/json"


async def test_options_file_is_read(aiohttp_client, tmp_path):
    (tmp_path / "options.json").write_text(
        json.dumps({"hub_host": "<b>h</b>", "hub_port": 99, "hub_token": SECRET, "log_level": "x"})
    )
    c = await aiohttp_client(create_app(tmp_path, auth=NoAuth()))
    assert SECRET not in await (await c.get("/api/v1/health")).text()
    page = await (await c.get("/")).text()
    assert "<b>" not in page and "&lt;b&gt;h&lt;/b&gt;" in page


# ---------------------------------------------------------------- store
def test_atomic_write_crash_keeps_old_file(tmp_path, monkeypatch):
    p = tmp_path / "x.json"
    atomic_write(p, b"old")

    def boom(*a):
        raise OSError("crash")

    monkeypatch.setattr(os, "replace", boom)
    with pytest.raises(OSError):
        atomic_write(p, b"new")
    assert p.read_bytes() == b"old"
    assert list(tmp_path.iterdir()) == [p]  # temp file cleaned up


def test_store_roundtrip_and_corruption(tmp_path):
    s = Store(tmp_path)
    s.set_draft("default", {"a": 1}, 5)
    assert Store(tmp_path).load("default")["draft"] == {"config": {"a": 1}, "updated": 5}
    assert json.loads((tmp_path / "hubs" / "default.json").read_text())["v"] == 1
    (tmp_path / "hubs" / "default.json").write_text("{nope")
    with pytest.raises(StoreError):
        s.load("default")
    (tmp_path / "hubs" / "default.json").write_text('{"v": 99}')
    with pytest.raises(StoreError):
        s.load("default")
    with pytest.raises(StoreError):
        s.load("../x")


async def test_storage_only_under_data_dir(client, tmp_path):
    await put_draft(client)
    files = {p.relative_to(tmp_path).as_posix() for p in tmp_path.rglob("*") if p.is_file()}
    assert files == {"hubs/default.json"}


async def test_yaml_export_is_plain_yaml(client):
    await put_draft(client)
    text = await (await client.get("/api/v1/config/export_yaml?source=draft")).text()
    assert load_source(text) == CONFIG
    assert (await client.get("/api/v1/config/export_yaml?source=applied")).status == 404
