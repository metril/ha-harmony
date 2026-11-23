"""Preview (fast, memoised, error-tolerant, metered), key map view, and `editor:` metadata."""

import json
import time

import pytest
from addon_ws import get_bundle
from cfgapi import put_apply
from test_addon import CONFIG, H, clock

from harmony_addon import app as app_mod
from harmony_addon.app import create_app
from harmony_addon.auth import NoAuth
from harmony_core.compiler import compile_config
from harmony_core.config_model import parse_data, validate_pages
from harmony_core.hub_limits import CURRENT
from harmony_core.page_codec import MAX_BYTES, MAX_WIDGETS, Page, Widget, page_size

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


async def preview(c, config):
    r = await c.post("/api/v1/config/preview", json={"config": config}, headers=H)
    return r.status, await r.json()


# ---------------------------------------------------------------- speed and memo
async def test_preview_of_a_heavy_config_is_fast_and_memoised(c, monkeypatch):
    cfg = big()
    t = time.process_time()  # CPU time: a loaded host does not make the compile slower
    st, body = await preview(c, cfg)
    cold = (time.process_time() - t) * 1000
    assert st == 200 and body["ok"] and len(body["pages"]) == 12
    assert cold < 150, f"cold preview took {cold:.0f} ms"
    calls = []
    real = app_mod._preview
    monkeypatch.setattr(
        app_mod, "_preview", lambda cfg, limits: calls.append(1) or real(cfg, limits)
    )
    t = time.process_time()
    st, again = await preview(c, cfg)
    warm = (time.process_time() - t) * 1000
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
    for p, pu, raw, raw1 in zip(
        body["pages"], comp.usage["page_detail"], comp.pages_v2, comp.pages, strict=True
    ):
        assert p["bytes_v1"] == len(raw1) and p["wire_pages"][0]["bytes_v1"] == len(raw1)
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
    assert u["keys"] == len(comp.hub_json["keys"]) and u["keys_max"] == CURRENT.max_keys
    assert u["soft_keys"] == 120 and u["commands"] == 240 and u["inputs"] == 24
    assert body["hash"] == comp.config_hash


async def test_over_budget_page_reports_bytes_and_stays_missing(c):
    cfg = json.loads(json.dumps(CONFIG))
    ws = [{"type": "label", "x": i % 8, "y": i // 8, "label": "L" * 22} for i in range(10)]
    cfg["pages"] = [
        cfg["pages"][0],
        {"id": "fat", "layout": "grid", "cols": 8, "rows": 4, "widgets": ws},
    ]
    cfg["pages"][0]["widgets"][1] = {"type": "label", "x": 1, "y": 0, "label": "ok"}
    cfg["pages"][0]["widgets"][0]["action"] = {"activity": "off"}
    _, body = await preview(c, cfg)
    fat = body["pages"][1]
    probe = Page(0, 1, 1, tuple([Widget("label", i, 0, 0, 1, 1, "L" * 22) for i in range(10)]))
    want = page_size(probe, 2)
    assert want > MAX_BYTES
    assert not fat["ok"] and fat["bytes"] == want and fat["page"] is None
    assert "bytes" in fat["error"] and fat["bytes_v1"] == page_size(probe, 1)
    assert body["pages"][0]["ok"] and body["missing_pages"] == [{"index": 1, "id": "fat"}]
    assert not body["ok"] and body["errors"][0]["path"] == ["pages", 1]
    # and apply stays strict
    assert (await put_apply(c, cfg)).status == 422


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
    assert (await put_apply(c, cfg)).status == 200
    rev = await (await c.get("/api/v1/configs/main/revs/1")).json()
    assert rev["config"]["editor"] == EDITOR  # preserved in the store
    b1 = await get_bundle(c)
    assert "editor" not in b1["config"] and b1["config"] == CONFIG
    # editor-only edit: new applied revision in the add-on, same bundle rev (no hub push)
    assert (await put_apply(c, {**CONFIG, "editor": {"v": 2}})).status == 200
    b2 = await get_bundle(c)
    assert b2["rev"] == b1["rev"]
    assert (await get_bundle(c))["rev"] == b1["rev"]
    assert (await (await c.get("/api/v1/hubs")).json())["hubs"][0]["applied_rev"] == b1["rev"]
    # a config change moves the rev
    await put_apply(
        c, {**CONFIG, "devices": [{**CONFIG["devices"][0], "settle_ms": 777}], "editor": {"v": 2}}
    )
    assert (await get_bundle(c))["rev"] != b1["rev"]
    # export keeps it
    text = await (await c.get("/api/v1/configs/main/export_yaml?source=rev:3")).text()
    assert "editor:" in text
    hy = {**H, "Content-Type": "text/yaml"}
    assert (await c.post("/api/v1/configs/main/import_yaml", data=text, headers=hy)).status == 200
    assert (await (await c.get("/api/v1/configs/main")).json())["draft"]["config"]["editor"] == {
        "v": 2
    }


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
    assert dflt["exit"]["action"]["label"] == "Power Off"
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


async def test_keymap_needs_a_valid_config(c):
    r = await c.post(
        "/api/v1/config/keymap", json={"config": {"devices": [{"id": "BAD ID"}]}}, headers=H
    )
    assert r.status == 422 and (await r.json())["errors"]
    assert (await c.post("/api/v1/config/keymap", json={}, headers=H)).status == 400
    r = await c.post("/api/v1/config/keymap", json={"config": CONFIG}, headers=H)
    assert r.status == 200 and len((await r.json())["default"]) == 35


# ---------------------------------------------------------------- page format v2
def _list_cfg(n=40):
    base = json.loads(json.dumps(CONFIG))
    base["pages"] = [
        {
            "id": "home",
            "kind": "home",
            "title": "Home",
            "cols": 2,
            "rows": 2,
            "widgets": [{"type": "button", "x": 0, "y": 0, "label": "Next", "nav": "lst"}],
        },
        {
            "id": "lst",
            "title": "Scenes",
            "layout": "grid",
            "list": {"rows": 4},
            "widgets": [
                {
                    "type": "button",
                    "id": f"s{i}",
                    "label": f"Scene {i}",
                    "icon": "bulb",
                    "action": {"device": "tv", "command": "vol_up"},
                }
                for i in range(n)
            ],
        },
    ]
    return base


async def test_preview_carries_the_v2_model_and_both_byte_counts(c):
    _, body = await preview(c, _list_cfg())
    assert body["ok"], body["errors"]
    home, lst = body["pages"]
    m = home["page"]
    assert m["version"] == 2 and m["kind"] == "home" and m["title"] == "Home"
    nav = m["widgets"][0]
    assert (
        nav["role"] == "nav"
        and nav["bind"] == lst["wire_pages"][0]["page_id"]
        and nav.get("trailing")
        and not nav["icon"]
    )
    assert home["bytes"] > home["bytes_v1"] - 100 and home["limit"] == MAX_BYTES
    assert [w["page_id"] for w in home["wire_pages"]] == [0]
    # a 40-row list on 4-row pages becomes 10 wire pages, numbered 1/10 .. 10/10
    assert len(lst["wire_pages"]) == 10
    assert [w["seq"] for w in lst["wire_pages"]][:2] == [
        {"idx": 1, "cnt": 10},
        {"idx": 2, "cnt": 10},
    ]
    assert lst["page"] == lst["wire_pages"][0]["page"] and lst["ok"]
    assert lst["bytes"] == max(w["bytes"] for w in lst["wire_pages"])
    assert lst["widgets"] == 40
    comp = compile_config(parse_data(_list_cfg()))
    assert len(comp.pages) == len(comp.pages_v2) == 11 == len(body["pages"][1]["wire_pages"]) + 1


async def test_a_page_that_cannot_build_does_not_hide_the_others(c):
    cfg = _list_cfg()
    cfg["pages"].append(
        {
            "id": "ok2",
            "cols": 2,
            "rows": 1,
            "widgets": [{"type": "label", "x": 0, "y": 0, "label": "Hi"}],
        }
    )
    cfg["pages"][1]["widgets"] = cfg["pages"][1]["widgets"] * 5  # 200 rows: too many page groups
    _, body = await preview(c, cfg)
    assert not body["ok"] and body["hash"] is None
    paths = [e["path"] for e in body["errors"]]
    assert ["pages", 1] in paths
    assert [p["ok"] for p in body["pages"]] == [True, False, True]
    assert body["missing_pages"] == [{"index": 1, "id": "lst"}]


@pytest.mark.parametrize(
    ("edit", "path"),
    [
        (lambda p: p["widgets"][0].update(role="loud"), ["pages", 0, "widgets", 0, "role"]),
        (lambda p: p["widgets"][0].update(shape="blob"), ["pages", 0, "widgets", 0, "shape"]),
        (lambda p: p["widgets"][0].update(icon="nope"), ["pages", 0, "widgets", 0, "icon"]),
        (
            lambda p: p["widgets"][0].update(icon_only="yes"),
            ["pages", 0, "widgets", 0, "icon_only"],
        ),
        (lambda p: p["widgets"][0].update(nav="nowhere"), ["pages", 0, "widgets", 0, "nav"]),
        (lambda p: p.update(kind="weird"), ["pages", 0, "kind"]),
        (lambda p: p.update(activity="ghost"), ["pages", 0, "activity"]),
        (lambda p: p.update(back="x"), ["pages", 0, "back"]),
        (lambda p: p.update(title="x" * 200), ["pages", 0, "title"]),
        (lambda p: p.update(list={"rows": 99}), ["pages", 0, "list", "rows"]),
        (lambda p: p.update(list={"rows": 3}), ["pages", 0, "rows"]),
    ],
)
async def test_validate_reports_v2_keys_with_precise_paths(c, edit, path):
    cfg = json.loads(json.dumps(CONFIG))
    cfg["pages"] = [
        {
            "id": "p",
            "cols": 2,
            "rows": 2,
            "widgets": [
                {"type": "button", "x": 0, "y": 0, "label": "A", "nav": "next"},
                {"type": "label", "x": 1, "y": 0, "label": "L"},
            ],
        }
    ]
    cfg["pages"].append({"id": "q", "cols": 1, "rows": 1, "widgets": []})
    edit(cfg["pages"][0])
    r = await c.post("/api/v1/config/validate", json={"config": cfg}, headers=H)
    paths = [e["path"] for e in (await r.json())["errors"]]
    assert path in paths, paths


async def test_ref_errors_for_pages_are_all_collected(c):
    cfg = json.loads(json.dumps(CONFIG))
    cfg["pages"] = [
        {
            "id": "a",
            "kind": "home",
            "cols": 2,
            "rows": 1,
            "activity": "ghost",
            "widgets": [{"type": "button", "x": 0, "y": 0, "nav": "prev"}],
        },
        {
            "id": "b",
            "kind": "home",
            "cols": 2,
            "rows": 1,
            "widgets": [{"type": "button", "x": 0, "y": 0, "nav": "zzz"}],
        },
    ]
    r = await c.post("/api/v1/config/validate", json={"config": cfg}, headers=H)
    paths = [e["path"] for e in (await r.json())["errors"]]
    for want in (
        ["pages", 0, "activity"],
        ["pages", 0, "widgets", 0, "nav"],
        ["pages", 1, "kind"],
        ["pages", 1, "widgets", 0, "nav"],
    ):
        assert want in paths, paths


async def test_page_cap_is_in_usage_and_over_it_is_an_error(c):
    cfg = json.loads(json.dumps(CONFIG))
    mk = lambda i: {
        "id": f"p{i}",
        "cols": 1,
        "rows": 1,
        "widgets": [{"type": "label", "x": 0, "y": 0, "label": "x"}],
    }
    cfg["pages"] = [mk(i) for i in range(252)]
    _, body = await preview(c, cfg)
    assert body["ok"] and body["usage"]["pages"] == 252 and body["usage"]["page_limit"] == 252
    cfg["pages"].append(mk(252))
    _, body = await preview(c, cfg)
    assert not body["ok"] and any("at most 252" in e["msg"] for e in body["errors"])
    assert body["usage"] is None or body["usage"]["pages"] == 253


async def test_preview_survives_a_button_that_starts_an_unknown_activity(c):
    """The studio previews on every edit, so a dangling activity reference must be a validation
    error, not a 500."""
    cfg = {
        "devices": [
            {"id": "tv", "name": "TV", "commands": {"menu": {"proto": "nec", "addr": 1, "cmd": 1}}}
        ],
        "activities": [{"id": "watch", "name": "Watch", "devices": [{"device": "tv"}]}],
        "keys": {"menu": {"device": "nope", "command": "x"}},
        "pages": [
            {
                "id": "home",
                "cols": 2,
                "rows": 2,
                "widgets": [
                    {
                        "type": "button",
                        "id": "b",
                        "x": 0,
                        "y": 0,
                        "label": "Gone",
                        "action": {"activity": "gone"},
                    }
                ],
            }
        ],
    }
    st, body = await preview(c, cfg)
    assert st == 200 and body["ok"] is False and body["errors"]


async def test_preview_carries_widget_align_to_the_model(c):
    cfg = _list_cfg()
    cfg["pages"][0]["widgets"][0]["align"] = "right"
    _, body = await preview(c, cfg)
    assert body["ok"], body["errors"]
    assert body["pages"][0]["page"]["widgets"][0]["align"] == "right"
    bad = _list_cfg()
    bad["pages"][0]["widgets"][0]["align"] = "justify"
    _, body = await preview(c, bad)
    assert not body["ok"]
