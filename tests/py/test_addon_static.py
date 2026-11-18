"""Add-on static UI serving: index, hashed assets, cache + security headers, ingress prefixes."""

import re

import pytest

from harmony_addon.app import create_app
from harmony_addon.auth import IngressAuth, NoAuth
from harmony_addon.options import Options

INDEX = (
    '<!doctype html><html><head><script type="module" src="./assets/app-abc.js"></script>'
    '<link rel="stylesheet" href="./assets/app-abc.css"></head><body><hh-app></hh-app></body></html>'
)


@pytest.fixture
def static_dir(tmp_path):
    root = tmp_path / "frontend"
    (root / "assets").mkdir(parents=True)
    (root / "index.html").write_text(INDEX)
    (root / "assets" / "app-abc.js").write_text("console.log(1)")
    (root / "assets" / "app-abc.css").write_text("body{}")
    (tmp_path / "secret.txt").write_text("nope")
    return root


@pytest.fixture
async def ui(aiohttp_client, tmp_path, static_dir):
    app = create_app(tmp_path / "data", options=Options("info"), auth=NoAuth(), static_dir=static_dir)
    return await aiohttp_client(app)


async def test_index_and_assets_with_cache_and_security_headers(ui):
    r = await ui.get("/")
    body = await r.text()
    assert r.status == 200 and r.content_type == "text/html" and "<hh-app>" in body
    assert r.headers["Cache-Control"] == "no-cache"
    csp = r.headers["Content-Security-Policy"]
    assert "script-src 'self'" in csp and "style-src 'self'" in csp
    assert "connect-src 'self'" in csp  # without it default-src 'none' blocks the UI's own fetch()
    assert "unsafe" not in csp and "default-src 'none'" in csp
    assert r.headers["X-Content-Type-Options"] == "nosniff"
    for path, ctype in (("/assets/app-abc.js", "javascript"), ("/assets/app-abc.css", "text/css")):
        r = await ui.get(path)
        assert r.status == 200 and ctype in r.content_type
        assert "immutable" in r.headers["Cache-Control"]
        assert "default-src 'none'" in r.headers["Content-Security-Policy"]
        assert r.headers["X-Content-Type-Options"] == "nosniff"
        assert r.headers["Referrer-Policy"] == "no-referrer"
    assert (await ui.get("/api/v1/health")).headers["Cache-Control"] == "no-store"


async def test_index_has_only_relative_urls_so_any_ingress_prefix_works(ui):
    hdr = {"X-Ingress-Path": "/api/hassio_ingress/Zq3-any_token"}
    body = await (await ui.get("/", headers=hdr)).text()
    assert "hassio_ingress" not in body  # nothing is rewritten per prefix
    urls = re.findall(r'(?:src|href)="([^"]+)"', body)
    assert urls and all(u.startswith("./") for u in urls)


async def test_missing_and_traversal_never_leak_files(ui):
    for path in ("/assets/nope.js", "/assets/../secret.txt", "/assets/%2e%2e/secret.txt", "/index.html"):
        r = await ui.get(path)
        assert r.status in (403, 404), path
        assert "nope" not in await r.text()
        assert "default-src 'none'" in r.headers["Content-Security-Policy"]


async def test_ui_still_needs_ingress_peer_and_header(aiohttp_client, tmp_path, static_dir):
    app = create_app(
        tmp_path / "data",
        options=Options("info"),
        auth=IngressAuth(peers={"127.0.0.1"}),
        static_dir=static_dir,
    )
    c = await aiohttp_client(app)
    assert (await c.get("/")).status == 403  # no X-Ingress-Path
    assert (await c.get("/assets/app-abc.js")).status == 403
    assert (await c.get("/", headers={"X-Ingress-Path": "/x"})).status == 401  # no user id
    ok = {"X-Ingress-Path": "/x", "X-Remote-User-Id": "u1"}
    assert (await c.get("/", headers=ok)).status == 200
    assert (await c.get("/assets/app-abc.js", headers=ok)).status == 200


async def test_without_a_built_frontend_the_placeholder_is_served(aiohttp_client, tmp_path):
    app = create_app(tmp_path, options=Options("info"), auth=NoAuth(), static_dir=tmp_path / "none")
    c = await aiohttp_client(app)
    assert "not built yet" in await (await c.get("/")).text()
