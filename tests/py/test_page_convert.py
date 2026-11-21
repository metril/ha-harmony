"""Hand-written grid pages compile to the FIXED layout unless they opt out with `layout: grid`."""

from types import SimpleNamespace

from test_compiler_pages import BASE, btn

from harmony_core.compiler import compile_config
from harmony_core.config_model import parse_model
from harmony_core.page_codec import decode_page
from harmony_core.text_fit import fit_label


def build(pages: str):
    c = compile_config(parse_model(BASE + pages, "t.yaml"))
    return c, [decode_page(b) for b in c.pages_v2]


def tile(i, x, y, w=1):
    return f"{{type: tile, id: {i}, x: {x}, y: {y}, w: {w}, label: T, entity: sensor.t}}"


def test_default_pages_are_fixed_and_grid_opts_out():
    ws = btn("a", 0, 0) + "," + btn("b", 1, 0)
    for extra, fixed in (("", True), ("layout: grid,", False)):
        _, ps = build(f"pages:\n  - {{id: p, {extra} cols: 2, rows: 2, widgets: [{ws}]}}\n")
        assert ps[0].fixed is fixed


def test_classes_by_cell_width_and_reading_order():
    ws = ",".join(
        [
            btn("full", 0, 0, "All", ", w: 4"),
            btn("h1", 0, 1, "Half", ", w: 2"),
            tile("t", 2, 1, 2),
            btn("q1", 0, 2, "A"),
            btn("q2", 1, 2, "B"),
        ]
    )
    _, (p,) = build(f"pages:\n  - {{id: p, cols: 4, rows: 3, widgets: [{ws}]}}\n")
    got = {w.label or w.kind: (w.x, w.y, w.w, w.shape) for w in p.widgets}
    assert got["All"] == (0, 0, 12, "row")  # full width: a row
    assert got["Half"][2:] == (6, "card") and got["T"][2] == 6  # halves: card and tile
    assert got["A"][2] == got["B"][2] == 3  # quarters


def test_ids_actions_and_power_off_on_page_one():
    ws = ",".join(
        [btn(f"b{i}", 0, i, f"Item {i}") for i in range(9)]
        + [btn("off", 0, 9, "Down", action="{activity: 'off'}")]
    )
    c, ps = build(f"pages:\n  - {{id: p, cols: 1, rows: 10, widgets: [{ws}]}}\n")
    assert len(ps) == 2 and (ps[0].seq_idx, ps[0].seq_cnt) == (1, 2)
    first = {c.key_names[w.id + 128] for w in ps[0].widgets if w.kind == "button"}
    assert "tap_off" in first


def test_labels_fit_their_class():
    ws = ",".join([btn("a", 0, 0, "Natural Light"), btn("b", 1, 0, "Tropical Twilight"), btn("c", 2, 0, "Long Label X")])
    _, (p,) = build(f"pages:\n  - {{id: p, cols: 3, rows: 1, widgets: [{ws}]}}\n")
    page = SimpleNamespace(cols=p.cols, rows=p.rows, fixed=True)
    assert all(fit_label(page, w) is None for w in p.widgets)
