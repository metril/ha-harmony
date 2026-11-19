"""Bluetooth HID devices: code table, config model, compiler output."""

import json
from pathlib import Path

import pytest

from custom_components.ha_harmony.harmony_core import bt_codes
from custom_components.ha_harmony.harmony_core.compiler import compact_json, compile_config
from custom_components.ha_harmony.harmony_core.config_model import ConfigError, parse_model

VEC = json.loads((Path(__file__).resolve().parents[1] / "vectors" / "bt_codes.json").read_text())


@pytest.mark.parametrize("v", VEC["names"], ids=[v["name"] for v in VEC["names"]])
def test_names_and_codes_resolve(v):
    assert bt_codes.resolve(v["name"]) == (v["page"], v["usage"])


@pytest.mark.parametrize("bad", VEC["invalid"])
def test_bad_codes_are_refused(bad):
    with pytest.raises(bt_codes.BtCodeError):
        bt_codes.resolve(bad)


def test_pages_other_than_7_and_12_say_so():
    with pytest.raises(bt_codes.BtCodeError, match="07 .keyboard. or 0C"):
        bt_codes.parse_hid("HID:09:0001")


def test_addresses_normalise():
    for raw, want in VEC["addresses"]:
        assert bt_codes.norm_address(raw) == want
    for bad in VEC["bad_addresses"]:
        with pytest.raises(bt_codes.BtCodeError):
            bt_codes.norm_address(bad)


def test_every_default_command_resolves_and_is_unique_by_name():
    assert len(set(bt_codes.DEFAULT_COMMANDS)) == len(bt_codes.DEFAULT_COMMANDS)
    for n in bt_codes.DEFAULT_COMMANDS:
        assert bt_codes.resolve(n) == bt_codes.BUILTINS[n]


CFG = """
devices:
  - id: tv
    commands: {vol_up: {proto: nec, addr: 4, cmd: 16}}
  - id: shield
    kind: bt
    name: Shield
    address: aa:bb:cc:dd:ee:ff
    transport: le
    commands: {Home: Home, Ok: "HID:07:0028", Search: "0x0221"}
  - id: box
    kind: bt
activities:
  - id: watch
    devices: [{device: shield}]
    steps: [{device: shield, command: Home}]
    keys: {select: {device: shield, command: Ok}, up: {device: box, command: DirectionUp}}
"""


def test_model_resolves_bt_devices():
    m = parse_model(CFG)
    shield, box = m["devices"][1], m["devices"][2]
    assert shield["address"] == "AA:BB:CC:DD:EE:FF" and shield["transport"] == "le"
    assert shield["commands"]["Search"] == {"type": "bt", "page": 12, "usage": 545}
    assert box["transport"] == "auto" and "address" not in box
    assert tuple(box["commands"]) == bt_codes.DEFAULT_COMMANDS  # no `commands`: every built-in
    assert box["inputs"] == {}


def test_compiler_emits_host_slots_and_bt_hosts():
    c = compile_config(parse_model(CFG))
    hub = c.hub_json
    assert [d["id"] for d in hub["devices"]] == ["tv", "shield", "box"]
    assert hub["devices"][1]["commands"]["Ok"] == {"type": "bt", "page": 7, "usage": 40, "host": 0}
    assert hub["devices"][2]["commands"]["DirectionUp"]["host"] == 1
    assert hub["bt_hosts"] == [
        {"slot": 0, "addr": "AA:BB:CC:DD:EE:FF", "name": "Shield", "transport": "le"},
        {"slot": 1, "addr": None, "name": "box", "transport": "auto"},
    ]
    act = hub["activities"][0]
    assert act["steps"] == [{"type": "device", "device": "shield", "command": "Home"}]
    assert c.devices["shield"].commands == ("Home", "Ok", "Search")
    json.loads(compact_json(hub))


def test_no_bt_devices_no_table():
    assert "bt_hosts" not in compile_config(parse_model("devices: [{id: tv}]")).hub_json


def test_slots_follow_config_order_around_other_devices():
    cfg = "devices:\n" + "".join(
        f"  - {{id: b{i}, kind: bt, commands: {{x: Home}}}}\n  - {{id: i{i}}}\n" for i in range(3)
    )
    hosts = compile_config(parse_model(cfg)).hub_json["bt_hosts"]
    assert [h["slot"] for h in hosts] == [0, 1, 2] and [h["name"] for h in hosts] == [
        "b0",
        "b1",
        "b2",
    ]


def test_255_devices_fit_256_do_not():
    from custom_components.ha_harmony.harmony_core.hub_limits import CURRENT

    def cfg(n):
        return "devices:\n" + "".join(
            f"  - {{id: b{i}, kind: bt, commands: {{x: Home}}}}\n" for i in range(n)
        )

    assert len(compile_config(parse_model(cfg(255)), limits=CURRENT).hub_json["bt_hosts"]) == 255
    with pytest.raises(ConfigError, match="too many Bluetooth devices: 256 .*at most 255"):
        parse_model(cfg(256))


@pytest.mark.parametrize(
    ("body", "msg"),
    [
        ("address: nope", "AA:BB:CC:DD:EE:FF"),
        ("transport: wifi", "transport"),
        ("commands: {x: Nope}", "unknown bt command"),
        ("commands: {x: 'HID:09:0001'}", "not supported"),
        ("commands: {x: {proto: nec, addr: 1, cmd: 1}}", "bt command"),
        ("inputs: {a: Home}", "inputs"),
        ("power: {on: Home}", "power"),
    ],
)
def test_bad_bt_devices(body, msg):
    with pytest.raises(ConfigError, match=msg):
        parse_model(f"devices:\n  - {{id: b, kind: bt, {body}}}\n")


def test_unknown_bt_command_in_a_step_is_a_reference_error():
    with pytest.raises(ConfigError, match="has no 'Fly'"):
        parse_model(
            "devices: [{id: b, kind: bt, commands: {x: Home}}]\n"
            "activities: [{id: a, steps: [{device: b, command: Fly}]}]\n"
        )


# ------------------------------------------------------------------ bt_remotes (input)
def test_remote_defaults_use_the_elites_key_names():
    from custom_components.ha_harmony.harmony_core.keys import key_code

    for _spec, key in bt_codes.DEFAULT_REMOTE_KEYS:
        assert key_code(key) is not None, key
    m = parse_model("bt_remotes: [{name: Homatics}]\n")
    r = m["bt_remotes"][0]
    table = {(p, u): k for p, u, k in r["keys"]}
    assert (
        table[(7, 0x52)] == "up"
        and table[(12, 0xE9)] == "volume_up"
        and table[(12, 0x224)] == "exit"
    )
    assert table[(7, 0x27)] == "key_100" and table[(12, 0x30)] == "power_off_activity"
    assert "address" not in r and r["keys"] == sorted(r["keys"])


def test_remote_keys_override_defaults_and_are_validated():
    m = parse_model(
        "bt_remotes:\n  - name: R\n    address: '66:55:44:33:22:11'\n"
        "    keys: {VolumeUp: channel_up, 'HID:0C:0223': favorites, F1: key_90}\n"
    )
    r = m["bt_remotes"][0]
    assert r["address"] == "66:55:44:33:22:11"
    table = {(p, u): k for p, u, k in r["keys"]}
    assert table[(12, 0xE9)] == "channel_up" and table[(12, 0x223)] == "favorites"
    assert table[(7, 0x3A)] == "key_90" and table[(7, 0x52)] == "up"  # the rest stays default
    for body, msg in [
        ("keys: {VolumeUp: nope}", "unknown key"),
        ("keys: {Fly: up}", "unknown bt command"),
        ("keys: {VolumeUp: up, 'HID:0C:00E9': down}", "same usage"),
        ("keys: [up]", "mapping|dict"),
    ]:
        with pytest.raises(ConfigError, match=msg):
            parse_model(f"bt_remotes: [{{name: R, {body}}}]\n")


def test_remote_limit_and_compiled_table():
    one = "{name: R%d, address: aa:bb:cc:dd:ee:%02x}"
    cfg = "bt_remotes: [" + ", ".join(one % (i, i) for i in range(32)) + "]\n"
    hub = compile_config(parse_model(cfg)).hub_json
    assert [r["slot"] for r in hub["bt_remotes"]] == list(range(32))
    assert hub["bt_remotes"][1]["addr"] == "AA:BB:CC:DD:EE:01"
    first = hub["bt_remotes"][0]["keys"]
    assert [7, 0x52, 1] in first and [12, 0xE9, 8] in first  # [page, usage, key id]
    with pytest.raises(ConfigError, match="too many Bluetooth remotes: 33 .*at most 32"):
        parse_model("bt_remotes: [" + ", ".join(one % (i, i) for i in range(33)) + "]\n")
    assert "bt_remotes" not in compile_config(parse_model("devices: []")).hub_json


def test_bt_tables_count_toward_the_hub_budget():
    from custom_components.ha_harmony.harmony_core.compiler import config_stats
    from custom_components.ha_harmony.harmony_core.hub_limits import CURRENT, estimate_bytes

    def need(text):
        return estimate_bytes(CURRENT, **config_stats(compile_config(parse_model(text)).hub_json))

    nkeys = len(
        compile_config(parse_model("bt_remotes: [{name: R}]\n")).hub_json["bt_remotes"][0]["keys"]
    )
    assert (
        need("bt_remotes: [{name: R}]\n") - need("devices: []")
        == nkeys * CURRENT.key_b + CURRENT.dev_b
    )
    one_host = "devices: [{id: b, kind: bt, commands: {x: Home}}]\n"
    # a bt device is a device (its commands too) plus a host-table entry
    assert need(one_host) - need("devices: []") == 2 * CURRENT.dev_b + CURRENT.cmd_b


def test_a_bare_yaml_address_asks_for_quotes():
    with pytest.raises(ConfigError, match="quote the Bluetooth address"):
        parse_model("devices: [{id: b, kind: bt, address: 11:22:33:44:55:44}]\n")


def test_menu_is_the_consumer_menu_and_guide_is_separate():
    assert bt_codes.resolve("Menu") == (12, 0x40) and bt_codes.resolve("Guide") == (12, 0x8D)
    with pytest.raises(bt_codes.BtCodeError):
        bt_codes.resolve("settings")


def test_addresses_are_unique_across_bt_devices_and_remotes():
    a = "aa:bb:cc:dd:ee:ff"
    with pytest.raises(ConfigError, match="already used by devices"):
        parse_model(
            f"devices: [{{id: a, kind: bt, address: '{a}'}}, {{id: b, kind: bt, address: AA:BB:CC:DD:EE:FF}}]\n"
        )
    with pytest.raises(ConfigError, match="already used by devices"):
        parse_model(
            f"devices: [{{id: a, kind: bt, address: '{a}'}}]\nbt_remotes: [{{name: R, address: '{a}'}}]\n"
        )


def test_push_time_check_against_hello_ack_bt():
    from custom_components.ha_harmony.harmony_core.hub_limits import bt_fit_error

    hub = compile_config(parse_model(CFG)).hub_json  # 2 bt devices
    assert "no Bluetooth support" in bt_fit_error(hub, None)
    assert bt_fit_error(hub, {"hosts_max": 2, "remotes_max": 0}) is None
    assert "too many Bluetooth devices: 2 (the hub allows at most 1)" == bt_fit_error(
        hub, {"hosts_max": 1, "remotes_max": 4}
    )
    rem = compile_config(parse_model("bt_remotes: [{name: R}]\n")).hub_json
    assert "too many Bluetooth remotes: 1" in bt_fit_error(rem, {"hosts_max": 4, "remotes_max": 0})
    plain = compile_config(parse_model("devices: [{id: tv}]")).hub_json
    assert bt_fit_error(plain, None) is None  # no bt content: any hub
