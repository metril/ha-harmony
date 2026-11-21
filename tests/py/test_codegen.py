import textwrap
from pathlib import Path

import pytest

from proto.codegen import SchemaError, generate, load_schema

ROOT = Path(__file__).resolve().parents[2]
REAL_SCHEMA = ROOT / "proto" / "schema.yaml"
GEN_DIR = ROOT / "proto" / "gen"


def write(tmp_path, body):
    p = tmp_path / "schema.yaml"
    p.write_text(textwrap.dedent(body))
    return p


MINI = """
    version: 1
    enums:
      color: {red: 0, blue: 1}
    messages:
      - {name: foo, code: 1, transport: ws, fields: [{name: a, type: u8}, {name: b, type: str, optional: true}]}
      - {name: bar, code: 2, transport: both, fields: [{name: c, type: "enum:color"}]}
"""


def load_gen_py(out_dir):
    ns = {"__name__": "gen_proto"}
    exec(compile((out_dir / "harmony_proto.py").read_text(), "gen", "exec"), ns)  # noqa: S102
    return ns


def test_load_schema_parses_messages(tmp_path):
    s = load_schema(write(tmp_path, MINI))
    assert s.version == 1
    foo, bar = s.messages
    assert (foo.name, foo.code, foo.transport) == ("foo", 1, "ws")
    assert [(f.name, f.type, f.optional) for f in foo.fields] == [
        ("a", "u8", False),
        ("b", "str", True),
    ]
    assert bar.transport == "both" and bar.fields[0].type == "enum:color"


def test_duplicate_code_rejected(tmp_path):
    p = write(
        tmp_path,
        """
        version: 1
        messages:
          - {name: a, code: 1, transport: ws, fields: []}
          - {name: b, code: 1, transport: ws, fields: []}
        """,
    )
    with pytest.raises(SchemaError):
        load_schema(p)


def test_unknown_wire_type_rejected(tmp_path):
    p = write(
        tmp_path,
        """
        version: 1
        messages:
          - {name: a, code: 1, transport: ws, fields: [{name: x, type: u64}]}
        """,
    )
    with pytest.raises(SchemaError):
        load_schema(p)


@pytest.mark.parametrize("transport", ["rf", "both"])
def test_json_only_legal_in_ws(tmp_path, transport):
    body = f"""
        version: 1
        messages:
          - {{name: a, code: 1, transport: {transport}, fields: [{{name: x, type: json}}]}}
    """
    with pytest.raises(SchemaError):
        load_schema(write(tmp_path, body))
    ok = body.replace(transport, "ws")
    assert load_schema(write(tmp_path, ok)).messages[0].fields[0].type == "json"


def test_unknown_enum_rejected(tmp_path):
    p = write(
        tmp_path,
        """
        version: 1
        messages:
          - {name: a, code: 1, transport: ws, fields: [{name: x, type: "enum:nope"}]}
        """,
    )
    with pytest.raises(SchemaError):
        load_schema(p)


def test_generate_emits_three_files(tmp_path):
    out = tmp_path / "out"
    generate(write(tmp_path, MINI), out)
    for n in ("harmony_proto.h", "harmony_proto.c", "harmony_proto.py"):
        assert (out / n).stat().st_size > 0


def test_generated_python_round_trips(tmp_path):
    out = tmp_path / "out"
    generate(write(tmp_path, MINI), out)
    ns = load_gen_py(out)
    MsgType = ns["MsgType"]
    env = ns["encode_ws"](MsgType.FOO, {"a": 7}, msg_id=5)
    assert env == {"v": 1, "id": 5, "t": "foo", "d": {"a": 7}}
    assert ns["decode_ws"](env) == (MsgType.FOO, {"a": 7})
    env = ns["encode_ws"](MsgType.BAR, {"c": 1})
    assert ns["decode_ws"](env) == (MsgType.BAR, {"c": 1})
    assert MsgType.BAR == 2 and ns["PROTO_VERSION"] == 1


def test_generated_python_rejects_version_mismatch(tmp_path):
    out = tmp_path / "out"
    generate(write(tmp_path, MINI), out)
    ns = load_gen_py(out)
    with pytest.raises(ns["ProtoError"]):
        ns["decode_ws"]({"v": 99, "id": 1, "t": "foo", "d": {"a": 1}})


@pytest.mark.parametrize(
    "env",
    [
        {"v": 1, "id": 1, "t": "foo", "d": {}},  # missing required
        {"v": 1, "id": 1, "t": "foo", "d": {"a": "x"}},  # wrong type
        {"v": 1, "id": 1, "t": "foo", "d": {"a": True}},  # bool is not u8
        {"v": 1, "id": 1, "t": "foo", "d": {"a": 256}},  # out of range
        {"v": 1, "id": 1, "t": "nope", "d": {}},  # unknown message
        {"v": 1, "id": 1, "t": "bar", "d": {"c": 9}},  # bad enum value
        {"v": 1, "t": "foo", "d": {"a": 1}},  # missing id
    ],
)
def test_generated_python_rejects_bad_envelopes(tmp_path, env):
    out = tmp_path / "out"
    generate(write(tmp_path, MINI), out)
    ns = load_gen_py(out)
    with pytest.raises(ns["ProtoError"]):
        ns["decode_ws"](env)


def test_encode_rejects_rf_only_message(tmp_path):
    p = write(
        tmp_path,
        """
        version: 1
        messages:
          - {name: r, code: 1, transport: rf, fields: []}
        """,
    )
    out = tmp_path / "out"
    generate(p, out)
    ns = load_gen_py(out)
    with pytest.raises(ns["ProtoError"]):
        ns["encode_ws"](ns["MsgType"].R, {})


def test_generated_c_header_declares_every_message(tmp_path):
    out = tmp_path / "out"
    generate(write(tmp_path, MINI), out)
    h = (out / "harmony_proto.h").read_text()
    for s in ("HP_MSG_FOO", "HP_MSG_BAR", "HP_PROTO_VERSION 1", "hp_msg_desc"):
        assert s in h


def test_real_schema_has_phase2_messages():
    names = {m.name for m in load_schema(REAL_SCHEMA).messages}
    assert names == {
        "hello", "hello_ack", "error", "ping", "pong", "set_config", "config_ack",
        "command", "command_result", "key_event", "state_update", "page_push", "battery",
        "device_state", "tile_update", "activity_list", "remote_attached", "remote_info", "tile_state",
        "config_chunk", "set_hub_id", "hub_id_ack", "theme_push",
        "bt_pair", "bt_state", "bt_forget", "bt_key", "bt_remote_key", "bt_listen", "hub_log",
        "now_playing", "cover_chunk",
        "ota_begin", "ota_chunk", "ota_end", "ota_abort", "ota_status",
        "ota_offer", "ota_frag", "ota_frag_ack", "ota_commit", "ota_result",
    }  # fmt: skip


def test_ota_message_ids_are_stable_and_remote_ones_reserved():
    msgs = {m.name: m for m in load_schema(REAL_SCHEMA).messages}
    ids = {n: m.code for n, m in msgs.items() if n.startswith("ota_")}
    assert ids == {
        "ota_begin": 20, "ota_chunk": 21, "ota_end": 22, "ota_abort": 23, "ota_status": 24,
        "ota_offer": 30, "ota_frag": 31, "ota_frag_ack": 32, "ota_commit": 33, "ota_result": 34,
    }  # fmt: skip
    assert all(msgs[n].transport == "rf" for n in ("ota_offer", "ota_frag", "ota_frag_ack", "ota_commit", "ota_result"))


def test_real_schema_message_codes_are_unique():
    codes = [m.code for m in load_schema(REAL_SCHEMA).messages]
    assert len(codes) == len(set(codes))
    msgs = {m.name: m for m in load_schema(REAL_SCHEMA).messages}
    assert msgs["config_chunk"].code == 25
    assert (msgs["set_hub_id"].code, msgs["hub_id_ack"].code) == (26, 27)


def test_config_ack_hash_and_remote_attached_in_schema():
    msgs = {m.name: m for m in load_schema(REAL_SCHEMA).messages}
    ack = {f.name: f for f in msgs["config_ack"].fields}
    assert ack["config_hash"].type == "str" and ack["config_hash"].optional
    assert msgs["remote_attached"].transport == "ws" and not msgs["remote_attached"].fields


def test_committed_generated_files_are_fresh(tmp_path):
    generate(REAL_SCHEMA, tmp_path)
    for n in ("harmony_proto.h", "harmony_proto.c", "harmony_proto.py"):
        assert (tmp_path / n).read_text() == (GEN_DIR / n).read_text(), (
            f"{n} stale: run `uv run python -m proto.codegen`"
        )


def test_vendored_ha_copy_is_fresh(tmp_path):
    ha = tmp_path / "ha"
    generate(REAL_SCHEMA, tmp_path / "gen", ha)
    vendored = GEN_DIR.parents[1] / "harmony_core" / "harmony_proto.py"
    assert (ha / "harmony_proto.py").read_text() == vendored.read_text(), (
        "vendored harmony_proto.py stale: run `uv run python -m proto.codegen`"
    )


def _real_py():
    ns = {"__name__": "real_proto"}
    src = (GEN_DIR / "harmony_proto.py").read_text()
    exec(compile(src, "gen", "exec"), ns)  # noqa: S102
    return ns


def test_hello_ack_config_hash_optional():
    ns = _real_py()
    T = ns["MsgType"]
    base = {"hub_id": "h"}
    assert ns["decode_ws"](ns["encode_ws"](T.HELLO_ACK, base)) == (T.HELLO_ACK, base)
    withh = {"hub_id": "h", "config_hash": "abcd1234"}
    assert ns["decode_ws"](ns["encode_ws"](T.HELLO_ACK, withh)) == (T.HELLO_ACK, withh)
    with pytest.raises(ns["ProtoError"]):
        ns["encode_ws"](T.HELLO_ACK, {"hub_id": "h", "config_hash": 5})


def test_activity_list_is_ws_only_with_optional_hash():
    ns = _real_py()
    T = ns["MsgType"]
    assert ns["MESSAGES"]["activity_list"][1:] == (
        "ws",
        (("activities", "json", False), ("config_hash", "str", True)),
    )  # fmt: skip
    d = {"activities": ["TV", "Music"], "config_hash": "9198e550"}
    assert ns["decode_ws"](ns["encode_ws"](T.ACTIVITY_LIST, d)) == (T.ACTIVITY_LIST, d)
    d = {"activities": []}
    assert ns["decode_ws"](ns["encode_ws"](T.ACTIVITY_LIST, d)) == (T.ACTIVITY_LIST, d)
    with pytest.raises(ns["ProtoError"]):
        ns["encode_ws"](T.ACTIVITY_LIST, {"config_hash": "x"})


def test_device_state_round_trip():
    ns = _real_py()
    T = ns["MsgType"]
    d = {"device": "tv", "power": True, "input": "hdmi1"}
    assert ns["decode_ws"](ns["encode_ws"](T.DEVICE_STATE, d)) == (T.DEVICE_STATE, d)
    d = {"device": "tv", "power": False}
    assert ns["decode_ws"](ns["encode_ws"](T.DEVICE_STATE, d)) == (T.DEVICE_STATE, d)
    with pytest.raises(ns["ProtoError"]):
        ns["encode_ws"](T.DEVICE_STATE, {"device": "tv"})


def test_tile_update_is_rf_only():
    ns = _real_py()
    assert ns["MESSAGES"]["tile_update"][1:] == (
        "rf",
        (("remote", "u8", False), ("page", "u8", False), ("tile", "u8", False),
         ("text", "str", False)),
    )  # fmt: skip
    with pytest.raises(ns["ProtoError"]):
        ns["encode_ws"](ns["MsgType"].TILE_UPDATE, {"remote": 1, "page": 0, "tile": 2, "text": "x"})


STOCK_KEYS = [
    "Up",
    "Down",
    "Left",
    "Right",
    "Select",
    "Menu",
    "Exit",
    "VolumeUp",
    "VolumeDown",
    "VolumeMute",
    "ChannelUp",
    "ChannelDown",
    "PrevChannel",
    "Play",
    "Pause",
    "Stop",
    "Record",
    "Rewind",
    "FastForward",
    "DVR",
    "Guide",
    "Info",
    "Red",
    "Green",
    "Yellow",
    "Blue",
    "HomeControl1",
    "HomeControl2",
    "HomeControl3",
    "HomeControl4",
    "HomeControlPlus",
    "HomeControlMinus",
    "PowerOffActivity",
    "Home",
    "Favorites",
]


def test_key_table_is_the_35_confirmed_keys():
    keys = load_schema(REAL_SCHEMA).keys
    assert [k.name for k in keys] == STOCK_KEYS
    assert len({k.id for k in keys}) == len({k.evdev for k in keys}) == 35
    assert all(0 < k.id < 128 for k in keys)  # 128+ is reserved for page soft keys
    by = {k.name: k for k in keys}
    assert (by["VolumeUp"].evdev, by["Home"].evdev, by["Favorites"].evdev) == (115, 364, 102)
    assert by["HomeControl1"].event == "home_control_1" and by["DVR"].event == "dvr"
    assert by["PowerOffActivity"].event == "power_off_activity"


def test_python_key_table_round_trips():
    ns = _real_py()
    for kid, evdev, name, event in ns["KEYS"]:
        assert ns["KEY_BY_ID"][kid] == ns["KEY_BY_EVDEV"][evdev] == ns["KEY_BY_NAME"][name]
        assert ns["KEY_BY_EVENT"][event][0] == kid
    assert len(ns["KEYS"]) == 35 and 9999 not in ns["KEY_BY_EVDEV"]


@pytest.mark.parametrize(
    "keys, msg",
    [
        ("[{id: 1, evdev: 5, name: A}, {id: 1, evdev: 6, name: B}]", "duplicate"),
        ("[{id: 1, evdev: 5, name: A}, {id: 2, evdev: 5, name: B}]", "duplicate"),
        ("[{id: 128, evdev: 5, name: A}]", "id must be"),
        ("[{id: 1, evdev: 5, name: a}]", "bad key name"),
    ],
)
def test_bad_key_tables_rejected(tmp_path, keys, msg):
    with pytest.raises(SchemaError, match=msg):
        load_schema(write(tmp_path, f"version: 1\nkeys: {keys}\n"))


def test_remote_info_and_tile_state_round_trip():
    ns = _real_py()
    T = ns["MsgType"]
    assert ns["MESSAGES"]["remote_info"][1:] == (
        "both",
        (("remote", "u8", False), ("page_fmt", "u8", False), ("caps", "u8", False),
         ("icons", "u8", False), ("gen", "u8", False), ("pages", "u8", False)),
    )  # fmt: skip
    assert ns["MESSAGES"]["tile_state"][1:] == (
        "rf",
        (("remote", "u8", False), ("page", "u8", False), ("tile", "u8", False),
         ("state", "u8", False), ("text", "str", False)),
    )  # fmt: skip
    d = {"remote": 1, "page_fmt": 2, "caps": 3, "icons": 50, "gen": 167, "pages": 12}
    assert ns["decode_ws"](ns["encode_ws"](T.REMOTE_INFO, d)) == (T.REMOTE_INFO, d)
    assert T.REMOTE_INFO == 18 and T.TILE_STATE == 19
    with pytest.raises(ns["ProtoError"]):
        ns["encode_ws"](T.REMOTE_INFO, {**d, "icons": 256})
    with pytest.raises(ns["ProtoError"]):  # the stored-set fields are not optional
        ns["encode_ws"](T.REMOTE_INFO, {k: v for k, v in d.items() if k != "gen"})
    with pytest.raises(ns["ProtoError"]):  # rf-only: not a ws message
        ns["encode_ws"](T.TILE_STATE, {"remote": 1, "page": 0, "tile": 1, "state": 2, "text": "x"})


def test_receiver_ignores_unknown_fields_sender_stays_strict(tmp_path):
    out = tmp_path / "out"
    generate(write(tmp_path, MINI), out)
    ns = load_gen_py(out)
    msg_type, data = ns["decode_ws"]({"v": 1, "id": 1, "t": "foo", "d": {"a": 1, "zz": "new"}})
    assert (msg_type, data) == (ns["MsgType"].FOO, {"a": 1})
    with pytest.raises(ns["ProtoError"]):
        ns["encode_ws"](ns["MsgType"].FOO, {"a": 1, "zz": 1})
    with pytest.raises(ns["ProtoError"]):  # known fields stay checked
        ns["decode_ws"]({"v": 1, "id": 1, "t": "foo", "d": {"a": 999, "zz": 1}})


def test_v036_decoder_accepts_main_hello_ack(tmp_path):
    """The 0.3.6 schema knew hello_ack without fw_build / ota_key."""
    old = (tmp_path / "old.yaml")
    old.write_text(
        "version: 1\nmessages:\n  - name: hello_ack\n    code: 2\n    transport: ws\n    fields:\n"
        "      - {name: hub_id, type: str}\n      - {name: fw_version, type: str, optional: true}\n"
    )
    out = tmp_path / "out"
    generate(old, out)
    ns = load_gen_py(out)
    env = {"v": 1, "id": 1, "t": "hello_ack",
           "d": {"hub_id": "h", "fw_version": "0.3.7", "fw_build": 366, "ota_key": "dev"}}
    assert ns["decode_ws"](env)[1] == {"hub_id": "h", "fw_version": "0.3.7"}
    with pytest.raises(ns["ProtoError"]):
        ns["decode_ws"]({**env, "d": {"fw_build": 366}})  # required field still enforced


def _v038_schema(tmp_path):
    """The real schema as 0.3.8 had it: no hello_ack.limits, no config_chunk."""
    import re as _re

    text = REAL_SCHEMA.read_text()
    text = _re.sub(r"^ *- \{name: limits,.*\n", "", text, flags=_re.MULTILINE)
    text = _re.sub(r"^  - name: config_chunk\n(?:    .*\n|      .*\n)+", "", text, flags=_re.MULTILINE)
    p = tmp_path / "v038.yaml"
    p.write_text(text)
    return p


LIMITS_HELLO = {
    "hub_id": "h", "fw_version": "0.3.9", "fw_build": 400, "ota_key": "release",
    "limits": {"budget": 8192, "flash_free": 4096, "chunk": 49152},
}


def test_v038_integration_decodes_hello_ack_with_limits(tmp_path):
    """A 0.3.8 integration (no limits, no config_chunk in its schema) against this hub."""
    out = tmp_path / "out"
    generate(_v038_schema(tmp_path), out)
    ns = load_gen_py(out)
    assert "CONFIG_CHUNK" not in ns["MsgType"].__members__
    got = ns["decode_ws"]({"v": 1, "id": 1, "t": "hello_ack", "d": LIMITS_HELLO})[1]
    assert got["hub_id"] == "h" and "limits" not in got and got["fw_build"] == 400


def test_this_integration_decodes_v038_hello_ack_and_is_refused():
    """This integration against a 0.3.8 hub: hello_ack without limits -> no valid limits."""
    from harmony_core.hub_limits import CURRENT, HubLimits

    ns = _real_py()
    env = {"v": 1, "id": 1, "t": "hello_ack",
           "d": {"hub_id": "h", "fw_version": "0.3.8", "fw_build": 366, "ota_key": "dev"}}
    data = ns["decode_ws"](env)[1]
    assert "limits" not in data
    with pytest.raises(ValueError):
        HubLimits.from_hello(data.get("limits"))
    wire = {k: v for k, v in CURRENT.to_wire().items() if k != "flash_free"}  # flash_free optional
    assert HubLimits.from_hello(wire) == CURRENT
