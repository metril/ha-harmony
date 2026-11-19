import importlib.util
import json
import re
from pathlib import Path

import pytest

from harmony_core import theme_math as tm
from harmony_core.themes import PRESETS, THEMES, swatch, theme_by_id

ROOT = Path(__file__).resolve().parents[2]
VEC = json.loads((ROOT / "tests" / "vectors" / "themes.json").read_text())
NAMES = ["red", "orange", "amber", "yellow", "lime", "green", "teal", "cyan", "blue", "indigo", "purple", "pink"]


def _gen():
    spec = importlib.util.spec_from_file_location("gen_themes", ROOT / "tools" / "gen_themes.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _float_contrast(a: int, b: int) -> float:
    def lum(c: int) -> float:
        out = 0.0
        for w, s in ((0.2126, 16), (0.7152, 8), (0.0722, 0)):
            v = ((c >> s) & 255) / 255
            out += w * (v / 12.92 if v <= 0.03928 else ((v + 0.055) / 1.055) ** 2.4)
        return out

    hi, lo = sorted((lum(a), lum(b)), reverse=True)
    return (hi + 0.05) / (lo + 0.05)


def test_generated_files_fresh():
    for path, text in _gen().outputs().items():
        assert path.read_text() == text, f"{path.name} stale: run `uv run python tools/gen_themes.py`"


def test_ids_unique_and_below_custom():
    ids = [t.id for t in THEMES]
    assert len(set(ids)) == len(ids) and max(ids) < 7
    assert [t.key for t in THEMES] == ["default", "midnight", "daylight", "high_contrast"]
    assert PRESETS["default"].name == "Cards" and theme_by_id(0) is PRESETS["default"] and theme_by_id(7) is None


def test_palette_order_is_wire_index():
    assert list(tm.PALETTE_NAMES) == NAMES
    for t in THEMES:
        assert [s.name for s in t.palette] == NAMES
        assert swatch(t.id, 1).name == "red" and swatch(t.id, 12).name == "pink"
        assert swatch(t.id, 0) is None and swatch(t.id, 13) is None


def test_default_equals_remote_table():
    # theme.h no longer holds colour literals: the remote's default is row 0 of the generated theme_presets.c
    c = (ROOT / "remote" / "app" / "theme_presets.c").read_text()
    row = re.search(r'\{0, "default", "[^"]*",\s*((?:0x[0-9A-Fa-f]{6},?\s*){13})', c)
    assert row
    vals = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{6})", row[1])]
    assert vals == list(PRESETS["default"].tokens.values())
    assert list(PRESETS["default"].tokens) == list(tm.TOKEN_NAMES)


@pytest.mark.parametrize("t", THEMES, ids=lambda t: t.key)
def test_preset_rules(t):
    for v in [*t.tokens.values(), *(c for s in t.palette for c in (s.fill, s.ink, s.pressed))]:
        assert tm.is565(v), hex(v)
    assert tm.validate_theme(t.tokens, [s.fill for s in t.palette]) == []
    for a, s, num, den in tm.PAIRS:  # independent float check
        assert _float_contrast(t.tokens[a], t.tokens[s]) >= num / den - 0.02, (a, s)
    for s in t.palette:
        assert s.ink in (t.tokens["tx"], t.tokens["bg"])
        assert _float_contrast(s.fill, s.ink) >= 4.5 - 0.02, s.name
        assert _float_contrast(s.pressed, s.ink) >= 3.0 - 0.02, s.name
        assert s.ink == tm.ink(s.fill, t.tokens["tx"], t.tokens["bg"])
        assert s.pressed == tm.pressed(s.fill, s.ink)


def test_ink_tie_goes_to_tx_and_pressed_moves_toward_ink():
    assert tm.ink(0x808080, 0x123456, 0x123456) == 0x123456
    assert tm.pressed(0xF8F8F8, 0) == tm.snap565(0xBABABA)
    assert tm.snap565(0xFFFFFF) == 0xF8FCF8 and tm.snap565(0x040204) == 0x080408 and tm.snap565(0x030103) == 0 and tm.snap565(0x040004) == 0x080008


def test_lin_table_matches_vectors():
    assert list(tm.LIN) == VEC["lin"]
    assert tm.LIN[0] == 0 and tm.LIN[255] == 4095


def test_vectors():
    assert len(VEC["derive"]) >= 300
    for c, want in VEC["snap"]:
        assert tm.snap565(c) == want
    for a, b, want in VEC["contrast"]:
        assert tm.contrast_x100(a, b) == want
        assert abs(want / 100 - _float_contrast(a, b)) <= 0.03 * _float_contrast(a, b)
    for f, t, b, i, p in VEC["derive"]:
        assert (tm.ink(f, t, b), tm.pressed(f, i)) == (i, p)
    for f, b, want in VEC["outline"]:
        assert tm.needs_outline(f, b) is want
    pals = [s.fill for s in PRESETS["default"].palette]
    for c, want in VEC["nearest_palette"]["cases"]:
        assert tm.nearest_palette(c, pals) == want
    for v in VEC["validate"]:
        assert tm.validate_theme(v["tokens"], v["fills"]) == v["problems"]
        for p in v["problems"]:
            assert p["ratio_x100"] < p["min_x100"] and tm.is565(p["fix"])


def test_validate_fix_passes():
    bad = {**PRESETS["default"].tokens, "tx": PRESETS["default"].tokens["bg"], "onacc": 0x20C8B0}
    probs = tm.validate_theme(bad)
    assert {(p["text"], p["surface"]) for p in probs} >= {("tx", "bg"), ("onacc", "acc")}
    for p in probs:
        need = (9, 2) if p["min_x100"] == 450 else (3, 1)
        assert tm.meets(p["fix"], bad[p["surface"]], *need)


def test_wire_golden_and_roundtrip():
    d = PRESETS["default"]
    wire = tm.serialise_theme(d.tokens, [s.fill for s in d.palette])
    assert len(wire) == tm.WIRE_LEN == 99 and wire[0] == 1
    assert wire[:16].hex() == "0110c419262167320affffadb86bd126"
    assert wire.hex() == VEC["wire"][0]["hex"]
    toks, pal = tm.parse_theme(wire)
    assert toks == dict(d.tokens) and pal == [(s.fill, s.ink, s.pressed) for s in d.palette]
    for w in VEC["wire"]:
        t, p = tm.parse_theme(bytes.fromhex(w["hex"]))
        assert t == w["tokens"] and [x[0] for x in p] == w["fills"]
        assert tm.serialise_theme(t, [x[0] for x in p]).hex() == w["hex"]
    for bad in VEC["wire_bad"]:
        with pytest.raises(ValueError):
            tm.parse_theme(bytes.fromhex(bad))
    with pytest.raises(ValueError):
        tm.serialise_theme(d.tokens, [0] * 11)


def test_presets_serialise_through_one_function():
    for w, t in zip(VEC["wire"], THEMES, strict=False):
        assert w["key"] == t.key
        assert tm.serialise_theme(t.tokens, [s.fill for s in t.palette]).hex() == w["hex"]


def test_resolve_theme():
    r = tm.resolve_theme
    d = PRESETS["default"]
    toks, fills = r("default", {}, PRESETS)
    assert toks == dict(d.tokens) and fills == [s.fill for s in d.palette]
    users = {
        "mine": {"extends": "midnight", "tokens": {"acc": "#ff0000"}, "palette": {"1": {"fill": "#123456"}, 12: {"fill": "#FFFFFF"}}},
        "child": {"extends": "mine", "tokens": {"tx": "#f0f0f0"}},
    }
    toks, fills = r("child", users, PRESETS)
    assert toks["acc"] == 0xF80000 and toks["tx"] == 0xF0F0F0 and toks["bg"] == PRESETS["midnight"].tokens["bg"]
    assert fills[0] == tm.snap565(0x123456) and fills[11] == 0xF8FCF8 and fills[1] == PRESETS["midnight"].palette[1].fill

    def bad(name, themes, msg):
        with pytest.raises(tm.ThemeError, match=msg):
            r(name, themes, PRESETS)

    bad("midnight", {"midnight": {"extends": "default"}}, "reuse a preset")
    bad("x", {"x": {"extends": "default", "tokens": {"nope": "#000000"}}}, "unknown token")
    bad("x", {"x": {"extends": "default", "tokens": {"bg": "#12345"}}}, "bad colour")
    bad("x", {"x": {"extends": "default", "tokens": {"bg": 5}}}, "bad colour")
    bad("x", {"x": {"extends": "ghost"}}, "unknown theme")
    bad("x", {"x": {}}, "extends must name")
    bad("x", {"x": {"extends": "y"}, "y": {"extends": "x"}}, "circular")
    bad("x", {"x": {"extends": "x"}}, "circular")
    bad("x", {"x": {"extends": "default", "palette": {"13": {"fill": "#000000"}}}}, "palette index")
    bad("x", {"x": {"extends": "default", "palette": {"1": {"fill": "#000000", "ink": "#fff"}}}}, "want")
    chain = {f"t{i}": {"extends": f"t{i + 1}"} for i in range(5)} | {"t5": {"extends": "default"}}
    bad("t0", chain, "deeper than 4")
    ok = {f"t{i}": {"extends": f"t{i + 1}"} for i in range(3)} | {"t3": {"extends": "default"}}
    assert r("t0", ok, PRESETS)[0] == dict(d.tokens)  # depth 4 is allowed
