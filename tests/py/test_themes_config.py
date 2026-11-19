"""Config model and compiler: widget colours, user themes, budget, gen seed, caps degrade, warnings."""

import hashlib
import json
from pathlib import Path

import pytest
from test_compiler_pages import BASE, btn, page

from harmony_core.compiler import CompileError, compile_config
from harmony_core.config_model import ConfigError, parse_data, parse_model, validate_all
from harmony_core.page_codec import decode_page
from harmony_core.theme_build import CAP_THEME_COLOR, CAP_THEME_CUSTOM, active_theme
from harmony_core.theme_math import PALETTE_NAMES, nearest_palette, serialise_theme, snap565
from harmony_core.themes import PRESETS

FIX = Path(__file__).parent / "fixtures"
SNAPPED = f"#{snap565(0x3355FF):06X}"
DUSK = "themes:\n  dusk: {extends: midnight, tokens: {ACC: '#FF8A3D'}, palette: {3: {fill: '#2E7D32'}}}\n"


def comp(pages: str, head: str = ""):
    return compile_config(parse_model(BASE + head + pages, "t.yaml"))


def one(*widgets: str, extra: str = ""):
    return page(", ".join(widgets), extra=extra)


def problems(text: str):
    return [(e.path, e.msg) for e in validate_all(BASE + text, "t.yaml")]


# ---------------------------------------------------------------- rules
def W(c):
    return btn("a", 0, 0, "A", f", color: {c}")


@pytest.mark.parametrize(
    ("text", "path", "needle"),
    [
        (
            one(W("teal2")),
            ["pages", 0, "widgets", 0, "color"],
            "unknown colour 'teal2'; one of: red, orange",
        ),
        (one(W("'#12'")), ["pages", 0, "widgets", 0, "color"], "bad colour '#12'; want #RRGGBB"),
        (one(W("'#GG0000'")), ["pages", 0, "widgets", 0, "color"], "bad colour"),
        (
            one(btn("a", 0, 0, "A", ", on_color: red")),
            ["pages", 0, "widgets", 0],
            "`on_color` belongs on a tile",
        ),
        (
            one(W("red")) + "theme: nope\n",
            ["theme"],
            "unknown theme 'nope'; presets: default, midnight",
        ),
        ("theme: dusk\n" + DUSK + one(), [], None),  # valid
        (
            "themes:\n  midnight: {extends: default}\n",
            ["themes", "midnight"],
            "reuses a preset name",
        ),
        (
            "themes:\n  t: {tokens: {zz: '#000000'}}\n",
            ["themes", "t", "tokens", "zz"],
            "unknown token 'zz'; one of: bg, s1",
        ),
        ("themes:\n  t: {extends: ghost}\n", ["themes", "t", "extends"], "unknown theme 'ghost'"),
        (
            "themes:\n  a: {extends: b}\n  b: {extends: a}\n",
            ["themes", "a", "extends"],
            "circular extends",
        ),
        (
            "themes:\n"
            + "".join(f"  t{i}: {{extends: t{i + 1}}}\n" for i in range(5))
            + "  t5: {extends: midnight}\n",
            ["themes", "t0", "extends"],
            "deeper than 4",
        ),
        (
            "themes:\n  t: {palette: {13: {fill: '#000000'}}}\n",
            ["themes", "t", "palette", 13],
            "palette index 13 must be 1..12",
        ),
        (
            "themes:\n  t: {palette: {0: {fill: '#000000'}}}\n",
            ["themes", "t", "palette", 0],
            "must be 1..12",
        ),
        (
            "themes:\n  t: {tokens: {acc: red}}\n",
            ["themes", "t", "tokens", "acc"],
            "bad colour 'red'",
        ),
    ],
)
def test_rule(text, path, needle):
    found = problems(text)
    if needle is None:
        assert found == []
        return
    assert any(p == path and needle in m for p, m in found), found


def test_each_rule_is_a_located_config_error():
    with pytest.raises(ConfigError) as ei:
        parse_model(BASE + one(W("teal2")), "/cfg/hub.yaml")
    assert str(ei.value).startswith("/cfg/hub.yaml") and "pages[0].widgets[0].color" in str(
        ei.value
    ).replace(" ", "")


# ---------------------------------------------------------------- compile
def widgets_of(c, i=0):
    return decode_page(c.pages_v2[i]).widgets


def test_palette_free_and_on_colour_bytes():
    c = comp(
        one(
            btn("a", 0, 0, "A", ", color: red"),
            btn("b", 1, 0, "B", ", color: '#3355ff'"),
            "{type: tile, id: t, x: 2, y: 0, label: T, entity: light.x, color: blue, on_color: amber}",
        )
    )
    a, b, t = widgets_of(c)
    assert (a.color, b.color, t.color, t.on_color) == (
        "red",
        SNAPPED,
        "blue",
        "amber",
    )  # free = RGB565
    assert c.models[0].theme == 0 and c.theme_id == 0 and c.theme_wire is None
    plain = comp(
        one(
            btn("a", 0, 0, "A"),
            btn("b", 1, 0, "B"),
            "{type: tile, id: t, x: 2, y: 0, label: T, entity: light.x}",
        )
    )
    assert (
        len(c.pages_v2[0]) - len(plain.pages_v2[0]) == 1 + 3 + 1
    )  # palette 1, free 3, tile blue+amber 1
    assert all(w.color is None for w in widgets_of(plain))


def test_preset_theme_id_and_user_theme_wire():
    mid = comp(one(btn("a", 0, 0, "A")), "theme: midnight\n")
    assert mid.theme_id == 1 and mid.theme_wire is None and mid.models[0].theme == 1
    c = comp(one(btn("a", 0, 0, "A")), "theme: dusk\n" + DUSK)
    assert c.theme_id == 7 and c.models[0].theme == 7 and c.theme_base == "midnight"
    assert c.theme_wire is not None and len(c.theme_wire) == 99
    assert decode_page(c.pages_v2[0]).theme == 7
    assert (
        comp(one(btn("a", 0, 0, "A")), "theme: default\n").pages_v2
        == comp(one(btn("a", 0, 0, "A"))).pages_v2
    )


GOLDEN = {
    "living_room": {
        "gen": 16,
        "n": 14,
        "v2": "b56e93c573d7b2a3046f22f90761b279d998445342b11196eab5ec4c41aace66",
        "v1": "85182975efecb3658c9bccb1da6c6004c525135cfe1af57701c1e03516c017c4",
        "hash": "16b320ed",
    },
    "large": {
        "gen": 100,
        "n": 19,
        "v2": "b760f2c4efe4ba32810149884b87b302128c67eb06ed469e785918514c79fd75",
        "v1": "75bb953efe52e2dad8127dc94116bce92c15f91cb46d3d1712ef9c7fa0da1fdc",
        "hash": "5f6cca72",
    },
}


@pytest.mark.parametrize("name", GOLDEN)
def test_uncoloured_configs_compile_byte_identical_with_the_same_gen(name):
    if name == "living_room":
        m = parse_model((FIX / "living_room.yaml").read_text(), "lr", FIX)
    else:
        m = parse_data(json.loads((FIX / "large_config.json").read_text()))
    c = compile_config(m)
    g = GOLDEN[name]
    assert (c.gen, len(c.pages_v2), c.config_hash) == (g["gen"], g["n"], g["hash"])
    assert hashlib.sha256(b"".join(c.pages_v2)).hexdigest() == g["v2"]
    assert hashlib.sha256(b"".join(c.pages)).hexdigest() == g["v1"]
    assert (
        c.for_caps(0).pages_v2 == c.pages_v2 and c.for_caps(12).gen == c.gen
    )  # nothing to degrade


def test_gen_changes_iff_theme_or_colour_changes():
    w = btn("a", 0, 0, "A")
    wc = btn("a", 0, 0, "A", ", color: red")
    base = comp(one(w))
    assert comp(one(w)).gen == base.gen
    assert comp(one(wc)).gen != base.gen
    assert comp(one(w), "theme: midnight\n").gen != base.gen
    u1 = comp(one(w), "theme: dusk\n" + DUSK)
    u2 = comp(one(w), "theme: dusk\n" + DUSK.replace("FF8A3D", "FF6A3D"))
    assert u1.theme_wire != u2.theme_wire
    # the 99 bytes are in the seed: same pages, different wire -> different gen across many edits
    gens = {
        comp(one(w), "theme: dusk\n" + DUSK.replace("FF8A3D", f"FF8A{n:02X}")).gen
        for n in range(0, 256, 8)
    }
    assert len(gens) > 4
    # a theme that is defined but not selected changes nothing
    assert comp(one(w), DUSK).gen == base.gen


def test_over_budget_message_has_a_cost_breakdown():
    labels = "x" * 6
    for n in range(8, 32):
        ws = [btn(f"b{i}", i % 8, i // 8, labels) for i in range(n)]
        cs = [
            btn(f"b{i}", i % 8, i // 8, labels, ", color: '#3355FF'" if i < 3 else ", color: red")
            for i in range(n)
        ]
        try:
            comp(page(", ".join(ws), cols=8, rows=4))
        except CompileError:
            continue
        try:
            comp(page(", ".join(cs), cols=8, rows=4))
        except CompileError as exc:
            msg = str(exc)
            assert (
                "bytes over (3 custom colours = 6 B, " in msg and f"{n} colour bytes = {n} B" in msg
            )
            assert "use palette colours, shorten labels" in msg
            return
    pytest.fail("no size sits between plain and coloured")


def test_list_pages_split_by_bytes_with_colours():
    rows = ", ".join(
        f"{{type: button, id: r{i}, label: 'row label long', color: '#112233', action: {{none: true}}}}"
        for i in range(30)
    )
    c = comp(f"pages:\n  - {{id: l, cols: 1, list: {{rows: 16}}, widgets: [{rows}]}}\n")
    assert len(c.pages_v2) >= 2 and all(len(p) <= 251 for p in c.pages_v2)


# ---------------------------------------------------------------- degrade by caps
RICH = one(
    btn("a", 0, 0, "Netflix", ", color: red"),
    btn("b", 1, 0, "Custom", ", color: '#3355FF'"),
    "{type: tile, id: t, x: 2, y: 0, label: Lamp, entity: light.x, on_color: '#3355FF'}",
)


def test_degrade_matrix():
    c = comp(RICH, "theme: dusk\n" + DUSK)
    full = c.for_caps(CAP_THEME_COLOR | CAP_THEME_CUSTOM)
    assert (
        full.pages_v2 == c.pages_v2
        and full.theme_wire == c.theme_wire
        and full.theme_id == 7
        and not full.notices
    )

    mid = c.for_caps(CAP_THEME_COLOR)  # palette + presets only
    fills = [s.fill for s in PRESETS["midnight"].palette]
    blue = PALETTE_NAMES[nearest_palette(0x3355FF, fills) - 1]
    ws = decode_page(mid.pages_v2[0]).widgets
    assert (ws[0].color, ws[1].color, ws[2].on_color) == ("red", blue, blue)
    assert mid.theme_id == 1 and decode_page(mid.pages_v2[0]).theme == 1 and mid.theme_wire is None
    assert mid.gen != c.gen
    msgs = " | ".join(n["msg"] for n in mid.notices)
    assert (
        "theme 'dusk' replaced by its base preset 'midnight'" in msgs and "'Custom', 'Lamp'" in msgs
    )
    assert "Netflix" not in msgs  # a palette colour survives

    none = c.for_caps(0)  # role colours
    ws = decode_page(none.pages_v2[0]).widgets
    assert all(w.color is None and w.on_color is None for w in ws)
    assert (
        none.theme_id == 0 and decode_page(none.pages_v2[0]).theme == 0 and none.theme_wire is None
    )
    assert none.pages_v2 != mid.pages_v2
    assert any("theme 'dusk' not shown" in n["msg"] for n in none.notices)
    assert [n["page"] for n in none.notices if n["scope"] == "widgets"] == ["p"]
    assert len(none.pages_v2[0]) < len(mid.pages_v2[0]) < len(c.pages_v2[0])
    assert none.pages_v2_b64 and c.for_caps(0) is none  # cached per tier


def test_preset_theme_degrades_to_default_without_colour_caps():
    c = comp(one(btn("a", 0, 0, "A", ", color: red")), "theme: daylight\n")
    assert c.for_caps(CAP_THEME_COLOR).pages_v2 == c.pages_v2 and c.theme_id == 2
    low = c.for_caps(0)
    assert low.theme_id == 0 and decode_page(low.pages_v2[0]).widgets[0].color is None


# ---------------------------------------------------------------- warnings
def test_warnings_for_contrast_and_free_colours():
    bad = "themes:\n  murky: {extends: default, tokens: {tx: '#404850'}}\ntheme: murky\n"
    c = comp(one(btn("a", 0, 0, "A", ", color: '#808080'")), bad)
    msgs = [w["msg"] for w in c.warnings]
    assert all(w["severity"] == "warning" for w in c.warnings)
    assert any(
        "theme 'murky': tx on bg contrast is" in m and "nearest passing colour #" in m for m in msgs
    )
    assert comp(one(btn("a", 0, 0, "A", ", color: red"))).warnings == []
    dusk = [
        w["msg"] for w in comp(one(btn("a", 0, 0, "A")), DUSK).warnings
    ]  # unused themes still warn
    assert dusk and all(m.startswith("theme 'dusk': ink on amber") for m in dusk)


def test_free_colour_with_weak_ink_warns_with_a_fix():
    # mid grey cannot reach 4.5:1 with either the dark or the light text of the default theme
    c = comp(one(btn("a", 0, 0, "A", ", color: '#777777'")))
    (w,) = [w for w in c.warnings if w["path"][-1] == "color"]
    assert w["path"] == ["pages", 0, "widgets", 0, "color"]
    assert "#777777 has only" in w["msg"] and "nearest passing colour #" in w["msg"]
    # the colour still compiles: warnings never block
    assert widgets_of(c)[0].color == f"#{snap565(0x777777):06X}"


def test_serialised_wire_matches_theme_math():
    c = comp(one(btn("a", 0, 0, "A")), "theme: dusk\n" + DUSK)
    m = parse_model(BASE + "theme: dusk\n" + DUSK)
    at = active_theme(m)
    assert c.theme_wire == serialise_theme(dict(at.tokens), list(at.fills))
    assert dict(at.tokens)["acc"] != PRESETS["midnight"].tokens["acc"]


def test_token_key_casing_vector_shared_with_the_editor():
    from harmony_core.theme_math import resolve_theme

    vec = json.loads((Path(__file__).parents[1] / "vectors" / "theme_casing.json").read_text())
    model = parse_data({"themes": vec["themes"], "theme": "t"})
    tokens, _ = resolve_theme("t", model["themes"], PRESETS)
    assert {k: tokens[k] for k in vec["expect"]} == vec["expect"]
