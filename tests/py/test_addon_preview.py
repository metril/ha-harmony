"""Preview (fast, memoised, error-tolerant, metered), key map view, and `editor:` metadata."""

import json
import time

import pytest
from test_addon import CONFIG, H, clock, put_draft

from harmony_addon import app as app_mod
from harmony_addon.app import create_app
from harmony_addon.auth import NoAuth
from harmony_core.compiler import LIMITS, compile_config
from harmony_core.config_model import parse_data, validate_pages
from harmony_core.page_codec import MAX_BYTES, MAX_WIDGETS, Widget, encoded_size

TOKEN = "ab" * 32
BEARER = {"Authorization": f"Bearer {TOKEN}"}


def big():
    """~12 pages, 6 devices x 40 commands, 8 activities: a heavy real-world config."""
    devs = [
        {
            "id": f"dev{d}",
            "name": f"Dev {d}",
            "power": {
                "on": {"proto": "nec", "addr": d, "cmd": 1},
                "off": {"proto": "nec", "addr": d, "cmd": 2},
            },
            "inputs": {f"in{i}": {"proto": "nec", "addr": d, "cmd": 10 + i} for i in range(4)},
            "commands": {f"c{i}": {"proto": "nec", "addr": d, "cmd": i} for i in range(40)},
        }
        for d in range(6)
    ]
    acts = [
        {
            "id": f"a{i}",
            "name": f"Act {i}",
            "devices": [{"device": f"dev{i % 6}", "input": "in0"}],
            "keys": {
                k: {"device": f"dev{i % 6}", "command": "c1"}
                for k in ("volume_up", "volume_down", "volume_mute", "select", "up", "down")
            },
        }
        for i in range(8)
    ]
    pages = [
        {
            "id": f"p{p}",
            "cols": 4,
            "rows": 4,
            "widgets": [
                {
                    "type": "button",
                    "id": f"b{p}_{i}",
                    "x": i % 4,
                    "y": i // 4,
                    "label": f"B{i}",
                    "action": {"device": "dev0", "command": f"c{i}"},
                }
                for i in range(10)
            ],
        }
        for p in range(12)
    ]
    return {
        "devices": devs,
        "activities": acts,
        "keys": {"menu": {"activity": "a0"}},
        "pages": pages,
        "editor": {"layout": {"note": "x"}},
    }


@pytest.fixture
async def c(aiohttp_client, tmp_path):
    return await aiohttp_client(create_app(tmp_path, auth=NoAuth(), clock=clock))


async def preview(c, config=None):
    r = await c.post(
        "/api/v1/config/preview", json={} if config is None else {"config": config}, headers=H
    )
    return r.status, await r.json()


# ---------------------------------------------------------------- speed and memo
async def test_preview_of_a_heavy_config_is_fast_and_memoised(c, monkeypatch):
    cfg = big()
    t = time.perf_counter()
    st, body = await preview(c, cfg)
    cold = (time.perf_counter() - t) * 1000
    assert st == 200 and body["ok"] and len(body["pages"]) == 12
    assert cold < 150, f"cold preview took {cold:.0f} ms"
    calls = []
    real = app_mod._preview
    monkeypatch.setattr(app_mod, "_preview", lambda cfg: calls.append(1) or real(cfg))
    t = time.perf_counter()
    st, again = await preview(c, cfg)
    warm = (time.perf_counter() - t) * 1000
    assert again == body and not calls and warm < 50
    # editor-only changes hit the memo too; a real change does not
    await preview(c, {**cfg, "editor": {"other": 1}})
    assert not calls
    cfg2 = json.loads(json.dumps(cfg))
    cfg2["pages"][0]["widgets"][0]["label"] = "Changed"
    await preview(c, cfg2)
    assert len(calls) == 1
    print(f"\npreview 12 pages: cold {cold:.1f} ms, memo {warm:.1f} ms")


async def test_memo_is_bounded(c):
    for i in range(app_mod.PREVIEW_MEMO_MAX + 5):
        await preview(c, {**CONFIG, "devices": [{**CONFIG["devices"][0], "settle_ms": 100 + i}]})
    assert len(c.app[app_mod.MEMO_KEY]) == app_mod.PREVIEW_MEMO_MAX


# ---------------------------------------------------------------- metering
async def test_page_and_total_usage_match_the_compiler(c):
    cfg = big()
    _, body = await preview(c, cfg)
    comp = compile_config(parse_data(cfg))
    for p, pu, raw in zip(body["pages"], comp.usage["pages"], comp.pages, strict=True):
        assert (p["bytes"], p["limit"], p["widgets"], p["widget_limit"]) == (
            len(raw),
            MAX_BYTES,
            10,
            MAX_WIDGETS,
        )
        assert p["bytes"] == pu["bytes"] and p["ok"] and p["error"] is None
        assert p["page"]["widgets"] and p["id"].startswith("p")
    u = body["usage"]
    assert u["json_bytes"] == len(
        json.dumps(comp.hub_json, separators=(",", ":"), ensure_ascii=False)
    )
    assert u["json_max"] == LIMITS["MAX_JSON"] and u["pool_max"] == LIMITS["POOL_WORDS"]
    assert u["keys"] == len(comp.hub_json["keys"]) and u["keys_max"] == LIMITS["MAX_KEYS"]
    assert u["soft_keys"] == 120 and u["commands"] == 240 and u["inputs"] == 24
    assert body["hash"] == comp.config_hash


async def test_over_budget_page_reports_bytes_and_stays_missing(c):
    cfg = json.loads(json.dumps(CONFIG))
    ws = [{"type": "label", "x": i % 8, "y": i // 8, "label": "L" * 22} for i in range(10)]
    cfg["pages"] = [cfg["pages"][0], {"id": "fat", "cols": 8, "rows": 4, "widgets": ws}]
    cfg["pages"][0]["widgets"][1] = {"type": "label", "x": 1, "y": 0, "label": "ok"}
    cfg["pages"][0]["widgets"][0]["action"] = {"activity": "off"}
    _, body = await preview(c, cfg)
    fat = body["pages"][1]
    want = encoded_size([Widget("label", i, 0, 0, 1, 1, "L" * 22) for i in range(10)])
    assert want > MAX_BYTES
    assert (
        not fat["ok"] and fat["bytes"] == want and "bytes" in fat["error"] and fat["page"] is None
    )
    assert body["pages"][0]["ok"] and body["missing_pages"] == [{"index": 1, "id": "fat"}]
    assert not body["ok"] and body["errors"][0]["path"] == ["pages", 1]
    # and apply stays strict
    await put_draft(c, cfg)
    assert (await c.post("/api/v1/config/apply", json={}, headers=H)).status == 422


# ---------------------------------------------------------------- error tolerance
async def test_invalid_draft_still_returns_the_pages_that_compile(c):
    cfg = big()
    cfg["devices"][0]["id"] = "BAD ID"  # not a page problem: errors elsewhere
    cfg["pages"][3]["widgets"][0]["x"] = 99  # a page problem
    cfg["pages"][5]["widgets"].append({"type": "tile", "id": "t"})  # tile without entity
    st, body = await preview(c, cfg)
    assert st == 200 and body["ok"] is False and body["hash"] is None and body["usage"] is None
    assert [m["index"] for m in body["missing_pages"]] == [3, 5]
    good = [p for p in body["pages"] if p["ok"]]
    assert len(good) == 10 and all(p["page"]["widgets"] for p in good)
    bad = body["pages"][3]
    assert bad["page"] is None and bad["error"] and bad["widgets"] == 10
    paths = [e["path"][:2] for e in body["errors"]]
    assert ["pages", 3] in paths and ["pages", 5] in paths and ["devices", 0] in paths
    assert (await c.post("/api/v1/config/validate", json={"config": cfg}, headers=H)).status == 200


async def test_preview_without_pages_or_garbage_pages(c):
    st, body = await preview(c, {})
    assert st == 200 and body["ok"] and body["pages"] == []
    st, body = await preview(c, {"pages": ["junk", 5, {"id": "p"}]})
    assert st == 200 and not body["ok"] and len(body["pages"]) == 3
    assert all(not p["ok"] for p in body["pages"]) and body["missing_pages"][0]["index"] == 0


def test_validate_pages_is_shape_only():
    cfg = {
        "pages": [
            {
                "id": "p",
                "cols": 2,
                "rows": 2,
                "widgets": [
                    {
                        "type": "button",
                        "id": "b",
                        "x": 0,
                        "y": 0,
                        "action": {"device": "ghost", "command": "x"},
                    }
                ],
            }
        ]
    }
    ((model, errs),) = validate_pages(cfg)
    assert errs == [] and model["id"] == "p"  # dangling refs are not this function's business


# ---------------------------------------------------------------- editor metadata
EDITOR = {"layers": {"tv": {"who": "denon", "order": [1, 2, 3]}}, "v": 1}


async def test_editor_metadata_is_kept_but_never_leaves_the_add_on(aiohttp_client, tmp_path):
    app = create_app(tmp_path, auth=NoAuth(), clock=clock, bundle_token=TOKEN, bundle_networks=None)
    c = await aiohttp_client(app)
    cfg = {**CONFIG, "editor": EDITOR}
    assert (await put_draft(c, cfg)).status == 200
    assert (await c.post("/api/v1/config/apply", json={}, headers=H)).status == 200
    full = await (await c.get("/api/v1/config?full=1")).json()
    assert full["applied"]["config"]["editor"] == EDITOR  # preserved in the store
    b1 = await (await c.get("/api/v1/bundle", headers=BEARER)).json()
    assert "editor" not in b1["config"] and b1["config"] == CONFIG
    # editor-only edit: new applied revision in the add-on, same bundle rev (no hub push)
    await put_draft(c, {**CONFIG, "editor": {"v": 2}})
    assert (await c.post("/api/v1/config/apply", json={}, headers=H)).status == 200
    b2 = await (await c.get("/api/v1/bundle", headers=BEARER)).json()
    assert b2["rev"] == b1["rev"]
    st = await (await c.get("/api/v1/config/status")).json()
    assert st["applied_rev"] == b1["rev"]
    assert (await (await c.get("/api/v1/hubs")).json())["hubs"][0]["applied_rev"] == b1["rev"]
    # a config change moves the rev
    await put_draft(
        c, {**CONFIG, "devices": [{**CONFIG["devices"][0], "settle_ms": 777}], "editor": {"v": 2}}
    )
    await c.post("/api/v1/config/apply", json={}, headers=H)
    assert (await (await c.get("/api/v1/bundle", headers=BEARER)).json())["rev"] != b1["rev"]
    # export keeps it
    text = await (await c.get("/api/v1/config/export_yaml?source=applied")).text()
    assert "editor:" in text
    hy = {**H, "Content-Type": "text/yaml"}
    assert (await c.post("/api/v1/config/import_yaml", data=text, headers=hy)).status == 200
    assert (await (await c.get("/api/v1/config")).json())["draft"]["config"]["editor"] == {"v": 2}


def test_compiler_ignores_editor_and_integration_accepts_it():
    from custom_components.ha_harmony.addon_link import compile_source

    plain = compile_config(parse_data(CONFIG))
    with_editor = compile_config(parse_data({**CONFIG, "editor": EDITOR}))
    assert with_editor == plain and with_editor.config_hash == plain.config_hash
    assert compile_source({**CONFIG, "editor": EDITOR}, "h").config_hash == plain.config_hash


@pytest.mark.parametrize(
    "editor",
    [
        [],
        "x",
        {"a": {"b": {"c": {"d": {"e": {"f": {"g": {"h": 1}}}}}}}},  # deeper than 8
        {"k": "x" * 2001},
        {"k" * 101: 1},
        {f"k{i}": "y" * 1900 for i in range(40)},  # over 64 KB
        {"l": list(range(6000))},  # too many nodes
        {"f": float("nan")},
    ],
)
async def test_editor_bounds(c, editor):
    r = await c.post(
        "/api/v1/config/validate",
        data=json.dumps({"config": {"editor": editor}}),
        headers={**H, "Content-Type": "application/json"},
    )
    if r.status == 200:
        body = await r.json()
        assert body["ok"] is False and body["errors"][0]["path"][0] == "editor"
    else:
        assert r.status == 400  # refused earlier by the request bounds (JSON NaN, etc.)


# ---------------------------------------------------------------- key map
async def test_keymap_layers_and_inheritance(c):
    cfg = {
        "devices": [
            {
                "id": "tv",
                "commands": {"vol": {"proto": "nec", "addr": 1, "cmd": 1}},
                "inputs": {"hdmi1": {"proto": "nec", "addr": 1, "cmd": 2}},
            },
        ],
        "activities": [
            {
                "id": "watch",
                "name": "Watch",
                "devices": [{"device": "tv"}],
                "keys": {"volume_up": {"device": "tv", "command": "vol"}, "menu": {"none": True}},
            },
            {"id": "bare", "name": "Bare", "devices": [{"device": "tv"}]},
        ],
        "keys": {
            "menu": {"activity": "watch"},
            "exit": {"activity": "off"},
            "volume_up": {"event": "vol_ha"},
            "VolumeDown": {"device": "tv", "input": "hdmi1"},
        },
    }
    st = await c.post("/api/v1/config/keymap", json={"config": cfg}, headers=H)
    body = await st.json()
    assert st.status == 200 and body["ok"]
    dflt = {r["key"]: r for r in body["default"]}
    assert dflt["menu"]["source"] == "default" and dflt["menu"]["action"] == {
        "kind": "activity",
        "activity": "watch",
        "label": "Start watch",
    }
    assert dflt["exit"]["action"]["label"] == "Power off"
    assert dflt["volume_up"]["action"]["kind"] == "ha" and dflt["play"]["source"] == "none"
    assert dflt["play"]["action"] is None
    watch = {r["key"]: r for r in body["activities"][0]["keys"]}
    assert watch["volume_up"]["source"] == "activity"
    assert watch["volume_up"]["action"] == {
        "kind": "device",
        "device": "tv",
        "command": "vol",
        "label": "tv: vol",
    }
    assert (
        watch["menu"]["source"] == "activity" and watch["menu"]["action"]["kind"] == "none"
    )  # overrides default
    assert watch["exit"]["source"] == "default"  # inherited
    assert watch["play"]["source"] == "none"
    bare = {r["key"]: r for r in body["activities"][1]["keys"]}
    assert bare["menu"]["source"] == "default" and bare["volume_up"]["source"] == "default"
    assert len(body["default"]) == len(watch) == 35 and body["activities"][0]["id"] == "watch"
    assert [r["code"] for r in body["default"]] == sorted(r["code"] for r in body["default"])
    vd = dflt["volume_down"]  # a stock key name resolves to the same key
    assert vd["source"] == "default" and vd["action"]["input"] == "hdmi1"


async def test_keymap_needs_a_valid_config_and_uses_the_draft_by_default(c):
    r = await c.post(
        "/api/v1/config/keymap", json={"config": {"devices": [{"id": "BAD ID"}]}}, headers=H
    )
    assert r.status == 422 and (await r.json())["errors"]
    assert (await c.post("/api/v1/config/keymap", json={}, headers=H)).status == 409
    await put_draft(c)
    r = await c.post("/api/v1/config/keymap", json={}, headers=H)
    assert r.status == 200 and len((await r.json())["default"]) == 35
