"""Build one device block (docs/ha-config.md format) from an archive device file + codeset."""

from __future__ import annotations

import json
import re
from dataclasses import dataclass, field
from typing import Any

import yaml

from .convert import Converted, convert_command

# Must equal compiler.LIMITS (tests/py/test_irdb_device.py enforces it).
MAX_COMMANDS = 256
MAX_INPUTS = 64
MAX_JSON = 131072
MAX_NAME_CHARS = 200  # archive command names are short; anything longer is junk
MAX_PRONTO_CHARS = 8192  # the hub takes at most 520 words (about 2.6 kB of text)

DEVICE_TYPES = {
    1: "Television", 2: "VCR", 3: "CD Player", 4: "DVD Player", 5: "Laserdisc", 6: "Tape Deck",
    7: "Tuner", 8: "MiniDisc", 9: "DAT", 10: "Satellite", 11: "Cable", 12: "PVR",
    13: "Game Console", 14: "Media Player", 15: "Audio Receiver", 16: "Amplifier", 17: "Projector",
    18: "Home Automation", 19: "Lighting", 20: "Climate", 21: "Audio Switcher", 22: "Video Switcher",
}  # fmt: skip

_SYMBOLS = {
    "+": "plus",
    "-": "minus",
    ".": "dot",
    "#": "hash",
    "*": "star",
    "/": "slash",
    "&": "and",
}


def device_type_id(value: object) -> int | None:
    """Accepts an archive deviceType number, its digits, or a name such as "tv" / "dvd player"."""
    if isinstance(value, bool):
        return None
    if isinstance(value, int):
        return value
    text = str(value).strip().casefold()
    if text.isdigit():
        return int(text)
    if text in ("tv", "television"):
        return 1
    for k, v in DEVICE_TYPES.items():
        if v.casefold() == text:
            return k
    return None


def _slug(raw: str, limit: int = 31) -> str:
    if not re.search(r"[A-Za-z0-9]", raw):  # a symbol-only name such as "+" or "."
        raw = "".join(f" {_SYMBOLS[c]} " if c in _SYMBOLS else c for c in raw)
    else:
        raw = raw.replace("+", " plus ")
    s = re.sub(r"(?<=[a-z])(?=[A-Z])", "_", raw)
    s = re.sub(r"[^a-z0-9]+", "_", s.lower()).strip("_")
    return s[:limit].strip("_") or "cmd"


def command_name(raw: str, used: set[str]) -> str:
    """Archive command name -> a unique, valid, non-reserved command name (deterministic)."""
    base = _slug(raw)
    if base in ("power_on", "power_off") or base.startswith("input_"):
        base = _slug("ir_" + base)
    name, n = base, 2
    while name in used:
        suffix = f"_{n}"
        name = base[: 31 - len(suffix)] + suffix
        n += 1
    used.add(name)
    return name


def _label(raw: str, used: set[str]) -> str:
    """Free-text name (input labels): keep it readable, cap at 31 bytes, make unique."""
    base = "".join(c for c in raw if ord(c) >= 32).strip() or "input"
    while len(base.encode()) > 31:
        base = base[:-1]
    name, n = base, 2
    while name in used:
        suffix = f" {n}"
        name = base
        while len((name + suffix).encode()) > 31:
            name = name[:-1]
        name += suffix
        n += 1
    used.add(name)
    return name


_TEXT_FIELDS = ("pronto", "prontoRepeat", "keycode", "protocol")


@dataclass
class Built:
    device: dict[str, Any]
    skipped: list[tuple[str, str]] = field(default_factory=list)  # (archive name, reason)
    forms: dict[str, str] = field(default_factory=dict)  # our command name -> proto | pronto
    notes: list[str] = field(default_factory=list)


def build_device(
    device: dict,
    codeset: dict,
    *,
    mfr: str,
    model_file: str,
    codeset_path: str,
    imported_at: str,
    dev_id: str | None = None,
    name: str | None = None,
) -> Built:
    model = str(device.get("model") or model_file).strip()
    out: dict[str, Any] = {
        "id": dev_id or _slug(f"{mfr} {model}"),
        "name": (name or f"{mfr} {model}")[:31].strip() or "Device",
        "source": {
            "kind": "harmony_archive",
            "mfr": mfr,
            "model_file": model_file,
            "codeset": codeset_path,
            "imported_at": imported_at,
        },
    }
    built = Built(out)
    delay = (device.get("timing") or {}).get("powerOnDelay")
    if isinstance(delay, int) and not isinstance(delay, bool) and 0 < delay <= 60000:
        out["settle_ms"] = delay

    by_raw: dict[str, Converted] = {}
    commands: dict[str, dict] = {}
    used: set[str] = set()
    size = 2
    for cmd in codeset.get("commands", []):
        raw = cmd.get("name") if isinstance(cmd, dict) else None
        if not isinstance(raw, str) or not raw.strip():
            built.skipped.append((repr(raw), "command has no name"))
            continue
        if len(raw) > MAX_NAME_CHARS:
            built.skipped.append((raw[:40] + "...", "command name is too long"))
            continue
        if any(isinstance(cmd.get(k), str) and len(cmd[k]) > MAX_PRONTO_CHARS for k in _TEXT_FIELDS):
            built.skipped.append((raw, "IR data is too long"))
            continue
        res = convert_command(cmd)
        if not isinstance(res, Converted):
            built.skipped.append((raw, res.reason))
            continue
        if len(commands) >= MAX_COMMANDS:
            built.skipped.append((raw, f"over the hub's {MAX_COMMANDS} commands limit"))
            continue
        n = command_name(raw, used)
        # running size of json.dumps(commands): braces + each `"name":action,`
        add = len(json.dumps(n)) + len(json.dumps(res.action, separators=(",", ":"))) + 2
        if size + add > MAX_JSON:
            used.discard(n)
            built.skipped.append((raw, "over the hub's config size limit"))
            continue
        size += add
        commands[n] = res.action
        built.forms[n] = res.form
        by_raw.setdefault(raw, res)
        built.notes.extend(f"{n}: {x}" for x in res.notes)

    power = device.get("power")
    if isinstance(power, dict):
        first = {
            k: next((x for x in power.get(k) or [] if isinstance(x, str)), None)
            for k in ("on", "off", "toggle")
        }
        pick = {k: by_raw[v].action for k, v in first.items() if v in by_raw}
        if "on" in pick and "off" in pick:
            out["power"] = {"on": pick["on"], "off": pick["off"]}
        elif "toggle" in pick:
            out["power"] = {"toggle": pick["toggle"]}
        else:
            built.notes.append("no usable power commands in the archive entry")

    inputs: dict[str, dict] = {}
    labels: set[str] = set()
    lst = (
        (device.get("inputs") or {}).get("list") if isinstance(device.get("inputs"), dict) else None
    )
    for item in lst if isinstance(lst, list) else []:
        cmds = item.get("commands") if isinstance(item, dict) else None
        key = cmds[0] if isinstance(cmds, list) and cmds and isinstance(cmds[0], str) else None
        if key in by_raw and isinstance(item.get("name"), str) and len(inputs) < MAX_INPUTS:
            inputs[_label(item["name"], labels)] = by_raw[key].action
    if inputs:
        out["inputs"] = inputs
    out["commands"] = commands
    return built


class _Flow(dict):
    pass


class _Dumper(yaml.SafeDumper):
    def increase_indent(self, flow=False, indentless=False):  # indent list items like the docs
        return super().increase_indent(flow, False)


_Dumper.add_representer(
    _Flow, lambda d, v: d.represent_mapping("tag:yaml.org,2002:map", v, flow_style=True)
)
# Keep `on:` / `off:` as plain keys (the config loader reads YAML 1.2 booleans).
_Dumper.yaml_implicit_resolvers = {
    ch: [(t, rx) for t, rx in rs if t != "tag:yaml.org,2002:bool"]
    for ch, rs in yaml.SafeDumper.yaml_implicit_resolvers.items()
}
_Dumper.add_implicit_resolver(
    "tag:yaml.org,2002:bool", re.compile(r"^(?:true|false|True|False|TRUE|FALSE)$"), list("tfTF")
)


def _flow(device: dict) -> dict:
    d = dict(device)
    if "power" in d:
        d["power"] = _Flow({k: _Flow(v) for k, v in d["power"].items()})
    for sect in ("inputs", "commands"):
        if sect in d:
            d[sect] = {k: _Flow(v) for k, v in d[sect].items()}
    d["source"] = _Flow(d["source"]) if "source" in d else None
    if d["source"] is None:
        del d["source"]
    return d


def to_yaml(devices: list[dict]) -> str:
    """A complete config fragment (`devices:` list) in the docs/ha-config.md style."""
    return yaml.dump(
        {"devices": [_flow(d) for d in devices]},
        Dumper=_Dumper,
        sort_keys=False,
        width=10**9,
        allow_unicode=True,
        default_flow_style=False,
    )
