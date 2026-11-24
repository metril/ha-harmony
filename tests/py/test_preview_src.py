"""The preview tells the editor which config widget each compiled widget came from (`src`)."""

import copy
import json
from pathlib import Path

from harmony_addon.app import _preview

BASE = json.loads((Path(__file__).parent / "fixtures" / "large_config.json").read_text())
ACT = {"device": "tv", "command": "up"}


def btn(i, cell, label=None, action=None):
    return {
        "type": "button",
        "id": f"b{i}",
        "x": (cell % 3) * 4,
        "y": cell // 3,
        "w": 4,
        "h": 1,
        "label": label or f"B{i}",
        "action": action or ACT,
    }


def preview_page(widgets, layout=None):
    c = copy.deepcopy(BASE)
    page = {"id": "big", "title": "Big", "cols": 12, "rows": 9, "widgets": widgets}
    if layout:
        page["layout"] = layout
    c["pages"] = [page]
    out = _preview(c)
    assert not out["errors"], out["errors"]
    return out["pages"][0]["wire_pages"]


def check_src(wires, widgets):
    seen = []
    for w in wires:
        src = [x["src"] for x in w["page"]["widgets"]]
        assert src == sorted(src)  # each screen lists its widgets in config order
        for x in w["page"]["widgets"]:
            assert x["label"] == widgets[x["src"]]["label"]
        seen += src
    assert sorted(seen) == list(range(len(widgets)))


def test_src_is_the_config_index_after_a_reorder():
    ws = [btn(i, i + 1) for i in range(23)] + [btn(23, 0)]  # the last widget sits top-left
    wires = preview_page(ws)
    assert len(wires) > 1
    check_src(wires, ws)
    assert 23 in [x["src"] for x in wires[0]["page"]["widgets"]]  # it moved to screen 1
    assert 11 not in [x["src"] for x in wires[0]["page"]["widgets"]]  # pushing B11 to screen 2


def test_src_survives_all_off_promotion():
    off = btn(23, 23, "All Off", {"activity": "off"})
    ws = [btn(i, i) for i in range(23)] + [off]
    wires = preview_page(ws)
    assert len(wires) > 1
    check_src(wires, ws)
    assert 23 in [x["src"] for x in wires[0]["page"]["widgets"]]


def test_src_on_a_single_screen_fixed_reorder():
    ws = [btn(2, 0), btn(1, 1), btn(0, 2)]
    wires = preview_page(ws, "fixed")
    assert len(wires) == 1
    check_src(wires, ws)
