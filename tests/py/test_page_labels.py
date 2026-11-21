"""Icon inference, the icon_only default and the overflow warning of page_build."""

import pytest

from harmony_core.compiler import compile_config
from harmony_core.config_model import parse_model
from harmony_core.icons import ICON_IDS
from harmony_core.page_build import icon_for_text

BASE = """
devices:
  - id: shield
    name: Shield TV
    commands: {DirectionUp: {proto: nec, addr: 4, cmd: 9}, settings: {proto: nec, addr: 4, cmd: 10}, plex: {proto: nec, addr: 4, cmd: 11}}
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
        ("InputHdmi1", "input"),
        ("PowerOff", "power"),
        ("Settings", "settings"),
        ("home", "home"),
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
    _, ws = widgets(btn(1, "Shield TV Settings", "settings"), cols=3, rows=4)
    assert ws[0].icon_only
    _, ws = widgets(btn(1, "Settings", "settings"), cols=3, rows=4)
    assert not ws[0].icon_only


def test_no_icon_means_no_icon_only():
    _, ws = widgets(btn(1, "Zzyzx", "plex"))
    assert not ws[0].icon and not ws[0].icon_only


def test_overflow_is_a_warning_not_an_error():
    c, _ = widgets(btn(1, "Plexamp Deluxe Edition", "plex", w=2))
    msgs = [w["msg"] for w in c.warnings]
    assert any("Plexamp Deluxe Edition" in m and "px too wide" in m for m in msgs)
    assert all(w["severity"] == "warning" for w in c.warnings)


def test_a_label_that_fits_warns_nothing():
    c, _ = widgets(btn(1, "Plex", "plex", w=2))
    assert not [w for w in c.warnings if "too wide" in w["msg"]]
