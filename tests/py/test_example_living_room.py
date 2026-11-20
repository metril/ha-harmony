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
