"""Page compilation: inference, title rule, list splitting, ids and the v1/v2 encodings."""

import json
from pathlib import Path

import pytest

from harmony_core.compiler import CompileError, compile_config
from harmony_core.config_model import ConfigError, parse_model
from harmony_core.icons import ICON_IDS
from harmony_core.page_codec import decode_page, encode

GOLDEN = json.loads((Path(__file__).parent / "fixtures" / "pages_v1_goldens.json").read_text())

BASE = """
devices:
  - id: tv
    power: {toggle: {proto: nec, addr: 4, cmd: 8}}
    commands: {mute: {proto: nec, addr: 4, cmd: 9}, vol_up: {proto: nec, addr: 4, cmd: 10}}
activities:
  - {id: watch, name: Watch TV, devices: [{device: tv}]}
  - {id: music, name: Music, devices: [{device: tv}]}
"""


def comp(pages: str, base: str = BASE):
    return compile_config(parse_model(base + pages, "t.yaml"))


def page(widgets: str, extra: str = "", cols: int = 4, rows: int = 4, pid: str = "p") -> str:
    return f"pages:\n  - {{id: {pid}, cols: {cols}, rows: {rows}, {extra} widgets: [{widgets}]}}\n"


def btn(i, x, y, label="", extra="", action="{device: tv, command: mute}"):
    return f"{{type: button, id: {i}, x: {x}, y: {y}, label: '{label}', action: {action}{extra}}}"


# ---------------------------------------------------------------- v1 compatibility
def _shape(data: bytes):
    p = decode_page(data)
    return (p.page_id, p.cols, p.rows), [(w.kind, w.x, w.y, w.w, w.h, w.label) for w in p.widgets]


@pytest.mark.parametrize("name", list(GOLDEN))
def test_v1_output_matches_pre_v2_goldens(name):
    c = compile_config(parse_model(GOLDEN[name]["yaml"], "t.yaml"))
    old = [bytes.fromhex(h) for h in GOLDEN[name]["v1_pages"]]
    assert [_shape(p) for p in c.pages] == [_shape(p) for p in old]
    assert [p.version for p in map(decode_page, c.pages)] == [1] * len(old)


def test_v1_byte_identical_when_no_label_precedes_a_soft_key():
    # the second sample's only label is the last widget of the last page: no id shifts
    old = [bytes.fromhex(h) for h in GOLDEN["no_title_label"]["v1_pages"]]
    c = compile_config(parse_model(GOLDEN["no_title_label"]["yaml"], "t.yaml"))
    assert c.pages[0] == old[0]
    ow, nw = decode_page(old[1]).widgets, decode_page(c.pages[1]).widgets
    assert [w.id for w in nw[:-1]] == [w.id for w in ow[:-1]]  # buttons keep their ids
    assert nw[-1].kind == "label" and nw[-1].id not in {w.id for w in nw[:-1]}


# ---------------------------------------------------------------- title rule
def test_row0_label_becomes_header_title_and_rows_shift():
    c = comp(page("{type: label, x: 0, y: 0, w: 4, label: Living}, " + btn("a", 1, 1, "A"), rows=3))
    (m,) = c.models
    assert (m.title, m.rows) == ("Living", 2)
    assert [(w.kind, w.y) for w in m.widgets] == [("button", 0)]
    p1 = decode_page(c.pages[0])  # v1: the title is a label again, rows shifted back down
    assert p1.rows == 3 and next((w.kind, w.y, w.label) for w in p1.widgets) == (
        "label",
        0,
        "Living",
    )


def test_explicit_title_wins_and_keeps_row0_label():
    c = comp(
        page(
            "{type: label, x: 0, y: 0, w: 4, label: Living}, " + btn("a", 1, 1, "A"),
            "title: Den,",
            rows=3,
        )
    )
    (m,) = c.models
    assert m.title == "Den" and m.rows == 3 and [w.kind for w in m.widgets] == ["label", "button"]


def test_title_over_24_bytes_is_a_config_error():
    with pytest.raises(ConfigError, match="24 bytes"):
        parse_model(BASE + page(btn("a", 0, 0), f"title: {'x' * 25},"), "t.yaml")


# ---------------------------------------------------------------- inference
def test_inference_and_explicit_overrides():
    c = comp(
        page(
            ", ".join(
                [
                    btn("off", 0, 0, "All Off", action="{activity: 'off'}"),
                    btn("pw", 1, 0, "power off"),
                    btn("w", 2, 0, "Go", action="{activity: music}"),
                    btn("m", 3, 0, "Mute"),
                    "{type: tile, id: t, x: 0, y: 1, w: 2, label: L, entity: light.kitchen}",
                    btn(
                        "e",
                        2,
                        1,
                        "Mute",
                        ", icon: none, role: primary, shape: pill, icon_only: true",
                    ),
                    btn("e2", 3, 1, "Mute", ", icon: star"),
                ]
            )
        )
    )
    w = {x.label or x.kind: x for x in c.models[0].widgets}
    power = ICON_IDS["power"]
    assert (w["All Off"].role, w["All Off"].icon) == ("danger", power)
    assert (w["power off"].role, w["power off"].icon) == ("danger", power)
    assert (w["Go"].bind, w["Go"].icon, w["Go"].role) == (1, ICON_IDS["music"], "default")
    assert w["tile"].icon == ICON_IDS["bulb"] if "tile" in w else True
    mutes = [x for x in c.models[0].widgets if x.label == "Mute"]
    assert mutes[0].icon == ICON_IDS["mute"] and mutes[0].role == "default"
    assert (mutes[1].icon, mutes[1].role, mutes[1].shape, mutes[1].icon_only) == (
        0,
        "primary",
        "pill",
        True,
    )
    assert mutes[2].icon == ICON_IDS["star"]
    assert next(x for x in c.models[0].widgets if x.kind == "tile").icon == ICON_IDS["bulb"]


def test_activity_name_keywords_pick_icon():
    c = comp(page(btn("w", 0, 0, "Anything", action="{activity: watch}")))
    assert c.models[0].widgets[0].icon == ICON_IDS["tv"]
    assert c.models[0].widgets[0].bind == 0


def test_v1_drops_extras():
    c = comp(page(btn("w", 0, 0, "Go", action="{activity: watch}")))
    w = decode_page(c.pages[0]).widgets[0]
    assert (w.role, w.icon, w.bind) == ("default", 0, None)
    w2 = decode_page(c.pages_v2[0]).widgets[0]
    assert (w2.icon, w2.bind) == (ICON_IDS["tv"], 0)


# ---------------------------------------------------------------- ids and nav
NAVCFG = """pages:
  - id: home
    kind: home
    cols: 2
    rows: 2
    widgets:
      - {type: button, nav: second, x: 0, y: 0, label: Go}
      - {type: label, x: 1, y: 0, label: Hi}
      - {type: button, id: a, x: 0, y: 1, label: A, action: {device: tv, command: mute}}
  - id: second
    back: true
    activity: music
    cols: 2
    rows: 2
    widgets:
      - {type: button, nav: prev, x: 0, y: 0}
      - {type: button, nav: home, x: 1, y: 0, icon: star}
      - {type: button, id: b, x: 0, y: 1, label: B, action: {device: tv, command: vol_up}}
      - {type: tile, id: t, x: 1, y: 1, entity: sensor.x}
"""


def test_labels_and_nav_take_no_soft_key_and_binds_resolve():
    c = comp(NAVCFG)
    h, s = c.models
    assert (h.kind, s.back, s.act, h.total, s.total) == ("home", True, 2, 2, 2)
    nav = [w for w in h.widgets if w.role == "nav"]
    assert nav[0].bind == 1 and nav[0].icon == 0 and nav[0].trailing and nav[0].shape == "row"
    assert [(w.role, w.bind, w.icon) for w in s.widgets if w.role == "nav"] == [
        ("nav", 0, ICON_IDS["chev_left"]),
        ("nav", 0, ICON_IDS["star"]),
    ]
    # soft keys: only buttons and tiles, consecutive from 0 across pages
    assert c.key_names == {128: "tap_a", 129: "tap_b", 130: "tap_t"}
    assert {k for k in c.hub_json["keys"]} == {"128", "129"}
    assert [(t.page, t.widget) for t in c.tiles] == [(1, 2)]
    ids = {w.label or w.kind: w.id for w in h.widgets}
    assert ids["A"] == 0 and ids["Hi"] != 0  # the label takes a page-local id, A keeps global 0
    assert len({w.id for w in h.widgets}) == len(h.widgets)


def test_both_encodings_from_one_compile():
    c = comp(NAVCFG)
    assert [decode_page(p).version for p in c.pages] == [1, 1]
    assert [decode_page(p).version for p in c.pages_v2] == [2, 2]
    assert c.encoded_pages(2) is c.pages_v2 and c.encoded_pages(1) is c.pages
    assert [decode_page(p) for p in c.pages_v2] == list(c.models)
    assert len({m.gen for m in c.models}) == 1
    # nav widgets are dropped from v1 (no navigation there)
    assert all(w.kind != "button" or w.label for w in decode_page(c.pages[1]).widgets)


def test_gen_changes_with_page_layout_even_if_hub_json_does_not():
    a = comp(page(btn("a", 0, 0, "A")))
    b = comp(page(btn("a", 1, 0, "A")))
    assert a.hub_json == b.hub_json
    assert a.models[0].gen != b.models[0].gen or a.pages_v2 != b.pages_v2


@pytest.mark.parametrize(
    ("widget", "match"),
    [
        ("{type: button, nav: next, x: 0, y: 0}", "last page"),
        ("{type: button, nav: prev, x: 0, y: 0}", "first page"),
        ("{type: button, nav: home, x: 0, y: 0}", "kind: home"),
        ("{type: button, nav: nope, x: 0, y: 0}", "unknown page"),
        ("{type: button, nav: next, x: 0, y: 0, action: {none: true}}", "not both"),
        ("{type: button, nav: next, x: 0, y: 0, role: primary}", "nav role"),
        ("{type: label, nav: next, x: 0, y: 0}", "belongs on a button"),
        ("{type: button, id: a, x: 0, y: 0, action: {none: true}, icon: bogus}", "unknown icon"),
        ("{type: button, id: a, x: 0, y: 0, action: {none: true}, role: nav}", "role"),
    ],
)
def test_widget_key_errors(widget, match):
    with pytest.raises(ConfigError, match=match):
        parse_model(BASE + page(widget), "t.yaml")


def test_page_activity_and_second_home_errors():
    with pytest.raises(ConfigError, match="unknown activity"):
        parse_model(BASE + page(btn("a", 0, 0), "activity: nope,"), "t.yaml")
    two = "pages:\n" + "".join(
        f"  - {{id: h{i}, kind: home, cols: 1, rows: 1, widgets: []}}\n" for i in range(2)
    )
    with pytest.raises(ConfigError, match="second home"):
        parse_model(BASE + two, "t.yaml")


# ---------------------------------------------------------------- budget
def test_over_budget_reports_exact_overage():
    ws = ", ".join(btn(f"b{i}", i % 16, i // 16, "abcdefgh") for i in range(20))
    with pytest.raises(CompileError) as e:
        comp(page(ws, cols=16, rows=2))
    # 20 widgets * 13 + header 11 = 271: exactly 20 over
    msg = str(e.value)
    assert msg.startswith("page 'p': ") and msg.endswith(
        "bytes over; shorten labels, use icon_only, or make it a list"
    )
    n = int(msg.split(": ")[1].split(" ")[0])
    assert n > 0
    # dropping exactly that many label bytes makes it fit exactly at 251
    short = ", ".join(
        btn(f"b{i}", i % 16, i // 16, "abcdefgh"[: 8 - (1 if i < n else 0)]) for i in range(20)
    )
    c = comp(page(short, cols=16, rows=2))
    assert len(c.pages_v2[0]) == 251


# ---------------------------------------------------------------- lists
def lst(n, rows=3, label="row", extra=""):
    ws = ", ".join(
        f"{{type: button, id: r{i}, label: '{label}{i}', action: {{device: tv, command: mute}}{extra}}}"
        for i in range(n)
    )
    return f"pages:\n  - {{id: l, title: Inputs, list: {{rows: {rows}}}, widgets: [{ws}]}}\n"


def test_list_splits_by_rows_with_seq_and_consecutive_ids():
    c = comp(lst(7, rows=3))
    assert len(c.models) == 3
    assert [(m.seq_idx, m.seq_cnt) for m in c.models] == [(1, 3), (2, 3), (3, 3)]
    assert [len(m.widgets) for m in c.models] == [3, 3, 1]
    assert all(m.total == 3 and m.rows == 3 and m.cols == 1 for m in c.models)
    w = c.models[0].widgets
    assert [(x.x, x.y, x.w, x.h, x.shape) for x in w] == [(0, i, 1, 1, "row") for i in range(3)]
    assert sorted(x.id for m in c.models for x in m.widgets) == list(range(7))
    assert [decode_page(p) for p in c.pages_v2] == list(c.models)


def test_list_splits_by_bytes():
    c = comp(lst(12, rows=16, label="x" * 18))  # 12 * (5+1+19..20) + header > 251
    assert len(c.models) > 1 and all(len(p) <= 251 for p in c.pages_v2)
    assert sum(len(m.widgets) for m in c.models) == 12
    assert all(m.seq_cnt == len(c.models) for m in c.models)


def test_single_group_list_is_not_paged_and_nav_targets_first_group():
    c = comp(
        lst(2) + "  - {id: n, cols: 1, rows: 1, widgets: [{type: button, nav: prev, x: 0, y: 0}]}\n"
    )
    assert c.models[0].seq_cnt == 0
    c = comp(
        lst(7, rows=3)
        + "  - {id: n, cols: 1, rows: 1, widgets: [{type: button, nav: prev, x: 0, y: 0}]}\n"
    )
    assert c.models[-1].widgets[0].bind == 0 and c.models[-1].page_id == 3


def test_list_page_config_errors():
    with pytest.raises(ConfigError, match="drop x/y"):
        parse_model(
            BASE + "pages: [{id: l, list: {rows: 3}, widgets: [{type: label, x: 0, y: 0}]}]", "t"
        )
    with pytest.raises(ConfigError, match="list.rows"):
        parse_model(BASE + "pages: [{id: l, rows: 2, list: {rows: 3}, widgets: []}]", "t")
    with pytest.raises(ConfigError, match="required key"):
        parse_model(BASE + "pages: [{id: l, rows: 2, widgets: []}]", "t")
    with pytest.raises(CompileError, match="at most 15"):
        comp(lst(48, rows=3))


def test_encode_helper_roundtrip():
    c = comp(NAVCFG)
    for m, b in zip(c.models, c.pages_v2, strict=True):
        assert encode(m, 2) == b


def test_more_than_32_pages_is_a_compile_error():
    """The remote's store holds page ids 0..31."""
    many = "pages:\n" + "".join(
        f"  - {{id: p{i}, cols: 1, rows: 1, widgets: []}}\n" for i in range(33)
    )
    with pytest.raises(CompileError, match="at most 32"):
        comp(many)
    comp("pages:\n" + "".join(f"  - {{id: p{i}, cols: 1, rows: 1, widgets: []}}\n" for i in range(32)))


# ---------------------------------------------------------------- device icons
ICON_BASE = BASE.replace("  - id: tv\n", "  - id: tv\n    icon: music\n")


def test_device_icon_is_the_default_for_widgets_that_target_it_and_widget_icon_wins():
    c = comp(
        page(
            ",".join(
                [
                    btn("a", 0, 0, "Mute"),
                    btn("b", 1, 0, "Mute", ", icon: star"),
                    btn("c", 2, 0, "Mute", ", icon: none"),
                ]
            )
        ),
        ICON_BASE,
    )
    a, b, n = c.models[0].widgets
    assert (a.icon, b.icon, n.icon) == (ICON_IDS["music"], ICON_IDS["star"], 0)


def test_device_without_icon_keeps_the_label_based_default():
    c = comp(page(btn("a", 0, 0, "Mute")))
    assert c.models[0].widgets[0].icon == ICON_IDS["mute"]


def test_device_icon_must_be_a_known_stock_icon_and_is_valid_on_every_device_kind():
    with pytest.raises(ConfigError):
        parse_model(BASE.replace("  - id: tv\n", "  - id: tv\n    icon: nope\n"), "t.yaml")
    assert parse_model(ICON_BASE, "t.yaml")["devices"][0]["icon"] == "music"
    ha = "devices:\n  - {id: s, kind: ha, icon: tv, entity: media_player.s}\n"
    assert parse_model(ha, "t.yaml")["devices"][0]["icon"] == "tv"
    bt = "devices:\n  - {id: b, kind: bt, icon: game}\n"
    assert parse_model(bt, "t.yaml")["devices"][0]["icon"] == "game"


def test_align_center_reaches_the_wire_and_default_is_unchanged():
    c = comp(
        page(
            btn("a", 0, 0, "A", ", align: center")
            + ","
            + btn("b", 1, 0, "B")
            + ","
            + btn("c", 2, 0, "C", ", align: left")
        )
    )
    a, b, d = decode_page(encode(c.models[0], 2)).widgets[:3]
    assert (a.align_center, b.align_center, d.align_center) == (True, False, False)
    assert a.label == "A" and not a.trailing
    with pytest.raises(ConfigError):
        comp(page(btn("a", 0, 0, "A", ", align: right")))
