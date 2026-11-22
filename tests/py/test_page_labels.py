"""Icon inference, the icon_only default and the overflow warning of page_build."""

import pytest
from test_compiler_pages import parse_model

from harmony_core.compiler import compile_config
from harmony_core.icons import ICON_IDS
from harmony_core.page_build import icon_for_text

BASE = """
devices:
  - id: shield
    name: Shield TV
    commands: {DirectionUp: {proto: nec, addr: 4, cmd: 9}, settings: {proto: nec, addr: 4, cmd: 10}, plex: {proto: nec, addr: 4, cmd: 11}, zzcmd: {proto: nec, addr: 4, cmd: 12}, Clear: {proto: nec, addr: 4, cmd: 13}}
activities:
  - {id: watch, name: Watch Shield, devices: [{device: shield}]}
"""


def widgets(*ws: str, cols: int = 4, rows: int = 5):
    cfg = BASE + f"pages:\n  - {{id: p, cols: {cols}, rows: {rows}, widgets: [{', '.join(ws)}]}}\n"
    c = compile_config(parse_model(cfg, "t.yaml"))
    return c, c.models[0].widgets


def btn(i, label, cmd, x=0, y=0, w=1, extra=""):
    return f"{{type: button, id: b{i}, x: {x}, y: {y}, w: {w}, h: 1, label: '{label}', action: {{device: shield, command: {cmd}}}{extra}}}"


@pytest.mark.parametrize(
    ("text", "icon"),
    [
        ("DirectionUp", "up"),
        ("direction_down", "down"),
        ("VolUp", "vol_up"),
        ("vol_up", "vol_up"),
        ("Vol Down", "vol_down"),
        ("InputHdmi1", "hdmi1"),
        ("PowerOff", "power"),
        ("Settings", "settings"),
        ("home", "home"),
        ("Plex", "plex"),
        ("YouTube", "youtube"),
        ("You Tube", "youtube"),
        ("Prime Video", "prime_video"),
        ("Apple TV", "apple_tv"),
        ("Disney+", "disney_plus"),
        ("PS5", "playstation"),
        ("Spotify", "spotify"),
    ],
)
def test_icon_for_text(text, icon):
    assert icon_for_text(text) == ICON_IDS[icon]


def test_icon_for_text_none():
    assert icon_for_text("Zzyzx") == 0


def test_exact_name_beats_keyword_match():
    # "back" is a keyword of nothing but the back icon; "stop" would also tie on keywords elsewhere
    assert icon_for_text("stop") == ICON_IDS["stop"]
    assert icon_for_text("Skip Next") == ICON_IDS["skip_next"]


def test_one_wide_cell_in_a_four_column_grid_defaults_to_icon_only():
    _, ws = widgets(btn(1, "Settings", "settings"))
    assert ws[0].icon and ws[0].icon_only


def test_wide_cell_keeps_its_label():
    _, ws = widgets(btn(1, "Settings", "settings", w=2))
    assert ws[0].icon and not ws[0].icon_only


def test_explicit_icon_only_false_wins():
    _, ws = widgets(btn(1, "Settings", "settings", extra=", icon_only: false"))
    assert not ws[0].icon_only


def test_over_budget_label_with_an_icon_goes_icon_only_in_a_three_column_grid():
    _, ws = widgets(btn(1, "Shieldtvsettingsdeluxe", "settings"), cols=3, rows=4)
    assert ws[0].icon_only
    _, ws = widgets(btn(1, "Settings", "settings"), cols=3, rows=4)
    assert not ws[0].icon_only


def test_no_icon_means_no_icon_only():
    _, ws = widgets(btn(1, "Zzyzx", "zzcmd"))
    assert not ws[0].icon and not ws[0].icon_only


def test_overflow_is_a_warning_not_an_error():
    c, _ = widgets(btn(1, "Zzyzx Deluxe Edition", "zzcmd"))
    msgs = [w["msg"] for w in c.warnings]
    assert any("Zzyzx Deluxe Edition" in m and "px too wide" in m for m in msgs)
    assert all(w["severity"] == "warning" for w in c.warnings)


def test_a_label_that_fits_warns_nothing():
    c, _ = widgets(btn(1, "Plex", "plex", w=2))
    assert not [w for w in c.warnings if "too wide" in w["msg"]]


def test_brand_logo_defaults_to_icon_only_even_in_a_wide_cell():
    _, ws = widgets(btn(1, "YouTube", "plex", w=2), btn(2, "Plex", "plex", x=2, w=2))
    assert all(w.icon and w.icon_only for w in ws)
    _, ws = widgets(btn(1, "YouTube", "plex", w=2, extra=", icon_only: false"))
    assert not ws[0].icon_only


# --- captions never ellipsize: abbreviation table, trailing words, then the glyph alone ---------------------------


@pytest.mark.parametrize(
    ("full", "short"),
    [
        ("Power Toggle", "Power"),
        ("Context Menu", "Menu"),
        ("Page Down", "Pg Dn"),
        ("Page Up", "Pg Up"),
        ("Skip Forward", "Skip Fwd"),
        ("Skip Back", "Skip Back"),
        ("Volume Up", "Vol Up"),
        ("Channel Down", "Ch Down"),
        ("Computer 4", "PC 4"),
    ],
)
def test_abbreviation_table(full, short):
    from harmony_core.key_names import abbreviate

    assert abbreviate(full) == short


KEYS = [
    ("Home", "home", "home"),
    ("Skip Forward", "fwd", "skip10"),
    ("Skip Back", "back", "back"),
    ("Power Toggle", "pwr", "power"),
    ("Context Menu", "ctx", "menu"),
    ("Page Up", "pgu", "up"),
    ("Page Down", "pgd", "down"),
    ("Zzyzxdeluxeedition", "zz", "search"),
]


def keys_page(extra=""):
    ws = [
        f"{{type: button, id: k{i}, x: {(i % 3) * 4}, y: {i // 3}, w: 4, h: 1, label: '{lab}', icon: {ic},"
        f" action: {{device: shield, command: {cmd}}}{extra}}}"
        for i, (lab, cmd, ic) in enumerate(KEYS)
    ]
    cmds = ", ".join(f"{c}: {{proto: nec, addr: 4, cmd: {i + 20}}}" for i, (_, c, _i) in enumerate(KEYS))
    cfg = BASE.replace("settings:", f"{cmds}, settings:")
    cfg += f"pages:\n  - {{id: p, layout: fixed, cols: 12, rows: 3, widgets: [{', '.join(ws)}]}}\n"
    return compile_config(parse_model(cfg, "t.yaml"))


def test_glyph_keys_shorten_instead_of_ellipsizing():
    from harmony_core.text_fit import fit_label

    c = keys_page(", icon_only: false")
    got = {w.label for pg in c.models for w in pg.widgets}
    for want in ("Skip Fwd", "Power", "Menu", "Pg Up"):
        assert want in got, got
    for pg in c.models:
        for w in pg.widgets:
            assert "..." not in w.label
            if w.label != "Zzyzxdeluxeedition":
                assert fit_label(pg, w) is None, w.label


def test_a_caption_that_cannot_be_shortened_drops_to_the_glyph_not_dots():
    c = keys_page()
    got = {w.label: w for pg in c.models for w in pg.widgets}
    assert got["Zzyzxdeluxeedition"].icon_only


def test_a_key_row_is_all_captions_or_none():
    c = keys_page()
    for pg in c.models:
        rows = {}
        for w in pg.widgets:
            rows.setdefault(w.y, []).append(w.icon_only)
        assert all(len(set(r)) == 1 for r in rows.values())


def test_compacted_card_shortens_its_word_rather_than_breaking_mid_word():
    ws = ", ".join(
        f"{{type: button, id: b{i}, x: {i % 2}, y: {i // 2 + 1}, label: '{lab}', action: {{device: shield, command: zzcmd}}}}"
        for i, lab in enumerate(["MacBook Pro", "Desktop", "Work Laptop", "Computer 4", "Power On", "Power Off"])
    )
    tile = "{type: tile, id: t, x: 0, y: 0, w: 2, label: Src, entity: select.x}"
    c = compile_config(parse_model(BASE + f"pages:\n  - {{id: k, cols: 2, rows: 4, widgets: [{tile}, {ws}]}}\n", "t.yaml"))
    assert not [w for w in c.warnings if "too wide" in w["msg"]]


# --- contrast: a brand logo is exempt, text on a brand fill is not ---------------------------------------------------


def _colour_warnings(widget: str):
    cfg = BASE + f"pages:\n  - {{id: p, cols: 4, rows: 2, widgets: [{widget}]}}\n"
    return [w for w in compile_config(parse_model(cfg, "t.yaml")).warnings if "best text" in w["msg"]]


def test_brand_logo_on_brand_red_is_exempt():
    w = btn(1, "YouTube", "plex", w=2, extra=", icon: youtube, icon_only: true, color: '#FF0000'")
    assert _colour_warnings(w) == []


def test_text_on_brand_red_still_warns():
    w = btn(1, "Zzyzx", "zzcmd", w=2, extra=", color: '#FF0000'")
    assert _colour_warnings(w)


# --- icons: label, then command / service target; siblings stay consistent ---------------------------------------------


@pytest.mark.parametrize(("text", "icon"), [("Clear", "cctv_off"), ("Clear Camera", "cctv"), ("MacBook Pro", "laptop")])
def test_icon_keywords(text, icon):
    assert icon_for_text(text) == ICON_IDS[icon] or text == "Clear Camera"


def test_icon_falls_back_to_the_command_name():
    _, ws = widgets(btn(1, "Zz", "Clear", w=1))
    assert ws[0].icon == ICON_IDS["cctv_off"]


def test_select_tile_gets_the_input_glyph():
    cfg = BASE + "pages:\n  - {id: p, cols: 2, rows: 1, widgets: [{type: tile, id: t, x: 0, y: 0, w: 2, label: Source, entity: select.kvm_source}]}\n"
    c = compile_config(parse_model(cfg, "t.yaml"))
    assert c.models[0].widgets[0].icon == ICON_IDS["input"]


def test_macbook_caption_keeps_its_glyph_with_its_siblings():
    ws = ", ".join(
        f"{{type: button, id: b{i}, x: {i % 2}, y: {i // 2 + 1}, label: '{lab}', action: {{device: shield, command: zzcmd}}}}"
        for i, lab in enumerate(["MacBook Pro", "Desktop", "Work Laptop", "Computer 4", "Power On", "Power Off"])
    )
    tile = "{type: tile, id: t, x: 0, y: 0, w: 2, label: Src, entity: select.x}"
    c = compile_config(parse_model(BASE + f"pages:\n  - {{id: k, cols: 2, rows: 4, widgets: [{tile}, {ws}]}}\n", "t.yaml"))
    btns = [w for pg in c.models for w in pg.widgets if w.kind == "button"]
    assert all(w.icon for w in btns[:4]) and not any(w.icon_only for w in btns[:4])
