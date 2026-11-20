"""Editor add-on server: routes, store, and the security requirements (S1, S2, S3, S8)."""

import json
import os
import tempfile
from pathlib import Path

import pytest
from aiohttp import FormData

from harmony_addon import schemas
from harmony_addon.app import MAX_BODY, create_app
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
NOW = [1_000]


def clock():
    NOW[0] += 1
    return NOW[0]


@pytest.fixture
async def client(aiohttp_client, tmp_path):
    app = create_app(tmp_path, options=Options("info"), clock=clock, auth=NoAuth())
    return await aiohttp_client(app)


async def put_draft(c, config=CONFIG):
    return await c.put("/api/v1/config/draft", json={"config": config}, headers=H)


# ---------------------------------------------------------------- routes
async def test_health_and_placeholder(client):
    r = await client.get("/api/v1/health")
    body = await r.json()
    assert r.status == 200 and body["ok"] and body["bundle_schema"] == 1
    caps = body["capabilities"]
    assert caps["page_format"] == 2 and isinstance(caps["ha_actions"], bool)
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
    assert p["ok"] and p["pages"][0]["page"]["widgets"] and p["usage"]["json_bytes"] > 0
    v2 = p["pages"][0]["page"]
    assert v2["version"] == 2 and v2["total"] == len(p["pages"]) and v2["widgets"]
    assert {"role", "shape", "icon", "bind", "icon_only"} <= set(v2["widgets"][0])
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
    assert meta["applied"]["rev"] == HISTORY_MAX + 2 and len(meta["history"]) == HISTORY_MAX
    assert meta["history"][0]["rev"] == HISTORY_MAX + 3  # the version left is on top: a redo


async def test_revert_previous_can_itself_be_reverted(client):
    revs = []
    for i in range(3):
        await put_draft(
            client, {**CONFIG, "devices": [{**CONFIG["devices"][0], "settle_ms": 100 + i}]}
        )
        r = await client.post("/api/v1/config/apply", json={}, headers=H)
        revs.append((await r.json())["applied"]["rev"])
    for want in (revs[1], revs[2], revs[1]):  # back, forward again, back
        r = await client.post("/api/v1/config/revert", json={"target": "previous"}, headers=H)
        meta = await r.json()
        assert meta["applied"]["rev"] == want
    assert sorted(h["rev"] for h in meta["history"]) == [revs[0], revs[2]]


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
    assert r.status == 400
    r = await client.post("/api/v1/config/import_yaml", data="a: &x [1]\nb: *x", headers=hy)
    assert r.status == 400


# ---------------------------------------------------------------- S1
def router_routes(app) -> list[tuple[str, str]]:
    """Every (method, path) the app really serves, read from the router, so a new route cannot
    slip past the protection tests below."""
    return sorted(
        {
            (r.method, r.resource.canonical)
            for r in app.router.routes()
            if r.method != "HEAD" and r.resource is not None
        }
    )


def _is_channel(path: str) -> bool:
    return path == "/api/v1/channel" or path.startswith("/api/v1/channel/")


def all_routes() -> list[tuple[str, str]]:
    """The full router, built WITH a static UI so index and /assets are enumerated too."""
    with tempfile.TemporaryDirectory() as d:
        ui = Path(d) / "ui"
        (ui / "assets").mkdir(parents=True)
        (ui / "index.html").write_text("<!doctype html>")
        return router_routes(create_app(Path(d) / "data", auth=NoAuth(), static_dir=ui))


_ALL = all_routes()
ROUTES = [("GET", "/"), ("GET", "/nope")] + [r for r in _ALL if not _is_channel(r[1])]
CHANNEL_ROUTES = [r for r in _ALL if _is_channel(r[1])]


async def test_s1_non_ingress_peer_is_forbidden_everywhere(aiohttp_client, tmp_path):
    c = await aiohttp_client(create_app(tmp_path))  # default IngressAuth; peer is 127.0.0.1
    for method, path in ROUTES:
        r = await c.request(method, path, headers={**H, "X-Ingress-Path": "/x"}, json={})
        assert r.status == 403, (method, path)
    assert ("GET", "/api/v1/ha/entities") in ROUTES  # enumerated from the router


async def test_s1_ingress_peer_needs_header_for_ui(aiohttp_client, tmp_path):
    c = await aiohttp_client(create_app(tmp_path, auth=IngressAuth(peers={"127.0.0.1"})))
    assert (await c.get("/")).status == 403
    ing = {"X-Ingress-Path": "/api/hassio_ingress/t", "X-Remote-User-Id": "u1"}
    assert (await c.get("/", headers=ing)).status == 200
    assert (await c.get("/api/v1/health", headers={"X-Remote-User-Id": "u1"})).status == 200


async def test_s1_every_ingress_route_needs_the_supervisor_user_id(aiohttp_client, tmp_path, caplog):
    import logging

    c = await aiohttp_client(create_app(tmp_path, auth=IngressAuth(peers={"127.0.0.1"})))
    ing = {"X-Ingress-Path": "/x"}
    for method, path in ROUTES:
        r = await c.request(method, path, headers={**H, **ing}, json={})
        assert r.status == 401, (method, path)
        assert (await r.json())["error"]["code"] == "unauthorized"
    for bad in ("", "a b", "x" * 65, "<script>"):
        r = await c.get("/api/v1/health", headers={"X-Remote-User-Id": bad})
        assert r.status == 401, bad
    # with an id the actions work, and the audit line names the user (and nothing secret)
    hdr = {**H, **ing, "X-Remote-User-Id": "abc123"}
    caplog.set_level(logging.INFO, logger="harmony_addon.app")
    assert (await c.put("/api/v1/config/draft", json={"config": CONFIG}, headers=hdr)).status == 200
    assert (await c.post("/api/v1/config/apply", json={}, headers=hdr)).status == 200
    assert (await c.post("/api/v1/config/revert", json={"target": "applied"}, headers=hdr)).status == 200
    hy = {**hdr, "Content-Type": "text/yaml"}
    assert (await c.post("/api/v1/config/import_yaml", data=SMALL, headers=hy)).status == 200
    lines = [r.getMessage() for r in caplog.records if r.getMessage().startswith("audit ")]
    assert lines == [
        "audit user=abc123 action=apply hub=default",
        "audit user=abc123 action=revert:applied hub=default",
        "audit user=abc123 action=import_yaml hub=default",
    ]


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
    for q in ("?hub=../x", "?bogus=1", "?hub=a%20b"):
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
    wide = {"a": list(range(schemas.MAX_NODES + 10))}
    assert (
        await client.put("/api/v1/config/draft", json={"config": wide}, headers=H)
    ).status == 400
    big = json.dumps({"config": {"a": "x" * (MAX_BODY + 100_000)}})
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


async def test_options_file_is_read_and_stray_keys_are_ignored(aiohttp_client, tmp_path):
    (tmp_path / "options.json").write_text(
        json.dumps({"hub_token": "leak-me", "log_level": "<b>x</b>"})
    )
    c = await aiohttp_client(create_app(tmp_path, auth=NoAuth()))
    text = await (await c.get("/api/v1/health")).text() + await (await c.get("/")).text()
    assert "leak-me" not in text and "<b>" not in text and "log level: info" in text


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
    assert files == {"hubs/default.json", "bundle_token"}


async def test_yaml_export_is_plain_yaml(client):
    await put_draft(client)
    text = await (await client.get("/api/v1/config/export_yaml?source=draft")).text()
    assert load_source(text) == CONFIG
    assert (await client.get("/api/v1/config/export_yaml?source=applied")).status == 404


# ---------------------------------------------------------------- import_yaml hardening
YH = {**H, "Content-Type": "text/yaml"}


async def imp(client, text):
    r = await client.post("/api/v1/config/import_yaml", data=text, headers=YH)
    body = await r.text()
    assert "Traceback" not in body
    return r


async def test_import_enforces_bounds_like_put_draft(client):
    r = await imp(client, "devices: []\nnone: [" + ",".join(["0"] * (schemas.MAX_NODES + 10)) + "]")
    assert r.status == 400
    deep = "x: " + "[" * 30 + "]" * 30
    assert (await imp(client, deep)).status == 400
    assert (await client.get("/api/v1/config")).status == 200
    assert (await (await client.get("/api/v1/config")).json())["draft"] is None


async def test_import_non_json_yaml_types_are_400(client):
    for text in (
        "devices: []\nwhen: 2026-01-01",
        "devices: []\nwhen: 2026-01-01 10:00:00",
        "devices: []\nx: !!set {a, b}",
        "devices: []\nx: .nan",
        "devices: []\nx: .inf",
    ):
        assert (await imp(client, text)).status == 400, text


async def test_import_int_keys_match_apply(client):
    text = SMALL.replace("keys:\n  menu:", "keys:\n  5: {activity: watch}\n  menu:")
    r = await imp(client, text)
    # import and apply must agree: either both accept or import rejects (never import-OK, apply-422)
    a = await client.post("/api/v1/config/apply", json={}, headers=H)
    if r.status == 200:
        assert a.status == 200
    else:
        assert r.status == 422 and a.status == 409
    got = await (await client.get("/api/v1/config")).json()
    draft = got["draft"] or {"config": {}}
    assert all(isinstance(k, str) for k in draft["config"].get("keys", {}))


async def test_import_deep_block_nesting_is_400(client):
    block = "".join(" " * i + "a:\n" for i in range(1300))
    assert (await imp(client, block)).status == 400


async def test_import_alias_bomb_and_deep_nesting_are_400(client):
    bomb = "a: &a [x, x, x, x, x, x, x, x, x]\n" + "".join(
        f"{chr(98 + i)}: &{chr(98 + i)} [" + ", ".join([f"*{chr(97 + i)}"] * 9) + "]\n"
        for i in range(8)
    )
    assert (await imp(client, bomb)).status == 400
    assert (await imp(client, "[" * 900_000)).status == 400
    assert (await imp(client, "x: " + "[" * 400_000 + "]" * 400_000)).status == 400
    assert (await imp(client, "{" * 200_000)).status == 400
