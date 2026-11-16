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
        {"v": 1, "id": 1, "t": "foo", "d": {"a": 1, "zz": 1}},  # unknown field
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
    }  # fmt: skip


def test_committed_generated_files_are_fresh(tmp_path):
    generate(REAL_SCHEMA, tmp_path)
    for n in ("harmony_proto.h", "harmony_proto.c", "harmony_proto.py"):
        assert (tmp_path / n).read_text() == (GEN_DIR / n).read_text(), (
            f"{n} stale: run `uv run python -m proto.codegen`"
        )
