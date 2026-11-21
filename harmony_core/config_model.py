"""Config document: schema, IR resolution, validation (YAML text or JSON, shared add-on/integration).

`parse_model` returns a plain validated dict (the "model"); every IR code in it is already
resolved to a config-format-2 IR action, so `compiler.compile_config` is a pure function of it.
Errors are `ConfigError`s that name the file, line and YAML location. No Home Assistant imports.
"""

from __future__ import annotations

import json
import re
from pathlib import Path
from typing import Any

import voluptuous as vol
import yaml

from .bt_codes import (
    BT_MAX_HOSTS,
    BT_MAX_REMOTES,
    DEFAULT_COMMANDS,
    DEFAULT_REMOTE_KEYS,
    REMOTE_MAX_KEYS,
    TRANSPORTS,
    BtCodeError,
    norm_address,
)
from .bt_codes import (
    resolve as bt_resolve,
)
from .ha_actions import default_tap, validate_ha
from .harmony_proto import SOFT_KEY_BASE
from .icons import ICON_IDS
from .ir import IrCode, IrImportError, parse_flipper_file, parse_smartir_file
from .keys import KEY_CODES, key_code
from .theme_math import MAX_DEPTH, PALETTE_NAMES, TOKEN_NAMES, ThemeError, resolve_theme
from .themes import PRESETS

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

MAX_FLOW_DEPTH = 64  # a real config nests ~10 deep; PyYAML's scanner is quadratic in flow depth


def check_yaml_depth(text: str, file: str = "<config>") -> None:
    """Refuse absurd `[`/`{` nesting with one linear scan (an over-approximation: quotes count)."""
    depth = 0
    for ch in text:
        if ch in "[{":
            depth += 1
            if depth > MAX_FLOW_DEPTH:
                raise ConfigError(file, [], "invalid YAML: nested too deeply")
        elif ch in "]}" and depth:
            depth -= 1


def load_source(text: str) -> Any:
    """Parse config YAML to plain data with the loader `parse_model` uses (no validation)."""
    check_yaml_depth(text)
    try:
        return yaml.load(text, Loader=_Loader)
    except yaml.YAMLError as exc:
        raise ConfigError("<config>", [], f"invalid YAML: {getattr(exc, 'problem', exc)}") from None
    except RecursionError:
        raise ConfigError("<config>", [], "invalid YAML: nested too deeply") from None


_SLUG = re.compile(r"^[a-z0-9_]{1,31}$")
_ENTITY = re.compile(r"^[a-z0-9_]+\.[a-z0-9_]+$")
_IR_FORMS = ("proto", "raw", "pronto", "flipper", "smartir")
_NAV = re.compile(r"^(?:[a-z0-9_]{1,31})$")  # next | prev | home | <page id>
ROLES = ("default", "primary", "secondary", "danger")  # `nav` comes only from the `nav:` key
SHAPES = ("card", "row", "pill", "monogram")
NAV_WORDS = ("next", "prev", "home")


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
            except UnicodeDecodeError:
                raise vol.Invalid(f"{kind} file {name!r}: not UTF-8 text") from None
            except IrImportError as exc:
                raise vol.Invalid(f"{kind} file {name!r}: {exc}") from None
        return self._cache[key]

    def ir(self, v: Any) -> dict[str, Any]:
        if not isinstance(v, dict):
            raise vol.Invalid("an IR action must be a mapping")
        forms = [k for k in _IR_FORMS if k in v]
        extra = set(v) - set(_IR_FORMS) - {"port_mask", "repeats"}
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
                if set(v) - {"proto", "addr", "cmd", "port_mask", "repeats"} or not {
                    "addr",
                    "cmd",
                } <= set(v):
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
        if "repeats" in v:
            rp = v["repeats"]
            if isinstance(rp, bool) or not isinstance(rp, int) or not 1 <= rp <= 8:
                raise vol.Invalid("repeats must be 1..8")
            if rp > 1:  # 1 is the default: keep the hub JSON (and its hash) unchanged
                out["repeats"] = rp
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
        if "ha" in v:
            if set(v) != {"ha"}:
                raise vol.Invalid("ha action takes only `ha`")
            return validate_ha(v["ha"])
        if "none" in v:
            return {"type": "none"}
        return {"type": "ir", **self.ir(v)}

    def ha_only(self, v: Any) -> dict[str, Any]:
        """A step of a Home Assistant device: only `{ha: ...}` is accepted."""
        if not isinstance(v, dict) or set(v) != {"ha"}:
            raise vol.Invalid(
                "a Home Assistant device takes `{ha: {service, target, ...}}` actions"
            )
        return validate_ha(v["ha"])


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


def _bt_address(v: Any) -> str:
    if isinstance(v, int) and not isinstance(v, bool):  # YAML reads 11:22:33:44:55:66 as a number
        raise vol.Invalid('quote the Bluetooth address: "11:22:33:44:55:66"')
    try:
        return norm_address(v)
    except BtCodeError as exc:
        raise vol.Invalid(str(exc)) from None


def _bt_command(v: Any) -> dict[str, Any]:
    """A bt device command: a built-in name or `HID:<page hex>:<usage hex>` -> a bt action (the
    compiler adds the host slot)."""
    try:
        page, usage = bt_resolve(v)
    except BtCodeError as exc:
        raise vol.Invalid(str(exc)) from None
    return {"type": "bt", "page": page, "usage": usage}


def _bt_remote_keys(v: Any) -> list[list[Any]]:
    """`{usage name | HID:pp:uuuu: key name}` merged over the defaults -> [[page, usage, key], ...]
    sorted by (page, usage). The key names are the Elite's own (`up`, `volume_up`, `key_<n>`)."""
    if v is None:
        v = {}
    if not isinstance(v, dict):
        raise vol.Invalid("keys must be a mapping of usage -> key name")
    out: dict[tuple[int, int], str] = {}
    for spec, key in DEFAULT_REMOTE_KEYS:
        out[bt_resolve(spec)] = key
    seen: dict[tuple[int, int], str] = {}
    for spec, key in v.items():
        try:
            pu = bt_resolve(spec)
        except BtCodeError as exc:
            raise vol.Invalid(f"{spec!r}: {exc}") from None
        if pu in seen:
            raise vol.Invalid(f"{spec!r} is the same usage as {seen[pu]!r}")
        seen[pu] = spec
        if key is None:  # `Usage: null` (a default removed): that button does nothing
            out.pop(pu, None)
        else:
            out[pu] = _keyname(key)
    if len(out) > REMOTE_MAX_KEYS:
        raise vol.Invalid(
            f"too many keys: {len(out)} (at most {REMOTE_MAX_KEYS}, defaults included)"
        )
    return [[p, u, k] for (p, u), k in sorted(out.items())]


def _title(v: Any) -> str:
    if not isinstance(v, str) or "\x00" in v:
        raise vol.Invalid("must be text")
    if len(v.encode()) > 24:
        raise vol.Invalid("must be at most 24 bytes")
    return v


_HEX6 = re.compile(r"^#[0-9a-fA-F]{6}$")


def _hexcol(v: Any) -> str:
    if not isinstance(v, str) or not _HEX6.match(v):
        raise vol.Invalid(f"bad colour {v!r}; want #RRGGBB (six hex digits)")
    return v.upper()


def _color(v: Any) -> str:
    """A widget colour: a palette name or a free `#RRGGBB`."""
    if isinstance(v, str):
        if v in PALETTE_NAMES:
            return v
        if v.startswith("#"):
            return _hexcol(v)
    raise vol.Invalid(f"unknown colour {v!r}; one of: {', '.join(PALETTE_NAMES)}, or #RRGGBB")


def _token_key(k: Any) -> str:
    if not isinstance(k, str) or k.lower() not in TOKEN_NAMES:
        raise vol.Invalid(f"unknown token {k!r}; one of: {', '.join(TOKEN_NAMES)}")
    return k.lower()


def _palette_key(k: Any) -> int:
    try:
        n = int(k) if isinstance(k, (int, str)) and not isinstance(k, bool) else 0
    except ValueError:
        n = 0
    if not 1 <= n <= len(PALETTE_NAMES):
        raise vol.Invalid(f"palette index {k!r} must be 1..{len(PALETTE_NAMES)}")
    return n


_THEME = {
    vol.Optional("extends", default="default"): vol.All(str, vol.Length(min=1, max=31)),
    vol.Optional("tokens", default=dict): {_token_key: _hexcol},
    vol.Optional("palette", default=dict): {_palette_key: {vol.Required("fill"): _hexcol}},
}


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
    ha = r.ha_only
    ha_device = vol.Schema(
        {
            vol.Required("id"): _slug,
            vol.Required("kind"): "ha",
            vol.Optional("name"): _name,
            vol.Optional("aliases", default=list): [_name],
            vol.Optional("icon"): vol.In(list(ICON_IDS), msg="unknown icon"),
            # The entity whose state is the device's state (on = not off/standby/unavailable).
            vol.Optional("entity"): vol.All(str, vol.Match(_ENTITY, msg="not an entity id")),
            vol.Optional("power"): vol.All(
                vol.Schema({vol.Optional("on"): ha, vol.Optional("off"): ha}), _check_ha_power
            ),
            vol.Optional("inputs", default=dict): {_name: ha},
            vol.Optional("settle_ms"): _int(0, 60000),
            vol.Optional("source"): {str: vol.Any(str, int, float, bool, None)},
            vol.Optional("commands", default=dict): {_cmd_name: ha},
        }
    )
    device = {
        vol.Optional("kind"): "ir",
        vol.Required("id"): _slug,
        vol.Optional("name"): _name,
        # Other names this device answers to in `remote.send_command` / `set_device_state`
        # (e.g. the stock hub's numeric device id, so existing automations keep working).
        # Resolved in Home Assistant; never sent to the hub.
        vol.Optional("aliases", default=list): [_name],
        # Stock icon shown for the device in the editor and the default for widgets that target it.
        vol.Optional("icon"): vol.In(list(ICON_IDS), msg="unknown icon"),
        vol.Optional("power"): power,
        vol.Optional("inputs", default=dict): {_name: ir},
        vol.Optional("settle_ms"): _int(0, 60000),
        # Provenance of an imported device (tools/irdb_import.py); never compiled or sent.
        vol.Optional("source"): {str: vol.Any(str, int, float, bool, None)},
        vol.Optional("commands", default=dict): {_cmd_name: ir},
    }
    ir_device = vol.Schema(device)
    bt_device = vol.Schema(
        {
            vol.Required("id"): _slug,
            vol.Required("kind"): "bt",
            vol.Optional("name"): _name,
            vol.Optional("aliases", default=list): [_name],
            vol.Optional("icon"): vol.In(list(ICON_IDS), msg="unknown icon"),
            vol.Optional("address"): _bt_address,  # none until the host is paired
            vol.Optional("transport", default="auto"): vol.In(TRANSPORTS),
            vol.Optional("commands"): {_cmd_name: _bt_command},  # default: every built-in
        }
    )

    def bt_dev(v: Any) -> dict[str, Any]:
        out = bt_device(v)
        if "commands" not in out:
            out["commands"] = {n: _bt_command(n) for n in DEFAULT_COMMANDS}
        out["inputs"] = {}  # a bt device has no inputs and no power codes
        return out

    def any_device(v: Any) -> dict[str, Any]:
        kind = v.get("kind") if isinstance(v, dict) else None
        if kind == "ha":
            return ha_device(v)
        return bt_dev(v) if kind == "bt" else ir_device(v)

    bt_remote = vol.Schema(
        {
            vol.Required("name"): _name,
            vol.Optional("address"): _bt_address,
            vol.Optional("keys"): vol.Any(None, dict),
        }
    )

    def bt_rem(v: Any) -> dict[str, Any]:
        out = bt_remote(v)
        out["keys"] = _bt_remote_keys(out.get("keys"))
        return out

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
        vol.Optional("x"): _int(0, 15),
        vol.Optional("y"): _int(0, 15),
        vol.Optional("w", default=1): _int(1, 15),
        vol.Optional("h", default=1): _int(1, 15),
        vol.Optional("label", default=""): str,
        vol.Optional("action"): act,
        vol.Optional("tap"): _tap(act),
        vol.Optional("entity"): vol.All(str, vol.Match(_ENTITY, msg="not an entity id")),
        vol.Optional("icon"): vol.In(["none", *ICON_IDS], msg="unknown icon"),
        vol.Optional("role"): vol.In(ROLES),
        vol.Optional("shape"): vol.In(SHAPES),
        vol.Optional("icon_only"): bool,
        vol.Optional("align"): vol.In(["left", "center"]),
        vol.Optional("color"): _color,
        vol.Optional("on_color"): _color,
        vol.Optional("nav"): vol.All(str, vol.Match(_NAV, msg="use next, prev, home or a page id")),
    }
    page = {
        vol.Required("id"): _slug,
        vol.Optional("cols"): _int(1, 16),
        vol.Optional("rows"): _int(1, 16),
        vol.Optional("kind"): vol.In(["home", "favorites"]),
        vol.Optional("title"): _title,
        vol.Optional("activity"): _slug,
        vol.Optional("back"): bool,
        vol.Optional("list"): {vol.Required("rows"): _int(1, 16)},
        vol.Required("widgets"): [vol.All(widget, _check_widget)],
    }
    return vol.Schema(
        {
            vol.Optional("devices", default=list): [any_device],
            vol.Optional("activities", default=list): [activity],
            vol.Optional("keys", default=dict): keys,
            vol.Optional("bt_remotes"): [bt_rem],
            vol.Optional("pages", default=list): [vol.All(page, _check_page)],
            # User themes (`extends` a preset or another user theme) and the one active theme.
            vol.Optional("themes"): {_slug: _THEME},
            vol.Optional("theme"): _slug,
            # Opaque editor metadata: bounded, never compiled, never sent to the hub.
            vol.Optional("editor"): _editor,
        }
    )


EDITOR_MAX_BYTES = 65536
EDITOR_MAX_NODES = 5000
EDITOR_MAX_DEPTH = 8


def _editor(v: Any) -> dict[str, Any]:
    """`editor:` is a JSON-shaped mapping with capped size, node count and depth."""
    if not isinstance(v, dict):
        raise vol.Invalid("editor must be a mapping")
    nodes = 0
    stack: list[tuple[Any, int]] = [(v, 1)]
    while stack:
        cur, depth = stack.pop()
        nodes += 1
        if nodes > EDITOR_MAX_NODES:
            raise vol.Invalid("editor metadata is too large")
        if depth > EDITOR_MAX_DEPTH:
            raise vol.Invalid(f"editor metadata is nested deeper than {EDITOR_MAX_DEPTH}")
        if isinstance(cur, dict):
            for k, x in cur.items():
                if not isinstance(k, str) or len(k) > 100:
                    raise vol.Invalid("editor keys must be short text")
                stack.append((x, depth + 1))
        elif isinstance(cur, list):
            stack.extend((x, depth + 1) for x in cur)
        elif isinstance(cur, str):
            if len(cur) > 2000:
                raise vol.Invalid("editor text is too long")
        elif cur is not None and not isinstance(cur, (bool, int, float)):
            raise vol.Invalid("editor metadata must be plain JSON data")
    try:
        size = len(json.dumps(v, allow_nan=False))
    except TypeError, ValueError:
        raise vol.Invalid("editor metadata must be plain JSON data") from None
    if size > EDITOR_MAX_BYTES:
        raise vol.Invalid(f"editor metadata is over {EDITOR_MAX_BYTES} bytes")
    return v


def validate_pages(config: dict[str, Any]) -> list[tuple[dict[str, Any] | None, list[ConfigError]]]:
    """Per page: (its validated model, []) or (None, [errors]). Shape only: references to
    devices and activities are NOT checked, so this is for rendering previews, never for apply."""
    pages = config.get("pages")
    if not isinstance(pages, list):
        return []
    schema = _build_schema(_Resolver(None))
    out: list[tuple[dict[str, Any] | None, list[ConfigError]]] = []
    for i, page in enumerate(pages):
        try:
            model = schema({"pages": [page]})
            out.append((model["pages"][0], []))
        except vol.Invalid as exc:
            errs = exc.errors if isinstance(exc, vol.MultipleInvalid) else [exc]
            out.append(
                (
                    None,
                    [
                        ConfigError("<config>", ["pages", i, *list(e.path)[2:]], e.error_message)
                        for e in errs
                    ],
                )
            )
    return out


def _tap(act: Any):
    def check(v: Any) -> Any:
        return v if isinstance(v, str) and v in ("toggle", "none") else act(v)

    return check


def _check_ha_power(p: dict[str, Any]) -> dict[str, Any]:
    if set(p) != {"on", "off"}:
        raise vol.Invalid("a Home Assistant device's power needs both `on` and `off`")
    return p


def _check_power(p: dict[str, Any]) -> dict[str, Any]:
    if set(p) not in ({"on", "off"}, {"toggle"}):
        raise vol.Invalid("power needs both `on` and `off`, or just `toggle`")
    return p


def _check_widget(w: dict[str, Any]) -> dict[str, Any]:
    kind = w["type"]
    nav = "nav" in w
    if kind != "label" and not nav and "id" not in w:
        raise vol.Invalid(f"a {kind} widget needs an `id`")
    if nav and kind != "button":
        raise vol.Invalid("`nav` belongs on a button")
    if nav and "action" in w:
        raise vol.Invalid("a button takes `nav` or `action`, not both")
    if kind == "button" and "action" not in w and not nav:
        raise vol.Invalid("a button needs an `action` (or `nav`)")
    if kind == "tile" and "entity" not in w:
        raise vol.Invalid("a tile needs an `entity`")
    if "tap" in w:
        if kind != "tile":
            raise vol.Invalid("`tap` belongs on a tile")
        if w["tap"] == "toggle" and default_tap(w["entity"]) is None:
            raise vol.Invalid(
                f"`tap: toggle` does not work on a {w['entity'].split('.')[0]} entity"
            )
    if kind != "button" and "action" in w or kind != "tile" and "entity" in w:
        raise vol.Invalid(f"unexpected `{'action' if 'action' in w else 'entity'}` on a {kind}")
    if "on_color" in w and kind != "tile":
        raise vol.Invalid("`on_color` belongs on a tile")
    if nav and "role" in w:
        raise vol.Invalid("a `nav` button has the nav role; drop `role`")
    return w


def _check_page(p: dict[str, Any]) -> dict[str, Any]:
    if "list" in p:
        if "rows" in p:
            raise vol.Invalid("a list page takes its rows from `list.rows`", path=["rows"])
        if p.get("cols", 1) > 15:
            raise vol.Invalid("a list page needs cols 1..15", path=["cols"])
        p["cols"] = p.get("cols", 1)
        p["rows"] = p["list"]["rows"]
        for j, w in enumerate(p["widgets"]):
            for k in ("x", "y"):
                if k in w:
                    raise vol.Invalid(
                        "list pages place widgets themselves; drop x/y", path=["widgets", j, k]
                    )
        return p
    for k in ("cols", "rows"):
        if k not in p:
            raise vol.Invalid("required key not provided", path=[k])
    for j, w in enumerate(p["widgets"]):
        for k in ("x", "y"):
            if k not in w:
                raise vol.Invalid("required key not provided", path=["widgets", j, k])
    return p


# ---------------------------------------------------------------- cross references
def _check_refs(model: Model, sink: list[_RefError] | None = None) -> None:
    """Dangling or duplicate references: raises _RefError at the first, or (with `sink`)
    appends every one found and carries on."""

    def fail(path: list[Any], msg: str) -> None:
        err = _RefError(path, msg)
        if sink is None:
            raise err
        sink.append(err)

    devs = {d["id"]: d for d in model["devices"]}
    acts = {a["id"] for a in model["activities"]}
    themes = model.get("themes", {})
    for tname in themes:
        if tname in PRESETS:
            fail(["themes", tname], f"user theme {tname!r} reuses a preset name; pick another")
            continue
        try:
            resolve_theme(tname, themes, PRESETS)
        except ThemeError as exc:
            msg = str(exc)
            if msg.startswith("unknown theme"):
                msg += f" (extends a preset ({', '.join(PRESETS)}) or a theme of this file)"
            elif "deeper than" in msg:
                msg = f"extends chain deeper than {MAX_DEPTH}"
            fail(["themes", tname, "extends"], f"theme {tname!r}: {msg}")
    chosen = model.get("theme")
    if chosen is not None and chosen not in PRESETS and chosen not in themes:
        fail(
            ["theme"],
            f"unknown theme {chosen!r}; presets: {', '.join(PRESETS)}"
            + (f"; yours: {', '.join(themes)}" if themes else ""),
        )

    def uniq(items: list[dict[str, Any]], section: str, what: str) -> None:
        seen: set[str] = set()
        for i, it in enumerate(items):
            if it["id"] in seen:
                fail([section, i, "id"], f"duplicate {what} id {it['id']!r}")
            seen.add(it["id"])

    uniq(model["devices"], "devices", "device")
    uniq(model["activities"], "activities", "activity")
    uniq(model["pages"], "pages", "page")

    if len(model.get("bt_remotes", ())) > BT_MAX_REMOTES:
        fail(
            ["bt_remotes", BT_MAX_REMOTES],
            f"too many Bluetooth remotes: {len(model['bt_remotes'])} (a hub handles at most {BT_MAX_REMOTES})",
        )
    bts = [i for i, d in enumerate(model["devices"]) if d.get("kind") == "bt"]
    if len(bts) > BT_MAX_HOSTS:
        fail(
            ["devices", bts[BT_MAX_HOSTS]],
            f"too many Bluetooth devices: {len(bts)} (a hub handles at most {BT_MAX_HOSTS})",
        )

    seen_addr: dict[str, list[Any]] = {}
    for sect, items in (("devices", model["devices"]), ("bt_remotes", model.get("bt_remotes", []))):
        for i, it in enumerate(items):
            a = it.get("address")
            if a is None or (sect == "devices" and it.get("kind") != "bt"):
                continue
            if a in seen_addr:
                fail([sect, i, "address"], f"Bluetooth address {a} is already used by {fmt_path(seen_addr[a])}")
            else:
                seen_addr[a] = [sect, i]

    # An alias must name exactly one device, and never shadow another device's real id --
    # otherwise `device: X` would silently reach a different device than it says.
    owner: dict[str, str] = {}
    for i, d in enumerate(model["devices"]):
        for j, alias in enumerate(d["aliases"]):
            path = ["devices", i, "aliases", j]
            if alias in devs and alias != d["id"]:
                fail(path, f"alias {alias!r} is another device's id")
            if alias in owner and owner[alias] != d["id"]:
                fail(path, f"alias {alias!r} is also used by {owner[alias]!r}")
            owner[alias] = d["id"]

    def check_action(a: dict[str, Any], path: list[Any]) -> None:
        if a["type"] == "device":
            d = devs.get(a["device"])
            if d is None:
                fail([*path, "device"], f"unknown device {a['device']!r}")
                return
            if "command" in a:
                ok = a["command"] in d["commands"] or (
                    a["command"] in ("power_on", "power_off") and "power" in d
                )
                if not ok:
                    fail([*path, "command"], f"{a['device']} has no {a['command']!r}")
            elif a["input"] not in d["inputs"]:
                fail([*path, "input"], f"{a['device']} has no input {a['input']!r}")
        elif a["type"] == "activity" and a["activity"] != "off" and a["activity"] not in acts:
            fail([*path, "activity"], f"unknown activity {a['activity']!r}")

    for i, a in enumerate(model["activities"]):
        base = ["activities", i]
        for j, ad in enumerate(a["devices"]):
            if ad["device"] not in devs:
                fail([*base, "devices", j, "device"], f"unknown device {ad['device']!r}")
                continue
            if "input" in ad and ad["input"] not in devs[ad["device"]]["inputs"]:
                fail([*base, "devices", j, "input"], f"no input {ad['input']!r}")
        for sect in ("steps", "off"):
            for j, s in enumerate(a[sect]):
                check_action(s, [*base, sect, j])
        for k, v in a["keys"].items():
            check_action(v, [*base, "keys", k])
    for k, v in model["keys"].items():
        check_action(v, ["keys", k])
    pages = model["pages"]
    pids = {p["id"] for p in pages}
    for kind in ("home", "favorites"):
        of_kind = [i for i, p in enumerate(pages) if p.get("kind") == kind]
        if len(of_kind) > 1:
            fail(["pages", of_kind[1], "kind"], f"a second {kind} page")
    seen: set[str] = set()
    for i, p in enumerate(pages):
        if "activity" in p and p["activity"] not in acts:
            fail(["pages", i, "activity"], f"unknown activity {p['activity']!r}")
        for j, w in enumerate(p["widgets"]):
            nav = w.get("nav")
            if nav is not None:
                path = ["pages", i, "widgets", j, "nav"]
                if nav == "next" and i + 1 >= len(pages):
                    fail(path, "`next` on the last page")
                if nav == "prev" and i == 0:
                    fail(path, "`prev` on the first page")
                if nav == "home" and not any(q.get("kind") == "home" for q in pages):
                    fail(path, "`home` needs a page with `kind: home`")
                if nav not in NAV_WORDS and nav not in pids:
                    fail(path, f"unknown page {nav!r}")
            if "id" in w:
                if w["id"] in seen:
                    fail(["pages", i, "widgets", j, "id"], f"duplicate widget id {w['id']!r}")
                seen.add(w["id"])
            if "action" in w:
                check_action(w["action"], ["pages", i, "widgets", j, "action"])
            if isinstance(w.get("tap"), dict):
                check_action(w["tap"], ["pages", i, "widgets", j, "tap"])


class _RefError(Exception):
    def __init__(self, path: list[Any], msg: str) -> None:
        super().__init__(msg)
        self.path, self.msg = path, msg


# ---------------------------------------------------------------- entry points
def parse_model(text: str, file: str = "<config>", base_dir: Path | None = None) -> Model:
    """Validate YAML `text`. `file` is only used in messages; `base_dir` anchors IR file refs.
    Fail-fast: raises ConfigError for the first problem."""
    model = _parse(text, file, base_dir, None)
    assert model is not None
    return model


def validate_all(
    text: str, file: str = "<config>", base_dir: Path | None = None
) -> list[ConfigError]:
    """Every problem the model can report in one pass (empty = valid). Schema errors come all at
    once; the cross-reference checks only run on a document that passes the schema."""
    errors: list[ConfigError] = []
    try:
        _parse(text, file, base_dir, errors)
    except ConfigError as exc:
        errors.append(exc)
    return errors


def _parse(
    text: str, file: str, base_dir: Path | None, errors: list[ConfigError] | None
) -> Model | None:
    check_yaml_depth(text, file)
    try:
        root = yaml.compose(text, Loader=_Loader)
        data = yaml.load(text, Loader=_Loader)
    except RecursionError:
        raise ConfigError(file, [], "invalid YAML: nested too deeply") from None
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
    return _parse_data(data, root, file, base_dir, errors)


def parse_data(data: dict[str, Any], base_dir: Path | None = None) -> Model:
    """Like parse_model for an already-loaded JSON-shaped dict (no YAML pass: much faster).
    Messages carry no line numbers. Fail-fast."""
    model = _parse_data(data, None, "<config>", base_dir, None)
    assert model is not None
    return model


def validate_data(data: dict[str, Any], base_dir: Path | None = None) -> list[ConfigError]:
    """`validate_all` for an already-loaded dict."""
    errors: list[ConfigError] = []
    try:
        _parse_data(data, None, "<config>", base_dir, errors)
    except ConfigError as exc:
        errors.append(exc)
    return errors


def _parse_data(
    data: dict[str, Any],
    root: yaml.Node | None,
    file: str,
    base_dir: Path | None,
    errors: list[ConfigError] | None,
) -> Model | None:
    try:
        model = _build_schema(_Resolver(base_dir))(data)
        refs: list[_RefError] | None = None if errors is None else []
        _check_refs(model, refs)
        if refs:
            errors.extend(ConfigError(file, e.path, e.msg, _line_of(root, e.path)) for e in refs)
            return None
    except vol.MultipleInvalid as exc:
        found = [
            ConfigError(file, list(e.path), e.error_message, _line_of(root, list(e.path)))
            for e in exc.errors
        ]
        if errors is None:
            raise found[0] from None
        errors.extend(found)
        return None
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
