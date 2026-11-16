"""User config file `<HA config dir>/ha_harmony/<hub_id>.yaml`: schema, IR resolution, validation.

`parse_model` returns a plain validated dict (the "model"); every IR code in it is already
resolved to a config-format-2 IR action, so `compiler.compile_config` is a pure function of it.
Errors are `ConfigError`s that name the file, line and YAML location. No Home Assistant imports.
"""

from __future__ import annotations

import re
from pathlib import Path
from typing import Any

import voluptuous as vol
import yaml

from .const import KEY_CODES, SOFT_KEY_BASE, key_code
from .ir import IrCode, IrImportError, parse_flipper_file, parse_smartir_file

Model = dict[str, Any]


class _Loader(yaml.SafeLoader):
    """SafeLoader with YAML 1.2 booleans: only true/false, so `on:` / `off:` stay plain keys."""


_Loader.yaml_implicit_resolvers = {
    ch: [(tag, rx) for tag, rx in rs if tag != "tag:yaml.org,2002:bool"]
    for ch, rs in yaml.SafeLoader.yaml_implicit_resolvers.items()
}
_Loader.add_implicit_resolver(
    "tag:yaml.org,2002:bool", re.compile(r"^(?:true|false|True|False|TRUE|FALSE)$"), list("tfTF")
)

_SLUG = re.compile(r"^[a-z0-9_]{1,31}$")
_ENTITY = re.compile(r"^[a-z0-9_]+\.[a-z0-9_]+$")
_IR_FORMS = ("proto", "raw", "pronto", "flipper", "smartir")


class ConfigError(ValueError):
    """A problem in the user's YAML; the message starts with `<file>:<line>: <location>`."""

    def __init__(self, file: str, path: list[Any], msg: str, line: int | None = None) -> None:
        self.file, self.path, self.msg, self.line = file, path, msg, line
        where = file + (f":{line}" if line else "")
        super().__init__(f"{where}: {fmt_path(path)}: {msg}" if path else f"{where}: {msg}")


def fmt_path(path: list[Any]) -> str:
    out = ""
    for p in path:
        out += f"[{p}]" if isinstance(p, int) else (f".{p}" if out else str(p))
    return out


def _line_of(root: yaml.Node | None, path: list[Any]) -> int | None:
    node = root
    for p in path:
        nxt = None
        if isinstance(node, yaml.MappingNode):
            nxt = next((v for k, v in node.value if str(k.value) == str(p)), None)
        elif isinstance(node, yaml.SequenceNode) and isinstance(p, int) and p < len(node.value):
            nxt = node.value[p]
        if nxt is None:
            break
        node = nxt
    return node.start_mark.line + 1 if node is not None else None


# ---------------------------------------------------------------- IR / action validators
class _Resolver:
    """Resolves `flipper:` / `smartir:` references relative to the YAML file's directory."""

    def __init__(self, base: Path | None) -> None:
        self.base = base
        self._cache: dict[tuple[str, Path], dict[str, IrCode]] = {}

    def _file(self, kind: str, name: Any) -> dict[str, IrCode]:
        if self.base is None:
            raise vol.Invalid(f"{kind} references need a config directory")
        if not isinstance(name, str) or not name:
            raise vol.Invalid(f"{kind}.file must be a file name")
        base = self.base.resolve()
        path = (base / name).resolve()
        if not path.is_relative_to(base):
            raise vol.Invalid(f"{kind} file {name!r} must stay inside {base}")
        key = (kind, path)
        if key not in self._cache:
            try:
                parse = parse_flipper_file if kind == "flipper" else parse_smartir_file
                self._cache[key] = parse(path)
            except OSError as exc:
                raise vol.Invalid(f"cannot read {kind} file {name!r}: {exc.strerror}") from None
            except IrImportError as exc:
                raise vol.Invalid(f"{kind} file {name!r}: {exc}") from None
        return self._cache[key]

    def ir(self, v: Any) -> dict[str, Any]:
        if not isinstance(v, dict):
            raise vol.Invalid("an IR action must be a mapping")
        forms = [k for k in _IR_FORMS if k in v]
        extra = set(v) - set(_IR_FORMS) - {"port_mask"}
        if "proto" not in v:
            extra = extra | ({"addr", "cmd"} & set(v))  # addr/cmd only belong to proto
        else:
            extra -= {"addr", "cmd"}
        if len(forms) != 1 or extra:
            raise vol.Invalid(
                f"expected exactly one of proto/addr/cmd, raw, pronto, flipper, smartir"
                f"{f' (unexpected: {sorted(extra)})' if extra else ''}"
            )
        form = forms[0]
        try:
            if form == "proto":
                if set(v) - {"proto", "addr", "cmd", "port_mask"} or not {"addr", "cmd"} <= set(v):
                    raise vol.Invalid("proto needs addr and cmd")
                out = IrCode(proto=v["proto"], addr=v["addr"], cmd=v["cmd"]).to_hub_json()
            elif form == "raw":
                raw = v["raw"]
                if not isinstance(raw, dict) or set(raw) != {"hz", "us"}:
                    raise vol.Invalid("raw needs hz and us")
                if not isinstance(raw["us"], list):
                    raise vol.Invalid("raw.us must be a list of microseconds")
                out = IrCode(hz=raw["hz"], us=tuple(raw["us"])).to_hub_json()
            elif form == "pronto":
                if not isinstance(v["pronto"], str):
                    raise vol.Invalid("pronto must be a string of hex words")
                out = IrCode(pronto=v["pronto"]).to_hub_json()
            else:
                ref = v[form]
                field = "signal" if form == "flipper" else "command"
                if not isinstance(ref, dict) or set(ref) != {"file", field}:
                    raise vol.Invalid(f"{form} needs file and {field}")
                codes = self._file(form, ref["file"])
                if ref[field] not in codes:
                    raise vol.Invalid(f"{form} file {ref['file']!r} has no {field} {ref[field]!r}")
                out = codes[ref[field]].to_hub_json()
        except IrImportError as exc:
            raise vol.Invalid(str(exc)) from None
        if "port_mask" in v:
            pm = v["port_mask"]
            if isinstance(pm, bool) or not isinstance(pm, int) or not 1 <= pm <= 255:
                raise vol.Invalid("port_mask must be 1..255")
            out["port_mask"] = pm
        return out

    def action(self, v: Any, *, allow_activity: bool = True) -> dict[str, Any]:
        if not isinstance(v, dict):
            raise vol.Invalid("an action must be a mapping")
        if "device" in v:
            if set(v) - {"device", "command", "input"} or ("command" in v) == ("input" in v):
                raise vol.Invalid("device action needs device plus exactly one of command/input")
            kind = "command" if "command" in v else "input"
            return {"type": "device", "device": _slug(v["device"]), kind: _name(v[kind])}
        if "activity" in v:
            if set(v) != {"activity"}:
                raise vol.Invalid("activity action takes only `activity`")
            if not allow_activity:
                raise vol.Invalid("activity actions are only allowed in key maps and page buttons")
            return {"type": "activity", "activity": v["activity"]}  # checked in cross-refs
        if "event" in v:
            if set(v) != {"event"}:
                raise vol.Invalid("event action takes only `event`")
            return {"type": "ha", "event": _name(v["event"])}
        if "bt" in v:
            usage = v["bt"]
            if set(v) != {"bt"} or isinstance(usage, bool) or not isinstance(usage, int):
                raise vol.Invalid("bt action takes an integer HID usage")
            if not 0 <= usage <= 65535:
                raise vol.Invalid("bt usage must be 0..65535")
            return {"type": "bt", "usage": usage}
        if "none" in v:
            return {"type": "none"}
        return {"type": "ir", **self.ir(v)}


def _slug(v: Any) -> str:
    if not isinstance(v, str) or not _SLUG.match(v):
        raise vol.Invalid("must be 1-31 characters of a-z, 0-9 or _")
    return v


def _name(v: Any) -> str:
    if not isinstance(v, str) or not v or any(ord(c) < 32 for c in v):
        raise vol.Invalid("must be a non-empty text")
    if len(v.encode()) > 31:
        raise vol.Invalid("must be at most 31 bytes")
    return v


def _cmd_name(v: Any) -> str:
    """A device command name: free text, but not one the hub resolves itself."""
    v = _name(v)
    if v in ("power_on", "power_off") or v.startswith("input_"):
        raise vol.Invalid(
            f"command name {v!r} is reserved (power_on, power_off and input_<name>"
            " are built in); pick another name"
        )
    return v


def _int(lo: int, hi: int):
    def check(v: Any) -> int:
        if isinstance(v, bool) or not isinstance(v, int) or not lo <= v <= hi:
            raise vol.Invalid(f"must be an integer {lo}..{hi}")
        return v

    return check


def _keyname(v: Any) -> str:
    if isinstance(v, int) and not isinstance(v, bool) and 0 <= v < SOFT_KEY_BASE:
        return f"key_{v}"
    if isinstance(v, str) and key_code(v) is not None:
        return v
    raise vol.Invalid(
        f"unknown key {v!r}; use one of {', '.join(KEY_CODES)} or key_<0..{SOFT_KEY_BASE - 1}>"
    )


def _build_schema(r: _Resolver) -> vol.Schema:
    ir, act = r.ir, r.action
    step = vol.All(lambda v: act(v, allow_activity=False))
    keys = vol.Schema({_keyname: act})
    power = vol.All(
        vol.Schema({vol.Optional("on"): ir, vol.Optional("off"): ir, vol.Optional("toggle"): ir}),
        _check_power,
    )
    device = {
        vol.Required("id"): _slug,
        vol.Optional("name"): _name,
        vol.Optional("power"): power,
        vol.Optional("inputs", default=dict): {_name: ir},
        vol.Optional("settle_ms"): _int(0, 60000),
        vol.Optional("commands", default=dict): {_cmd_name: ir},
    }
    activity = {
        vol.Required("id"): vol.All(_slug, vol.NotIn(["off"], msg="`off` is reserved")),
        vol.Optional("name"): _name,
        vol.Optional("devices", default=list): [
            vol.Schema({vol.Required("device"): _slug, vol.Optional("input"): _name})
        ],
        vol.Optional("steps", default=list): [step],
        vol.Optional("off", default=list): [step],
        vol.Optional("keys", default=dict): keys,
    }
    widget = {
        vol.Required("type"): vol.In(["button", "tile", "label"]),
        vol.Optional("id"): _slug,
        vol.Required("x"): _int(0, 15),
        vol.Required("y"): _int(0, 15),
        vol.Optional("w", default=1): _int(1, 15),
        vol.Optional("h", default=1): _int(1, 15),
        vol.Optional("label", default=""): str,
        vol.Optional("action"): act,
        vol.Optional("entity"): vol.All(str, vol.Match(_ENTITY, msg="not an entity id")),
    }
    page = {
        vol.Required("id"): _slug,
        vol.Required("cols"): _int(1, 16),
        vol.Required("rows"): _int(1, 16),
        vol.Required("widgets"): [vol.All(widget, _check_widget)],
    }
    return vol.Schema(
        {
            vol.Optional("devices", default=list): [device],
            vol.Optional("activities", default=list): [activity],
            vol.Optional("keys", default=dict): keys,
            vol.Optional("pages", default=list): [page],
        }
    )


def _check_power(p: dict[str, Any]) -> dict[str, Any]:
    if set(p) not in ({"on", "off"}, {"toggle"}):
        raise vol.Invalid("power needs both `on` and `off`, or just `toggle`")
    return p


def _check_widget(w: dict[str, Any]) -> dict[str, Any]:
    kind = w["type"]
    if kind != "label" and "id" not in w:
        raise vol.Invalid(f"a {kind} widget needs an `id`")
    if kind == "button" and "action" not in w:
        raise vol.Invalid("a button needs an `action`")
    if kind == "tile" and "entity" not in w:
        raise vol.Invalid("a tile needs an `entity`")
    if kind != "button" and "action" in w or kind != "tile" and "entity" in w:
        raise vol.Invalid(f"unexpected `{'action' if 'action' in w else 'entity'}` on a {kind}")
    return w


# ---------------------------------------------------------------- cross references
def _check_refs(model: Model) -> None:
    """Raises (path, message) tuples via _RefError for dangling or duplicate references."""
    devs = {d["id"]: d for d in model["devices"]}
    acts = {a["id"] for a in model["activities"]}

    def uniq(items: list[dict[str, Any]], section: str, what: str) -> None:
        seen: set[str] = set()
        for i, it in enumerate(items):
            if it["id"] in seen:
                raise _RefError([section, i, "id"], f"duplicate {what} id {it['id']!r}")
            seen.add(it["id"])

    uniq(model["devices"], "devices", "device")
    uniq(model["activities"], "activities", "activity")
    uniq(model["pages"], "pages", "page")

    def check_action(a: dict[str, Any], path: list[Any]) -> None:
        if a["type"] == "device":
            d = devs.get(a["device"])
            if d is None:
                raise _RefError([*path, "device"], f"unknown device {a['device']!r}")
            if "command" in a:
                ok = a["command"] in d["commands"] or (
                    a["command"] in ("power_on", "power_off") and "power" in d
                )
                if not ok:
                    raise _RefError([*path, "command"], f"{a['device']} has no {a['command']!r}")
            elif a["input"] not in d["inputs"]:
                raise _RefError([*path, "input"], f"{a['device']} has no input {a['input']!r}")
        elif a["type"] == "activity" and a["activity"] != "off" and a["activity"] not in acts:
            raise _RefError([*path, "activity"], f"unknown activity {a['activity']!r}")

    for i, a in enumerate(model["activities"]):
        base = ["activities", i]
        for j, ad in enumerate(a["devices"]):
            if ad["device"] not in devs:
                raise _RefError([*base, "devices", j, "device"], f"unknown device {ad['device']!r}")
            if "input" in ad and ad["input"] not in devs[ad["device"]]["inputs"]:
                raise _RefError([*base, "devices", j, "input"], f"no input {ad['input']!r}")
        for sect in ("steps", "off"):
            for j, s in enumerate(a[sect]):
                check_action(s, [*base, sect, j])
        for k, v in a["keys"].items():
            check_action(v, [*base, "keys", k])
    for k, v in model["keys"].items():
        check_action(v, ["keys", k])
    seen: set[str] = set()
    for i, p in enumerate(model["pages"]):
        for j, w in enumerate(p["widgets"]):
            if "id" in w:
                if w["id"] in seen:
                    raise _RefError(
                        ["pages", i, "widgets", j, "id"], f"duplicate widget id {w['id']!r}"
                    )
                seen.add(w["id"])
            if "action" in w:
                check_action(w["action"], ["pages", i, "widgets", j, "action"])


class _RefError(Exception):
    def __init__(self, path: list[Any], msg: str) -> None:
        super().__init__(msg)
        self.path, self.msg = path, msg


# ---------------------------------------------------------------- entry points
def parse_model(text: str, file: str = "<config>", base_dir: Path | None = None) -> Model:
    """Validate YAML `text`. `file` is only used in messages; `base_dir` anchors IR file refs."""
    try:
        root = yaml.compose(text, Loader=_Loader)
        data = yaml.load(text, Loader=_Loader)
    except yaml.YAMLError as exc:
        mark = getattr(exc, "problem_mark", None)
        raise ConfigError(
            file,
            [],
            f"invalid YAML: {getattr(exc, 'problem', exc)}",
            mark.line + 1 if mark else None,
        ) from None
    if data is None:
        data = {}
    if not isinstance(data, dict):
        raise ConfigError(file, [], "the top level must be a mapping", 1)
    try:
        model = _build_schema(_Resolver(base_dir))(data)
        _check_refs(model)
    except vol.MultipleInvalid as exc:
        err = exc.errors[0]
        path = list(err.path)
        raise ConfigError(file, path, err.error_message, _line_of(root, path)) from None
    except vol.Invalid as exc:
        path = list(exc.path)
        raise ConfigError(file, path, exc.error_message, _line_of(root, path)) from None
    except _RefError as exc:
        raise ConfigError(file, exc.path, exc.msg, _line_of(root, exc.path)) from None
    return model


def load_file(path: Path) -> Model | None:
    """Read + validate a config file (blocking IO). None if the file does not exist."""
    try:
        text = path.read_text(encoding="utf-8")
    except FileNotFoundError:
        return None
    except (OSError, UnicodeDecodeError) as exc:
        raise ConfigError(str(path), [], f"cannot read file: {exc}") from None
    return parse_model(text, str(path), path.parent)


def validate_ir(value: Any, base_dir: Path | None = None) -> dict[str, Any]:
    """Validate/resolve one IR action (for the send_ir service); raises vol.Invalid."""
    return _Resolver(base_dir).ir(value)
