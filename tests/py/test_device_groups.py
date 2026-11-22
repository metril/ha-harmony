"""Device `group`: config validation and the living-room example's merged Shield page."""

from pathlib import Path

import pytest

from harmony_core.compiler import compile_config
from harmony_core.config_model import ConfigError, parse_model

EX = Path(__file__).resolve().parents[2] / "examples" / "living-room.yaml"
BASE = """
devices:
  - {id: a, kind: bt, name: A, group: box}
  - {id: b, kind: ha, name: B, group: box, entity: media_player.x}
  - {id: c, name: C, group: box, commands: {Mute: {proto: nec, addr: 1, cmd: 2}}}
"""


def test_group_is_valid_on_every_device_kind_and_groups_section_overrides_name_and_icon():
    m = parse_model(BASE + "groups: {box: {name: The Box, icon: nvidia}}\n")
    assert [d["group"] for d in m["devices"]] == ["box"] * 3
    assert m["groups"] == {"box": {"name": "The Box", "icon": "nvidia"}}
    assert "groups" not in parse_model(BASE)


@pytest.mark.parametrize(
    "text",
    [
        BASE.replace("group: box", "group: Not A Slug", 1),
        BASE + "groups: {box: {icon: no_such_icon}}\n",
        BASE + "groups: {Bad Id: {name: X}}\n",
        BASE + "groups: {box: {title: X}}\n",
    ],
)
def test_bad_group_config_is_rejected(text):
    with pytest.raises(ConfigError):
        parse_model(text)


def test_example_groups_the_shield_into_one_row_and_one_page():
    m = parse_model(EX.read_text(), "living-room.yaml")
    devs = {d["id"]: d for d in m["devices"]}
    assert devs["shield"]["group"] == devs["shield_ha"]["group"] == "shield"
    pages = {p["id"]: p for p in m["pages"]}
    assert "dev_shield_apps" not in pages
    rows = [w["label"] for w in pages["devices"]["widgets"]]
    assert rows.count("Shield") == 1 and "Shield Apps" not in rows
    used = {w["action"]["device"] for w in pages["dev_shield"]["widgets"] if "action" in w}
    assert used == {"shield", "shield_ha"}


def test_app_buttons_on_the_merged_page_still_bind_to_the_apps_entity():
    m = parse_model(EX.read_text(), "living-room.yaml")
    c = compile_config(m)
    page = next(i for i, p in enumerate(c.models) if p.title == "Shield")
    mine = [b for b in c.apps if b.page == page]
    assert {"com.plexapp.android", "org.smarttube.stable"} <= {b.app_ids[0] for b in mine}
    assert {b.entity_id for b in mine} == {"media_player.nvidia_shield_tv"}


def test_every_configured_widget_reaches_some_compiled_page():
    """No widget is dropped by the split or the wire budget."""
    m = parse_model(EX.read_text(), "living-room.yaml")
    c = compile_config(m)
    # the compiler may shorten labels, never drop a widget
    assert sum(len(p.widgets) for p in c.models) >= sum(len(p["widgets"]) for p in m["pages"])
    shield = {w.label for p in c.models if p.title == "Shield" for w in p.widgets}
    assert {"Play/Pause", "Stop", "Garage Cam", "All Commands", "Plex", "Power On"} <= shield


def test_template_style_buttons_without_app_id_bind_by_label_on_a_merged_page():
    text = (
        BASE
        + """
activities:
  - {id: act, name: Act, devices: [{device: b}], media: {apps_entity: media_player.x}}
pages:
  - id: dev_box
    title: Box
    layout: fixed
    cols: 12
    rows: 2
    widgets:
      - {type: button, id: k, x: 0, y: 0, w: 4, label: Back, action: {device: a, command: Back}}
      - {type: button, id: p, x: 0, y: 1, w: 6, shape: card, label: Plex, icon: plex, action: {device: b, command: Plex}}
"""
    )
    m = parse_model(text.replace("kind: ha, name: B, group: box, entity: media_player.x}", "kind: ha, name: B, group: box, entity: media_player.x, commands: {Plex: {ha: {service: script.turn_on, target: {entity_id: script.p}}}}}"))
    c = compile_config(m)
    assert [(b.app_ids[0], b.entity_id) for b in c.apps] == [("com.plexapp.android", "media_player.x")]
