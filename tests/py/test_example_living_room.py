"""examples/living-room.yaml passes schema validation and compiles (with its pages)."""

from pathlib import Path

from harmony_core.compiler import compile_config
from harmony_core.config_model import parse_model

PATH = Path(__file__).resolve().parents[2] / "examples" / "living-room.yaml"


def test_example_validates_and_compiles():
    model = parse_model(PATH.read_text(), "living-room.yaml")
    c = compile_config(model)
    assert {a["id"] for a in model["activities"]} == {"shield_tv", "watch_tv", "desk"}
    assert c.pages


def test_devices_favorites_navigate_to_device_pages_and_home_has_nav():
    model = parse_model(PATH.read_text(), "living-room.yaml")
    pages = {p["id"]: p for p in model["pages"]}
    assert pages["devices"]["kind"] == "favorites" and pages["home"]["kind"] == "home"
    assert pages["home"]["title"] == "Home"
    navs = lambda p: {w["label"]: w["nav"] for w in pages[p]["widgets"] if w.get("nav")}
    assert navs("devices") == {
        "TV": "dev_tv",
        "Shield": "dev_shield",
        "Shield Apps": "dev_shield_apps",
        "Speakers": "dev_speakers",
        "Speaker Power": "dev_speaker_power",
        "KVM": "dev_kvm",
    }
    assert navs("home") == {"Lights": "lights", "Scenes": "scenes", "Blinds": "blinds", "Favorites": "favorites"}
    c = compile_config(model)
    nav = [w for p in c.models for w in p.widgets if w.role == "nav"]
    assert nav and all(w.trailing and w.shape == "row" and w.icon for w in nav)
