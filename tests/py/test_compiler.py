"""config_model schema errors + compiler output/limits (pure, no hub)."""

import re
from pathlib import Path

import pytest

from custom_components.ha_harmony.compiler import (
    LIMITS,
    CompileError,
    compact_json,
    compile_config,
    config_hash,
)
from custom_components.ha_harmony.config_model import ConfigError, parse_model
from custom_components.ha_harmony.const import KEY_CODES, SOFT_KEY_BASE
from custom_components.ha_harmony.page_codec import decode_page

ROOT = Path(__file__).resolve().parents[2]
FIX = ROOT / "tests/py/fixtures/ir"
NEC = "{proto: nec, addr: 4, cmd: 2}"

SMALL = f"""
devices:
  - id: tv
    power: {{on: {NEC}, off: {{proto: nec, addr: 4, cmd: 3}}}}
    inputs: {{hdmi1: {{proto: nec, addr: 4, cmd: 9}}}}
    settle_ms: 100
    commands: {{vol_up: {{proto: nec, addr: 4, cmd: 16}}}}
activities:
  - id: watch
    name: Watch
    devices: [{{device: tv, input: hdmi1}}]
    keys: {{ok: {{device: tv, command: vol_up}}}}
keys:
  menu: {{activity: watch}}
  key_77: {{event: custom}}
pages:
  - id: p
    cols: 2
    rows: 2
    widgets:
      - {{type: button, id: b, x: 0, y: 0, label: Go, action: {{activity: "off"}}}}
      - {{type: tile, id: t, x: 1, y: 0, label: T, entity: sensor.temp}}
"""


def model(text, **kw):
    return parse_model(text, "/cfg/hub.yaml", **kw)


def test_key_table_sane():
    assert len(set(KEY_CODES.values())) == len(KEY_CODES)
    assert all(0 <= c < SOFT_KEY_BASE for c in KEY_CODES.values())


def test_golden_compile():
    c = compile_config(model(SMALL))
    assert c.hub_json == {
        "format": 2,
        "devices": [
            {
                "id": "tv",
                "name": "tv",
                "power": {
                    "on": {"proto": "nec", "addr": 4, "cmd": 2},
                    "off": {"proto": "nec", "addr": 4, "cmd": 3},
                },
                "inputs": {"hdmi1": {"proto": "nec", "addr": 4, "cmd": 9}},
                "settle_ms": 100,
                "commands": {"vol_up": {"proto": "nec", "addr": 4, "cmd": 16}},
            }
        ],
        "activities": [
            {
                "id": "watch",
                "name": "Watch",
                "devices": [{"device": "tv", "input": "hdmi1"}],
                "keys": {"2": {"type": "device", "device": "tv", "command": "vol_up"}},
            }
        ],
        "keys": {
            "1": {"type": "activity", "activity": "watch"},
            "77": {"type": "ha", "event": "custom"},
            "128": {"type": "activity", "activity": "off"},
        },
    }
    assert c.key_names == {2: "ok", 1: "menu", 77: "key_77", 128: "tap_b", 129: "tap_t"}
    assert c.activities == ["Watch"]
    (page,) = c.pages
    dec = decode_page(page)
    assert [(w.kind, w.id, w.label) for w in dec.widgets] == [("button", 0, "Go"), ("tile", 1, "T")]
    assert [(t.page, t.widget, t.entity_id) for t in c.tiles] == [(0, 1, "sensor.temp")]
    assert compact_json(c.hub_json).startswith('{"format":2,"devices":[{"id":"tv"')
    assert re.fullmatch(r"[0-9a-f]{8}", c.config_hash) and c.config_hash == config_hash(c.hub_json)


def test_docs_example_compiles():
    text = (ROOT / "docs/ha-config.md").read_text()
    block = re.search(r"```yaml\n(.*?)```", text, re.DOTALL).group(1)
    c = compile_config(model(block))
    assert c.activities == ["Watch TV", "Stream"]
    assert len(c.pages) == 1 and len(c.tiles) == 1
    assert {"vol_up", "home", "tap_mute", "tap_off", "tap_temp"} <= set(c.key_names.values())
    assert len(text.splitlines()) < 120


def test_empty_file_is_empty_model():
    c = compile_config(model(""))
    assert c.hub_json == {"format": 2, "devices": [], "activities": [], "keys": {}}


# ------------------------------------------------------------------ schema errors
def bad(text, *needles, **kw):
    with pytest.raises(ConfigError) as ei:
        model(text, **kw)
    msg = str(ei.value)
    assert msg.startswith("/cfg/hub.yaml"), msg
    for n in needles:
        assert n in msg, msg
    return ei.value


def test_errors_name_file_line_and_path():
    e = bad(
        "devices:\n  - id: tv\n    commands:\n      vol: {proto: nec, addr: 4, cmd: 999}\n",
        "/cfg/hub.yaml:4: devices[0].commands.vol",
        "cmd 999 outside 0..255",
    )
    assert e.line == 4


TILE = "pages: [{id: p, cols: 2, rows: 2, widgets: [%s]}]"


@pytest.mark.parametrize(
    ("text", "needle"),
    [
        ("devices: [{id: Bad Id}]", "devices[0].id"),
        ("devices: [{name: x}]", "required key not provided"),
        ("bogus: 1", "not a valid option"),
        ("- 1", "top level must be a mapping"),
        ("devices: [", "invalid YAML"),
        ("devices: [{id: a, power: {on: " + NEC + "}}]", "both `on` and `off`"),
        ("devices: [{id: a, settle_ms: 70000}]", "0..60000"),
        ("devices: [{id: a, commands: {x: {proto: nec, addr: 1}}}]", "proto needs addr and cmd"),
        ("devices: [{id: a, commands: {x: {proto: nec, pronto: '0000'}}}]", "exactly one"),
        ("devices: [{id: a, commands: {x: {}}}]", "exactly one"),
        ("devices: [{id: a, commands: {x: {pronto: 'zz'}}}]", "pronto"),
        (
            "devices: [{id: a, commands: {x: {proto: nec, addr: 1, cmd: 1, port_mask: 0}}}]",
            "port_mask",
        ),
        ("devices: [{id: a}, {id: a}]", "duplicate device id"),
        ("activities: [{id: 'off'}]", "reserved"),
        ("activities: [{id: x, devices: [{device: nope}]}]", "unknown device 'nope'"),
        ("activities: [{id: x, steps: [{activity: x}]}]", "only allowed in key maps"),
        ("keys: {bogus_key: {none: true}}", "unknown key"),
        ("keys: {menu: {device: tv, command: x}}", "unknown device 'tv'"),
        ("keys: {menu: {activity: nope}}", "unknown activity 'nope'"),
        ("keys: {menu: {device: a, command: x, input: y}}", "exactly one"),
        ("devices: [{id: a}]\nkeys: {menu: {device: a, command: y}}", "has no 'y'"),
        (TILE % "{type: button, id: b, x: 0, y: 0}", "needs an `action`"),
        (TILE % "{type: tile, id: t, x: 0, y: 0, entity: nodot}", "not an entity id"),
        (TILE % "{type: tile, x: 0, y: 0, entity: a.b}", "needs an `id`"),
    ],
)
def test_schema_errors(text, needle):
    bad(text, needle)


def test_ir_file_refs():
    text = (
        "devices:\n  - id: a\n    commands:\n"
        "      p: {flipper: {file: sample.ir, signal: Power}}\n"
        "      q: {smartir: {file: smartir_pronto.json, command: power}, port_mask: 2}\n"
    )
    m = model(text, base_dir=FIX)
    cmds = m["devices"][0]["commands"]
    assert cmds["p"] == {"proto": "nec", "addr": 4, "cmd": 8}
    assert cmds["q"]["pronto"].startswith("0000 006D") and cmds["q"]["port_mask"] == 2
    bad(text.replace("Power", "Nope"), "has no signal 'Nope'", base_dir=FIX)
    bad(text.replace("sample.ir", "missing.ir"), "cannot read flipper file", base_dir=FIX)
    bad(text.replace("sample.ir", "../x.ir"), "must stay inside", base_dir=FIX)
    bad(text.replace("sample.ir", "bad_header.ir"), "bad_header.ir", base_dir=FIX)


@pytest.mark.parametrize(
    ("ref", "needle"),
    [
        ("{flipper: {file: missing.ir, signal: A}}", "cannot read flipper file 'missing.ir'"),
        ("{flipper: {file: sample.ir, signal: Nope}}", "has no signal 'Nope'"),
        ("{flipper: {file: ../x.ir, signal: A}}", "must stay inside"),
        ("{flipper: {file: binary.ir, signal: A}}", "'binary.ir': not UTF-8 text"),
        ("{smartir: {file: binary.ir, command: a}}", "'binary.ir': not UTF-8 text"),
        ("{smartir: {file: smartir_pronto.json, command: nope}}", "nope"),
        ("{smartir: {file: sub, command: a}}", "cannot read smartir file 'sub'"),
    ],
)
def test_bad_ir_file_refs_carry_file_line_and_path(tmp_path, ref, needle):
    (tmp_path / "binary.ir").write_bytes(b"\xff\xfe\x00")
    (tmp_path / "sub").mkdir()
    for f in ("sample.ir", "smartir_pronto.json"):
        (tmp_path / f).write_bytes((FIX / f).read_bytes())
    e = bad(
        f"devices:\n  - id: a\n    commands:\n      x: {ref}\n",
        "/cfg/hub.yaml:4: devices[0].commands.x",
        needle,
        base_dir=tmp_path,
    )
    assert e.line == 4 and e.path == ["devices", 0, "commands", "x"]


# ------------------------------------------------------------------ limits
def test_limits_match_mapping_h():
    src = (ROOT / "hub/engine/mapping.h").read_text()
    defs = {m[1]: int(m[2]) for m in re.finditer(r"#define HP_MAP_(\w+)\s+(\d+)", src)}
    defs.pop("ERR_MAX")
    assert defs == LIMITS, "python limits drifted from hub/engine/mapping.h"


def devices_yaml(n, cmds=0):
    cmd = "{" + ", ".join(f"c{j}: {NEC}" for j in range(cmds)) + "}"
    return "devices:\n" + "".join(f"  - {{id: d{i}, commands: {cmd}}}\n" for i in range(n))


def pronto(words):
    return f"'0000 006D 0000 {(words - 4) // 2:04X} {'0010 ' * (words - 4)}'"


@pytest.mark.parametrize(
    ("text", "needle"),
    [
        (devices_yaml(17), "too many devices: 17"),
        (devices_yaml(16, 17), "too many commands: 272"),
        (
            "devices: [{id: a, inputs: {" + ", ".join(f"i{j}: {NEC}" for j in range(65)) + "}}]",
            "too many inputs: 65",
        ),
        (
            "activities:\n" + "".join(f"  - {{id: a{i}}}\n" for i in range(17)),
            "too many activities",
        ),
        (
            "activities: [{id: a, steps: [" + ", ".join(["{none: true}"] * 17) + "]}]",
            "too many `steps`",
        ),
        (
            "devices: [" + ", ".join(f"{{id: d{i}}}" for i in range(9)) + "]\n"
            "activities: [{id: a, devices: ["
            + ", ".join(f"{{device: d{i}}}" for i in range(9))
            + "]}]",
            "9 devices",
        ),
        (
            "devices: [{id: a, commands: {"
            + ", ".join(f"c{i}: {{pronto: {pronto(500)}}}" for i in range(40))
            + "}}]",
            "pool holds 16384",
        ),
    ],
)
def test_oversize_is_a_clean_error(text, needle):
    m = model(text)
    with pytest.raises(CompileError, match=re.escape(needle)) as ei:
        compile_config(m)
    assert "hub allows" in str(ei.value) or "pool holds" in str(ei.value)


@pytest.mark.parametrize("name", ["power_on", "power_off", "input_x", "input_"])
def test_reserved_command_names_rejected(name):
    bad(
        f"devices: [{{id: tv, commands: {{{name}: {NEC}}}}}]",
        "devices[0].commands",
        name,
        "reserved",
    )
    model(f"devices: [{{id: tv, commands: {{my_{name}: {NEC}}}}}]")  # only the exact names/prefix


def test_pronto_too_long_for_hub():
    with pytest.raises(ConfigError, match="too long for hub|520"):
        model("devices: [{id: a, commands: {x: {pronto: " + pronto(530) + "}}}]")


def test_too_many_keys_widgets_and_bytes():
    keys = "keys:\n" + "".join(f"  key_{i}: {{none: true}}\n" for i in range(128))
    compile_config(model(keys))  # exactly at the limit
    page = (
        "pages:\n  - {id: p, cols: 16, rows: 16, widgets: ["
        + ", ".join(
            f"{{type: button, id: b{i}, x: {i % 16}, y: {i // 16}, action: {{none: true}}}}"
            for i in range(33)
        )
        + "]}"
    )
    with pytest.raises(CompileError, match="too many widgets: 33"):
        compile_config(model(page))
    two = TILE % "{type: label, x: 0, y: 0}, {type: label, x: 0, y: 0}"
    with pytest.raises(CompileError, match="page 'p'.*overlap"):
        compile_config(model(two))
    with pytest.raises(ConfigError, match="at most 31 bytes"):
        model("keys: {menu: {event: " + "x" * 32 + "}}")
    many = "pages:\n" + "".join(
        f"  - {{id: p{i}, cols: 16, rows: 16, widgets: ["
        + ", ".join(
            f"{{type: button, id: b{i}_{j}, x: {j % 16}, y: {j // 16}, action: {{none: true}}}}"
            for j in range(32)
        )
        + "]}\n"
        for i in range(5)
    )
    with pytest.raises(CompileError, match="128 soft keys"):
        compile_config(model(many))
