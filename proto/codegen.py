"""Generate C and Python protocol codecs from proto/schema.yaml (plain string templates)."""

from __future__ import annotations

import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Literal

import yaml

HERE = Path(__file__).resolve().parent
SCALARS = ("u8", "u16", "u32", "i32", "bool", "str", "bytes", "json")
TRANSPORTS = ("ws", "rf", "both")
_IDENT = re.compile(r"^[a-z][a-z0-9_]*$")


class SchemaError(ValueError):
    pass


@dataclass(frozen=True)
class Field:
    name: str
    type: str
    optional: bool


@dataclass(frozen=True)
class Message:
    name: str
    code: int
    transport: Literal["ws", "rf", "both"]
    fields: tuple[Field, ...]


SOFT_KEY_BASE = 128  # logical ids >= this are page soft keys (128 + widget id)
_KEY_NAME = re.compile(r"^[A-Z][A-Za-z0-9]*$")


@dataclass(frozen=True)
class Key:
    id: int  # logical code on the wire (u8, < SOFT_KEY_BASE)
    evdev: int  # Linux input code
    name: str  # stock name, e.g. VolumeUp
    event: str  # snake_case HA event type / YAML key name, e.g. volume_up


def snake(name: str) -> str:
    """VolumeUp -> volume_up, HomeControl1 -> home_control_1, DVR -> dvr."""
    return re.sub(r"(?<=[a-z])(?=[A-Z])|(?<=[A-Za-z])(?=[0-9])", "_", name).lower()


@dataclass(frozen=True)
class Schema:
    version: int
    messages: tuple[Message, ...]
    enums: tuple[tuple[str, tuple[tuple[str, int], ...]], ...] = ()
    keys: tuple[Key, ...] = ()


def _ident(kind: str, v: object) -> str:
    if not isinstance(v, str) or not _IDENT.match(v):
        raise SchemaError(f"bad {kind} name: {v!r}")
    return v


def load_schema(path: Path) -> Schema:
    raw = yaml.safe_load(Path(path).read_text())
    if not isinstance(raw, dict) or not isinstance(raw.get("version"), int):
        raise SchemaError("schema needs integer 'version'")
    enums: dict[str, tuple[tuple[str, int], ...]] = {}
    for ename, members in (raw.get("enums") or {}).items():
        _ident("enum", ename)
        if not isinstance(members, dict) or not members:
            raise SchemaError(f"enum {ename} needs members")
        pairs = []
        for mname, val in members.items():
            _ident("enum member", mname)
            if not isinstance(val, int) or isinstance(val, bool) or not 0 <= val <= 255:
                raise SchemaError(f"enum {ename}.{mname} must be 0..255")
            pairs.append((mname, val))
        if len({v for _, v in pairs}) != len(pairs):
            raise SchemaError(f"enum {ename} has duplicate values")
        enums[ename] = tuple(pairs)

    msgs, names, codes = [], set(), set()
    for m in raw.get("messages") or []:
        name = _ident("message", m.get("name"))
        code = m.get("code")
        if not isinstance(code, int) or isinstance(code, bool) or not 1 <= code <= 255:
            raise SchemaError(f"{name}: code must be 1..255")
        if name in names or code in codes:
            raise SchemaError(f"{name}: duplicate name or code {code}")
        names.add(name)
        codes.add(code)
        transport = m.get("transport")
        if transport not in TRANSPORTS:
            raise SchemaError(f"{name}: bad transport {transport!r}")
        fields, fnames = [], set()
        for f in m.get("fields") or []:
            fname = _ident("field", f.get("name"))
            ftype = f.get("type")
            if fname in fnames:
                raise SchemaError(f"{name}.{fname}: duplicate field")
            fnames.add(fname)
            if isinstance(ftype, str) and ftype.startswith("enum:"):
                if ftype[5:] not in enums:
                    raise SchemaError(f"{name}.{fname}: unknown enum {ftype[5:]!r}")
            elif ftype not in SCALARS:
                raise SchemaError(f"{name}.{fname}: unknown wire type {ftype!r}")
            if ftype == "json" and transport != "ws":
                raise SchemaError(f"{name}.{fname}: json is only legal in ws messages")
            fields.append(Field(fname, ftype, bool(f.get("optional", False))))
        msgs.append(Message(name, code, transport, tuple(fields)))
    keys: list[Key] = []
    kids: set[int] = set()
    kcodes: set[int] = set()
    knames: set[str] = set()
    kev: set[str] = set()
    for k in raw.get("keys") or []:
        kid, evdev, kname = k.get("id"), k.get("evdev"), k.get("name")
        if not isinstance(kid, int) or isinstance(kid, bool) or not 1 <= kid < SOFT_KEY_BASE:
            raise SchemaError(f"key {kname!r}: id must be 1..{SOFT_KEY_BASE - 1}")
        if not isinstance(evdev, int) or isinstance(evdev, bool) or not 1 <= evdev <= 0xFFFF:
            raise SchemaError(f"key {kname!r}: evdev must be 1..65535")
        if not isinstance(kname, str) or not _KEY_NAME.match(kname):
            raise SchemaError(f"bad key name: {kname!r}")
        event = snake(kname)
        if kid in kids or evdev in kcodes or kname in knames or event in kev:
            raise SchemaError(f"key {kname}: duplicate id, evdev code or name")
        kids.add(kid)
        kcodes.add(evdev)
        knames.add(kname)
        kev.add(event)
        keys.append(Key(kid, evdev, kname, event))
    return Schema(raw["version"], tuple(msgs), tuple(enums.items()), tuple(keys))


# ---------------------------------------------------------------- Python emitter

_PY_HEAD = '''\
"""Generated by proto/codegen.py from proto/schema.yaml. DO NOT EDIT."""

from __future__ import annotations

import base64
import binascii
from enum import IntEnum
from typing import Any

PROTO_VERSION = %(version)d


class ProtoError(ValueError):
    """Malformed or unsupported protocol message."""


class MsgType(IntEnum):
%(members)s


ENUMS: dict[str, dict[str, int]] = %(enums)r

# name -> (code, transport, ((field, wire type, optional), ...))
MESSAGES: dict[str, tuple[int, str, tuple[tuple[str, str, bool], ...]]] = {
%(messages)s
}

_INT_RANGES = {
    "u8": (0, 0xFF),
    "u16": (0, 0xFFFF),
    "u32": (0, 0xFFFFFFFF),
    "i32": (-(2**31), 2**31 - 1),
}
_BY_CODE = {code: name for name, (code, _t, _f) in MESSAGES.items()}

SOFT_KEY_BASE = 128  # logical ids >= this are page soft keys (128 + widget id)
# (logical id, evdev code, stock name, snake_case event type)
KEYS: tuple[tuple[int, int, str, str], ...] = (
%(keys)s
)
KEY_BY_ID = {k[0]: k for k in KEYS}
KEY_BY_EVDEV = {k[1]: k for k in KEYS}
KEY_BY_NAME = {k[2]: k for k in KEYS}
KEY_BY_EVENT = {k[3]: k for k in KEYS}
'''

_PY_TAIL = '''

def _check(msg: str, fname: str, ftype: str, value: Any) -> None:
    where = f"{msg}.{fname}"
    if ftype in _INT_RANGES:
        lo, hi = _INT_RANGES[ftype]
        if isinstance(value, bool) or not isinstance(value, int) or not lo <= value <= hi:
            raise ProtoError(f"{where}: expected {ftype}")
    elif ftype == "bool":
        if not isinstance(value, bool):
            raise ProtoError(f"{where}: expected bool")
    elif ftype == "str":
        if not isinstance(value, str):
            raise ProtoError(f"{where}: expected str")
    elif ftype == "bytes":  # base64 text on the ws
        if not isinstance(value, str):
            raise ProtoError(f"{where}: expected base64 str")
        try:
            base64.b64decode(value, validate=True)
        except (binascii.Error, ValueError) as exc:
            raise ProtoError(f"{where}: bad base64") from exc
    elif ftype == "json":
        if not isinstance(value, dict | list):
            raise ProtoError(f"{where}: expected JSON object or array")
    elif ftype.startswith("enum:"):
        allowed = ENUMS[ftype[5:]].values()
        if isinstance(value, bool) or not isinstance(value, int) or value not in allowed:
            raise ProtoError(f"{where}: bad {ftype} value")
    else:  # pragma: no cover - schema validated at generation time
        raise ProtoError(f"{where}: unknown type {ftype}")


def _validate(name: str, data: Any) -> dict[str, Any]:
    if name not in MESSAGES:
        raise ProtoError(f"unknown message {name!r}")
    _code, transport, fields = MESSAGES[name]
    if transport == "rf":
        raise ProtoError(f"{name} is not a ws message")
    if not isinstance(data, dict):
        raise ProtoError(f"{name}: 'd' must be an object")
    known = {f[0] for f in fields}
    for extra in data.keys() - known:
        raise ProtoError(f"{name}: unknown field {extra!r}")
    for fname, ftype, optional in fields:
        if fname not in data:
            if not optional:
                raise ProtoError(f"{name}: missing field {fname!r}")
            continue
        _check(name, fname, ftype, data[fname])
    return data


def encode_ws(msg_type: MsgType, data: dict[str, Any], msg_id: int = 0) -> dict[str, Any]:
    """Validate and wrap `data` in a ws envelope."""
    name = MsgType(msg_type).name.lower()
    _check("envelope", "id", "u32", msg_id)
    return {"v": PROTO_VERSION, "id": msg_id, "t": name, "d": dict(_validate(name, data))}


def decode_ws_full(env: Any) -> tuple[int, MsgType, dict[str, Any]]:
    """Validate an envelope; return (id, type, data)."""
    if not isinstance(env, dict):
        raise ProtoError("envelope must be an object")
    if env.get("v") != PROTO_VERSION or isinstance(env.get("v"), bool):
        raise ProtoError(f"unsupported protocol version {env.get('v')!r}")
    if "id" not in env:
        raise ProtoError("envelope missing id")
    _check("envelope", "id", "u32", env["id"])
    name = env.get("t")
    if not isinstance(name, str):
        raise ProtoError("envelope missing type")
    data = _validate(name, env.get("d", {}))
    return env["id"], MsgType[name.upper()], dict(data)


def decode_ws(env: Any) -> tuple[MsgType, dict[str, Any]]:
    """Validate an envelope; return (type, data)."""
    _id, msg_type, data = decode_ws_full(env)
    return msg_type, data
'''


def _emit_py(s: Schema) -> str:
    members = "\n".join(f"    {m.name.upper()} = {m.code}" for m in s.messages) or "    pass"
    msgs = "\n".join(
        f"    {m.name!r}: ({m.code}, {m.transport!r}, "
        f"({''.join(f'({f.name!r}, {f.type!r}, {f.optional}), ' for f in m.fields)})),"
        for m in s.messages
    )
    enums = {n: dict(p) for n, p in s.enums}
    keys = "\n".join(f"    ({k.id}, {k.evdev}, {k.name!r}, {k.event!r})," for k in s.keys)
    head = _PY_HEAD % {
        "version": s.version,
        "members": members,
        "enums": enums,
        "messages": msgs,
        "keys": keys,
    }
    return head + _PY_TAIL


# ---------------------------------------------------------------- C emitter

_C_TYPES = {
    "u8": "HP_T_U8", "u16": "HP_T_U16", "u32": "HP_T_U32", "i32": "HP_T_I32",
    "bool": "HP_T_BOOL", "str": "HP_T_STR", "bytes": "HP_T_BYTES", "json": "HP_T_JSON",
}  # fmt: skip
_TRANSPORT_C = {"ws": "HP_TRANSPORT_WS", "rf": "HP_TRANSPORT_RF", "both": "HP_TRANSPORT_BOTH"}

_H_HEAD = """\
/* Generated by proto/codegen.py from proto/schema.yaml. DO NOT EDIT. */
#ifndef HARMONY_PROTO_H
#define HARMONY_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HP_PROTO_VERSION %(version)d

typedef enum {
%(msg_enum)s
} hp_msg_type_t;

#define HP_MSG_COUNT %(count)d

%(schema_enums)s
typedef enum {
    HP_T_U8, HP_T_U16, HP_T_U32, HP_T_I32, HP_T_BOOL,
    HP_T_STR, HP_T_BYTES, HP_T_ENUM, HP_T_JSON
} hp_wire_type_t;

typedef enum {
    HP_TRANSPORT_WS = 1,
    HP_TRANSPORT_RF = 2,
    HP_TRANSPORT_BOTH = 3
} hp_transport_t;

typedef struct {
    const char *name;
    int32_t value;
} hp_enum_member_t;

typedef struct {
    const char *name;
    size_t count;
    const hp_enum_member_t *members;
} hp_enum_desc_t;

typedef struct {
    const char *name;
    hp_wire_type_t type;
    bool optional;
    const hp_enum_desc_t *enum_desc; /* non-NULL iff type == HP_T_ENUM */
} hp_field_desc_t;

typedef struct {
    const char *name;
    hp_msg_type_t type; /* == wire code */
    hp_transport_t transport;
    size_t field_count;
    const hp_field_desc_t *fields; /* NULL when field_count == 0 */
} hp_msg_desc_t;

/* NULL for an unknown type / name. */
const hp_msg_desc_t *hp_msg_desc(hp_msg_type_t type);
const hp_msg_desc_t *hp_msg_desc_by_name(const char *name);

/* Physical keys: logical id (the u8 in key_event.key, < 128) <-> evdev code <-> names. */
#define HP_KEY_COUNT %(key_count)d
#define HP_KEY_NONE 0xFF /* sentinel: no such key */
#define HP_SOFT_KEY_BASE 128 /* ids >= this are page soft keys */

typedef struct {
    uint8_t id;
    uint16_t evdev;
    const char *name;  /* stock name, e.g. "VolumeUp" */
    const char *event; /* snake_case HA event type, e.g. "volume_up" */
} hp_hwkey_t;

extern const hp_hwkey_t hp_hwkeys[HP_KEY_COUNT];

/* NULL for an unknown id / evdev code / name (names match the stock or event form, ASCII
 * case-insensitive). */
const hp_hwkey_t *hp_hwkey_by_id(uint8_t id);
const hp_hwkey_t *hp_hwkey_by_evdev(uint16_t evdev);
const hp_hwkey_t *hp_hwkey_by_name(const char *name);
/* evdev code -> logical id, or HP_KEY_NONE for a code that is not a Harmony key. */
uint8_t hp_key_from_evdev(uint16_t evdev);

#endif /* HARMONY_PROTO_H */
"""


def _emit_h(s: Schema) -> str:
    msg_enum = ",\n".join(f"    HP_MSG_{m.name.upper()} = {m.code}" for m in s.messages)
    parts = []
    for ename, members in s.enums:
        body = ",\n".join(f"    HP_{ename.upper()}_{n.upper()} = {v}" for n, v in members)
        parts.append(f"typedef enum {{\n{body}\n}} hp_{ename}_t;\n")
    return _H_HEAD % {
        "version": s.version,
        "msg_enum": msg_enum,
        "count": len(s.messages),
        "schema_enums": "\n".join(parts),
        "key_count": len(s.keys),
    }


def _emit_c_keys(s: Schema) -> list[str]:
    out = ["const hp_hwkey_t hp_hwkeys[HP_KEY_COUNT] = {"]
    out += [f'    {{{k.id}, {k.evdev}, "{k.name}", "{k.event}"}},' for k in s.keys]
    out += ["};", ""]
    for fn, arg, field in (
        ("hp_hwkey_by_id", "uint8_t id", "id"),
        ("hp_hwkey_by_evdev", "uint16_t evdev", "evdev"),
    ):
        out += [
            f"const hp_hwkey_t *{fn}({arg})",
            "{",
            "    for (size_t i = 0; i < HP_KEY_COUNT; i++) {",
            f"        if (hp_hwkeys[i].{field} == {field}) {{",
            "            return &hp_hwkeys[i];",
            "        }",
            "    }",
            "    return NULL;",
            "}",
            "",
        ]
    out += [
        "static bool hp_ieq(const char *a, const char *b)",
        "{",
        "    for (; *a && *b; a++, b++) {",
        "        char x = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;",
        "        char y = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;",
        "        if (x != y) {",
        "            return false;",
        "        }",
        "    }",
        "    return *a == *b;",
        "}",
        "",
        "const hp_hwkey_t *hp_hwkey_by_name(const char *name)",
        "{",
        "    if (name == NULL) {",
        "        return NULL;",
        "    }",
        "    for (size_t i = 0; i < HP_KEY_COUNT; i++) {",
        "        if (hp_ieq(hp_hwkeys[i].name, name) || hp_ieq(hp_hwkeys[i].event, name)) {",
        "            return &hp_hwkeys[i];",
        "        }",
        "    }",
        "    return NULL;",
        "}",
        "",
        "uint8_t hp_key_from_evdev(uint16_t evdev)",
        "{",
        "    const hp_hwkey_t *k = hp_hwkey_by_evdev(evdev);",
        "    return k ? k->id : HP_KEY_NONE;",
        "}",
        "",
    ]
    return out


def _emit_c(s: Schema) -> str:
    out = [
        "/* Generated by proto/codegen.py from proto/schema.yaml. DO NOT EDIT. */",
        '#include "harmony_proto.h"',
        "",
        "#include <string.h>",
        "",
    ]
    for ename, members in s.enums:
        out.append(f"static const hp_enum_member_t hp_enum_members_{ename}[] = {{")
        out += [f'    {{"{n}", {v}}},' for n, v in members]
        out.append("};")
        out.append(f"static const hp_enum_desc_t hp_enum_{ename} = {{")
        out.append(f'    "{ename}", {len(members)}, hp_enum_members_{ename}')
        out += ["};", ""]
    for m in s.messages:
        if not m.fields:
            continue
        out.append(f"static const hp_field_desc_t hp_fields_{m.name}[] = {{")
        for f in m.fields:
            if f.type.startswith("enum:"):
                t, e = "HP_T_ENUM", f"&hp_enum_{f.type[5:]}"
            else:
                t, e = _C_TYPES[f.type], "NULL"
            out.append(f'    {{"{f.name}", {t}, {str(f.optional).lower()}, {e}}},')
        out += ["};", ""]
    out.append("static const hp_msg_desc_t hp_msgs[] = {")
    for m in s.messages:
        fields = f"hp_fields_{m.name}" if m.fields else "NULL"
        out.append(
            f'    {{"{m.name}", HP_MSG_{m.name.upper()}, {_TRANSPORT_C[m.transport]}, '
            f"{len(m.fields)}, {fields}}},"
        )
    out += [
        "};",
        "",
        *_emit_c_keys(s),
        "const hp_msg_desc_t *hp_msg_desc(hp_msg_type_t type)",
        "{",
        "    for (size_t i = 0; i < sizeof hp_msgs / sizeof hp_msgs[0]; i++) {",
        "        if (hp_msgs[i].type == type) {",
        "            return &hp_msgs[i];",
        "        }",
        "    }",
        "    return NULL;",
        "}",
        "",
        "const hp_msg_desc_t *hp_msg_desc_by_name(const char *name)",
        "{",
        "    if (name == NULL) {",
        "        return NULL;",
        "    }",
        "    for (size_t i = 0; i < sizeof hp_msgs / sizeof hp_msgs[0]; i++) {",
        "        if (strcmp(hp_msgs[i].name, name) == 0) {",
        "            return &hp_msgs[i];",
        "        }",
        "    }",
        "    return NULL;",
        "}",
        "",
    ]
    return "\n".join(out)


HA_DIR = HERE.parent / "custom_components" / "ha_harmony"


def generate(schema: Path, out_dir: Path, ha_dir: Path | None = None) -> None:
    s = load_schema(schema)
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "harmony_proto.h").write_text(_emit_h(s))
    (out_dir / "harmony_proto.c").write_text(_emit_c(s))
    py = _emit_py(s)
    (out_dir / "harmony_proto.py").write_text(py)
    if ha_dir is not None:  # vendored copy: HA can't import the repo-level `proto` package
        Path(ha_dir).mkdir(parents=True, exist_ok=True)
        (Path(ha_dir) / "harmony_proto.py").write_text(py)


def main(argv: list[str] | None = None) -> int:
    args = sys.argv[1:] if argv is None else argv
    schema = Path(args[0]) if args else HERE / "schema.yaml"
    out = Path(args[1]) if len(args) > 1 else HERE / "gen"
    generate(schema, out, HA_DIR if not args else None)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
