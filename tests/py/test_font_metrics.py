"""proto/font_metrics.json (and its runtime copies) must match the baked fonts."""

import importlib.util
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location(
    "gen_font_metrics", ROOT / "tools" / "gen_font_metrics.py"
)
assert spec and spec.loader
gen = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gen)


def test_metrics_up_to_date():
    want = gen.render()
    for path in (gen.OUT, *gen.COPIES):
        assert path.read_text() == want, f"{path} is stale: run tools/gen_font_metrics.py"


def test_known_widths():
    m = gen.build()
    assert [m["b13"][str(c)] for c in (32, 65, 105)] == [3, 8, 3]
    assert set(m) == set(gen.FONTS)


def test_line_height_recorded():
    m = gen.build()
    assert m["b13"]["lh"] == 16
