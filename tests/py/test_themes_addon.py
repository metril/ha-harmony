"""The add-on's validate and preview endpoints carry the theme problems (warnings with severity)."""

import copy

import pytest
from test_addon import H, clock
from test_addon_preview import big

from harmony_addon.app import create_app
from harmony_addon.auth import NoAuth


@pytest.fixture
async def c(aiohttp_client, tmp_path):
    return await aiohttp_client(create_app(tmp_path, auth=NoAuth(), clock=clock))


async def post(c, route, config):
    r = await c.post(f"/api/v1/config/{route}", json={"config": config}, headers=H)
    return r.status, await r.json()


def themed(**top):
    cfg = copy.deepcopy(big())
    cfg.update(top)
    cfg["pages"][0]["widgets"][0]["color"] = "red"
    cfg["pages"][0]["widgets"][1]["color"] = "#777777"
    return cfg


async def test_validate_and_preview_return_warnings_without_failing(c):
    cfg = themed(
        themes={"murky": {"extends": "default", "tokens": {"tx": "#404850"}}}, theme="murky"
    )
    for route in ("validate", "preview"):
        st, body = await post(c, route, cfg)
        assert st == 200 and body["ok"] is True, body
        warn = [e for e in body["errors"] if e.get("severity") == "warning"]
        assert len(warn) == len(body["errors"]) >= 2
        assert any(e["path"] == ["themes", "murky"] for e in warn)
        assert any(
            e["path"] == ["pages", 0, "widgets", 1, "color"]
            and "nearest passing colour" in e["msg"]
            for e in warn
        )
        assert all(set(e) == {"path", "msg", "line", "severity"} for e in warn)
    st, body = await post(c, "preview", cfg)
    assert body["pages"][0]["ok"] and body["pages"][0]["page"]["widgets"][0]["color"] == "red"


async def test_validate_and_preview_return_the_new_errors(c):
    bad = themed(theme="nope")
    bad["pages"][1]["widgets"][0]["color"] = "teal2"
    _st, body = await post(c, "validate", bad)
    assert body["ok"] is False
    msgs = {tuple(e["path"]): e["msg"] for e in body["errors"]}
    assert "unknown colour 'teal2'" in msgs[("pages", 1, "widgets", 0, "color")]
    assert all("severity" not in e for e in body["errors"])  # errors keep their shape
    bad["pages"][1]["widgets"][0]["color"] = "red"  # references are checked once the shape is right
    _st, body = await post(c, "validate", bad)
    assert (
        "unknown theme 'nope'" in {tuple(e["path"]): e["msg"] for e in body["errors"]}[("theme",)]
    )
    bad["theme"] = "default"
    bad["pages"][1]["widgets"][0]["color"] = "teal2"
    _st, body = await post(c, "preview", bad)
    assert body["ok"] is False and any("unknown colour" in e["msg"] for e in body["errors"])


async def test_unthemed_config_has_no_extra_fields(c):
    _st, body = await post(c, "validate", big())
    assert body == {"ok": True, "errors": []}
