"""Stream C: input normaliser, inputs derived from commands, nav icon inference, living-room pages."""

import copy
import json
from pathlib import Path

import pytest
from test_irdb_device import load

from harmony_core.compiler import CompileError, compile_config
from harmony_core.config_model import parse_model
from harmony_core.icons import ICON_IDS
from harmony_core.key_names import normalize_input
from harmony_core.page_build import MAX_PAGES, nav_icon
from harmony_irdb.device import build_device

ROOT = Path(__file__).resolve().parents[2]
LIVING = ROOT / "examples" / "living-room.yaml"
VEC = json.loads((ROOT / "tests" / "vectors" / "inputs.json").read_text())


@pytest.mark.parametrize(("name", "want"), VEC)
def test_normalize_input_vectors(name, want):
    assert normalize_input(name) == want


def test_inputs_derived_from_commands_when_the_archive_has_no_list():
    d, cs, path = load("Sony", "KV-32V42.json")
    d = {k: v for k, v in d.items() if k != "inputs"}
    cs = copy.deepcopy(cs)
    for cmd, new in zip(cs["commands"][:4], ("InputHdmi1", "Input HDMI 2", "InputAv", "InputTv"), strict=True):
        cmd["name"] = new
    b = build_device(d, cs, mfr="S", model_file="f.json", codeset_path=path, imported_at="x")
    assert list(b.device["inputs"]) == ["HDMI 1", "HDMI 2", "AV", "TV"]


def test_samsung_hdmi_codes_match_the_archive_keycodes():
    # archive "GoVideoO1 32 Bit" data 0xE0E0 <cmd bit-reversed> <~cmd bit-reversed>: wire order, LSB first
    rev = lambda b: int(f"{b:08b}"[::-1], 2)
    archive = {"HDMI 1": 0xE0E09768, "HDMI 2": 0xE0E07D82, "HDMI 3": 0xE0E043BC}
    model = parse_model(LIVING.read_text(), "living-room.yaml")
    tv = next(d for d in model["devices"] if d["id"] == "tv")
    assert rev((archive["HDMI 1"] >> 8) & 0xFF) == 233  # the existing, verified code
    for label, data in archive.items():
        assert tv["inputs"][label]["cmd"] == rev((data >> 8) & 0xFF)
        assert tv["inputs"][label]["addr"] == rev((data >> 24) & 0xFF) == 7
    assert "HDMI 4" not in tv["inputs"]  # no direct code in the archive entry for this model


def test_living_room_pages_fit_and_every_nav_row_has_a_glyph():
    model = parse_model(LIVING.read_text(), "living-room.yaml")
    c = compile_config(model)
    assert len(c.models) <= MAX_PAGES
    assert all(w.icon for p in c.models for w in p.widgets if w.role == "nav")
    ids = {p["id"] for p in model["pages"]}
    assert {"dev_tv", "dev_shield", "dev_shield_apps", "dev_speakers", "dev_speaker_power", "dev_kvm", "favorites"} <= ids
    fav = next(p for p in model["pages"] if p["id"] == "favorites")
    assert all(w.get("compact") and w["w"] == 6 for w in fav["widgets"])


def test_too_many_pages_is_a_clear_error():
    pages = "".join(f"  - {{id: p{i}, cols: 1, rows: 1, title: P{i}, widgets: []}}\n" for i in range(MAX_PAGES + 1))
    with pytest.raises(CompileError, match=r"too many pages: 33 \(the remote holds at most 32\)"):
        compile_config(parse_model("pages:\n" + pages, "t.yaml"))


def test_nav_icon_order_device_icon_brand_keyword():
    assert nav_icon("TV", {"tv": "tv"}, {"tv": "tv"}) == ICON_IDS["tv"]  # the device's configured icon
    assert nav_icon("Shield", {}, {}) == ICON_IDS["nvidia"]  # brand
    assert nav_icon("Speakers", {}, {}) == ICON_IDS["speaker"]  # name keyword
    assert nav_icon("Zzz Qqq", {}, {}) == 0
    assert nav_icon("Next", {}, {}) == 0  # chevron words: the row draws its own
