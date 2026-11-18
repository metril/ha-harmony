"""Home Assistant actions in harmony_core: validation, compile-time split, inheritance, devices."""

import json

import pytest

from harmony_core.compiler import compile_config
from harmony_core.config_model import ConfigError, parse_model, validate_all
from harmony_core.keymap import resolve_keymaps

BASE = """
devices:
  - id: tv
    power: {on: {proto: nec, addr: 4, cmd: 1}, off: {proto: nec, addr: 4, cmd: 2}}
    inputs: {hdmi1: {proto: nec, addr: 4, cmd: 10}}
    commands: {vol_up: {proto: nec, addr: 4, cmd: 16, repeats: 3}}
  - id: shield
    kind: ha
    entity: media_player.shield
    settle_ms: 700
    power:
      on: {ha: {service: remote.turn_on, target: {entity_id: remote.shield}}}
      off: {ha: {service: remote.turn_off, target: {entity_id: remote.shield}}}
    inputs:
      home: {ha: {service: remote.send_command, target: {entity_id: remote.shield}, data: {command: HOME}}}
    commands:
      ok: {ha: {service: remote.send_command, target: {entity_id: remote.shield}, data: {command: DPAD_CENTER}}}
      down: {ha: {service: remote.send_command, target: {entity_id: remote.shield}, data: {command: DPAD_DOWN}, repeat: true}}
activities:
  - id: tv_on
    name: TV
    devices: [{device: tv, input: hdmi1}]
    keys: {select: {device: tv, command: vol_up}}
  - id: stream
    name: Stream
    devices: [{device: tv, input: hdmi1}, {device: shield, input: home}]
    steps: [{ha: {service: light.turn_off, target: {entity_id: light.room}}}]
    off: [{ha: {service: scene.turn_on, target: {entity_id: scene.bye}}}]
    keys:
      select: {device: shield, command: ok}
      left: {ha: {service: light.toggle, target: {entity_id: light.lamp}}}
keys:
  select: {device: tv, command: vol_up}
  left: {proto: nec, addr: 4, cmd: 3}
  down: {device: shield, command: down}
  up: {ha: {service: light.toggle, target: {entity_id: [light.a, light.b]}}}
  volume_up: {device: tv, command: vol_up}
"""


def model(text=BASE):
    return parse_model(text, "t.yaml")


def err(text):
    with pytest.raises(ConfigError) as exc:
        parse_model(text, "t.yaml")
    return str(exc.value)


def ha_key(body: str) -> str:
    return f"keys:\n  up: {{ha: {body}}}\n"


@pytest.mark.parametrize(
    "body,msg",
    [
        ("{service: shell_command.x, target: {entity_id: light.a}}", "not allowed"),
        ("{service: homeassistant.toggle, target: {entity_id: light.a}}", "not allowed"),
        ("{service: light, target: {entity_id: light.a}}", "domain.service"),
        ("{service: Light.Toggle, target: {entity_id: light.a}}", "domain.service"),
        ("{service: light.toggle}", "target"),
        ("{service: light.toggle, target: {area_id: x}}", "target"),
        ("{service: light.toggle, target: {entity_id: nope}}", "entity id"),
        ("{service: light.toggle, target: {entity_id: 'light.a'}, extra: 1}", "unexpected"),
        ("{service: light.turn_on, target: {entity_id: light.a}, data: {brightness: '{{ 1 }}'}}", "template"),
        ("{service: light.turn_on, target: {entity_id: light.a}, data: {x: '{% if 1 %}'}}", "template"),
        ("{service: light.turn_on, target: {entity_id: light.a}, data: {x: '{# c #}'}}", "template"),
        ("{service: light.turn_on, target: {entity_id: light.a}, data: {'{{k}}': 1}}", "template"),
        ("{service: light.turn_on, target: {entity_id: light.a}, data: {entity_id: light.b}}", "target"),
        ("{service: light.turn_on, target: {entity_id: light.a}, data: {a: {b: {c: {d: {e: 1}}}}}}", "deeper"),
        ("{service: light.turn_on, target: {entity_id: light.a}, repeat: yes}", "repeat"),
    ],
)
def test_hostile_ha_actions_are_rejected(body, msg):
    assert msg in err(ha_key(body))


def test_oversized_data_is_rejected():
    big = "[" + ", ".join(f"'{'x' * 150}'" for _ in range(8)) + "]"
    assert "over" in err(
        ha_key(f"{{service: light.turn_on, target: {{entity_id: light.a}}, data: {{v: {big}}}}}")
    )
    many = "[" + ",".join(["1"] * 70) + "]"
    assert "more than" in err(
        ha_key(f"{{service: light.turn_on, target: {{entity_id: light.a}}, data: {{v: {many}}}}}")
    )
    ents = ", ".join(f"light.l{i}" for i in range(9))
    assert "1..8" in err(ha_key(f"{{service: light.toggle, target: {{entity_id: [{ents}]}}}}"))


def test_ha_action_is_allowed_everywhere_an_action_is():
    text = BASE + """
pages:
  - id: p
    cols: 2
    rows: 2
    widgets:
      - {type: button, id: b, x: 0, y: 0, label: B, action: {ha: {service: scene.turn_on, target: {entity_id: scene.x}}}}
      - {type: tile, id: t, x: 1, y: 0, label: L, entity: light.a}
"""
    assert validate_all(text) == []
    for bad in ("shell_command.x",):
        assert validate_all(text.replace("scene.turn_on", bad))


def test_ha_device_needs_ha_actions_and_both_power_halves():
    assert "Home Assistant device takes" in err(
        "devices:\n  - {id: d, kind: ha, commands: {x: {proto: nec, addr: 1, cmd: 1}}}\n"
    )
    assert "needs both" in err(
        "devices:\n  - id: d\n    kind: ha\n    power: {on: {ha: {service: light.turn_on, target: {entity_id: light.a}}}}\n"
    )


def test_unknown_ha_device_command_is_a_reference_error():
    text = BASE.replace("{device: shield, command: ok}", "{device: shield, command: nope}")
    assert "has no 'nope'" in err(text)


def test_hub_json_never_carries_ha_things():
    c = compile_config(model())
    blob = json.dumps(c.hub_json)
    for word in ("shield", "ha_call", "light.toggle", "remote.send_command", "scene.bye"):
        assert word not in blob
    assert [d["id"] for d in c.hub_json["devices"]] == ["tv"]
    assert c.hub_json["activities"][1]["devices"] == [{"device": "tv", "input": "hdmi1"}]
    assert "steps" not in c.hub_json["activities"][1] and "off" not in c.hub_json["activities"][1]


def test_repeats_reaches_the_hub_json_only_above_one():
    c = compile_config(model())
    assert c.hub_json["devices"][0]["commands"]["vol_up"]["repeats"] == 3
    one = BASE.replace("repeats: 3", "repeats: 1")
    assert "repeats" not in compile_config(model(one)).hub_json["devices"][0]["commands"]["vol_up"]
    assert "repeats must be 1..8" in err(BASE.replace("repeats: 3", "repeats: 9"))
    assert "repeats must be 1..8" in err(BASE.replace("repeats: 3", "repeats: 0"))


def test_ha_table_follows_the_keymap_inheritance_rule():
    c = compile_config(model())
    k = {"select": 5, "left": 3, "down": 2, "up": 1, "volume_up": 8}
    default, tv_on, stream = c.ha_keys[None], c.ha_keys["tv_on"], c.ha_keys["stream"]
    # default layer: HA only where the default maps HA
    assert set(default) == {k["down"], k["up"]}
    assert default[k["down"]].repeat is True and default[k["down"]].entities == ("remote.shield",)
    assert default[k["up"]].entities == ("light.a", "light.b")
    # tv_on owns select (IR) and inherits the default's HA keys
    assert set(tv_on) == {k["down"], k["up"]}
    # stream overrides select with an HA device command and left with an ha action
    assert stream[k["select"]].data == {"command": "DPAD_CENTER"}
    assert stream[k["left"]].service == "light.toggle"
    assert k["volume_up"] not in stream
    # the hub never sees them, but an IR default must not fire under an activity's HA action
    hub_stream = c.hub_json["activities"][1]["keys"]
    assert hub_stream == {str(k["select"]): {"type": "none"}, str(k["left"]): {"type": "none"}} or (
        str(k["select"]) in hub_stream and str(k["left"]) in hub_stream
    )
    assert str(k["down"]) not in c.hub_json["keys"] and str(k["up"]) not in c.hub_json["keys"]


def test_keymap_view_uses_the_same_rule_and_summarizes_ha():
    maps = resolve_keymaps(model())
    stream = {r["key"]: r for r in next(a for a in maps["activities"] if a["id"] == "stream")["keys"]}
    assert stream["select"]["source"] == "activity"
    assert stream["select"]["action"]["kind"] == "device"  # the model action; the table is flat
    assert stream["left"]["action"] == {
        "kind": "ha",
        "service": "light.toggle",
        "entity": "light.lamp",
        "label": "light.toggle light.lamp",
    }
    assert stream["up"]["source"] == "default" and stream["up"]["action"]["entity"] == "light.a +1"


def test_ha_activity_steps_and_devices_are_compiled_for_the_integration():
    c = compile_config(model())
    act = c.ha_activities["stream"]
    assert act.devices == ("shield",)
    assert [(s.kind, s.device, s.call.service, s.wait_ms) for s in act.start] == [
        ("power_on", "shield", "remote.turn_on", 0),
        ("input", "shield", "remote.send_command", 700),
        ("step", None, "light.turn_off", 0),
    ]
    assert [s.call.service for s in act.off] == ["scene.turn_on"]
    assert "tv_on" not in c.ha_activities
    assert c.activity_ids == ["tv_on", "stream"]
    assert c.devices["shield"].ha and not c.devices["tv"].ha


TILES = """
pages:
  - id: p
    cols: 3
    rows: 3
    widgets:
      - {type: tile, id: l, x: 0, y: 0, label: L, entity: light.a}
      - {type: tile, id: s, x: 1, y: 0, label: S, entity: sensor.t}
      - {type: tile, id: n, x: 2, y: 0, label: N, entity: light.b, tap: none}
      - {type: tile, id: sc, x: 0, y: 1, label: SC, entity: scene.x, tap: toggle}
      - {type: tile, id: c, x: 1, y: 1, label: C, entity: sensor.u, tap: {ha: {service: button.press, target: {entity_id: button.z}}}}
"""


def test_tile_taps_default_to_toggle_and_stay_off_the_hub():
    c = compile_config(parse_model(TILES, "t.yaml"))
    codes = {t.entity_id: 128 + t.widget for t in c.tiles}
    table = c.ha_keys[None]
    assert table[codes["light.a"]].service == "light.toggle"
    assert codes["sensor.t"] not in table  # not toggleable: nothing
    assert codes["light.b"] not in table  # tap: none
    assert table[codes["scene.x"]].service == "scene.turn_on"
    assert table[codes["sensor.u"]].service == "button.press"
    assert c.hub_json["keys"] == {}


def test_tap_toggle_on_a_sensor_is_an_error():
    assert "does not work" in err(TILES.replace("entity: sensor.t}", "entity: sensor.t, tap: toggle}"))
    assert "`tap` belongs on a tile" in err(
        "pages:\n  - id: p\n    cols: 1\n    rows: 1\n    widgets:\n"
        "      - {type: button, id: b, label: B, x: 0, y: 0, tap: none, action: {none: true}}\n"
    )


def test_device_button_on_an_ha_device_becomes_an_ha_action():
    text = BASE + """
pages:
  - id: p
    cols: 1
    rows: 1
    widgets:
      - {type: button, id: ok, x: 0, y: 0, label: OK, action: {device: shield, command: ok}}
"""
    c = compile_config(parse_model(text, "t.yaml"))
    code = 128 + next(i for i in range(8) if c.key_names.get(128 + i) == "tap_ok")
    assert c.ha_keys[None][code].data == {"command": "DPAD_CENTER"}
    assert str(code) not in c.hub_json["keys"]


def test_remote_info_wait_default_is_one_and_a_half_seconds():
    import re
    from pathlib import Path

    src = (Path(__file__).resolve().parents[2] / "custom_components/ha_harmony/const.py").read_text()
    assert re.search(r"^INFO_WAIT = 1\.5\b", src, re.MULTILINE)


def test_the_documented_example_compiles():
    import re
    from pathlib import Path

    doc = (Path(__file__).resolve().parents[2] / "docs/ha-config.md").read_text()
    block = re.search(r"### Example: Shield.*?```yaml\n(.*?)```", doc, re.S).group(1)
    c = compile_config(parse_model(block, "ha-config.md"))
    assert [d["id"] for d in c.hub_json["devices"]] == ["tv"]
    assert c.hub_json["devices"][0]["commands"]["vol_up"]["repeats"] == 3
    assert set(c.ha_keys["shield_tv"]) >= {1, 2, 5}  # up, down, select under the activity
    tile = next(t for t in c.tiles if t.entity_id == "light.living_room")
    assert c.ha_keys[None][128 + tile.widget].service == "light.toggle"
