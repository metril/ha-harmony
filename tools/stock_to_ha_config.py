#!/usr/bin/env -S uv run
"""Convert the extracted stock Harmony IR database into an `ha_harmony` integration config.

Input:  `ir_codes.json` from the Phase 1 backup tool (or the `extracted/` dir holding it):
        {"warnings": [...], "devices": {"<id>": {"<command>": {...}}}}
Output: a `<hub_id>.yaml` / `.json` in the format documented in docs/ha-config.md, using the
        `raw: {hz, us}` IR action so the hub's own carrier and timings are kept verbatim.

Mapping of the stock signal (`once` / `repeat` / `finish` / `min_repeats`) onto `raw`:

The config schema's `raw` action is ONE mark/space sequence plus a carrier. The hub sends it once
per press and, while the key is held, re-sends the same sequence (hub/engine/irout.c; raw codes
always have rep_n = 0). There is no separate repeat frame. (`pronto` has a once/repeat split but
quantises durations to carrier periods and derives the carrier from a frequency word, so 37900 Hz
and 568/552 us would not survive; it is not used.)

--repeats single (default)  the press frame is `once` if the stock signal has one, else `repeat`
                            (the Samsung-style "frame lives in repeat" signals). A separate stock
                            repeat frame (NEC-style short repeat), `min_repeats` and `finish` are
                            not expressible and are recorded per command in the report.
--repeats expand            emits what the stock hub puts on the wire for one press:
                            once + repeat * min_repeats + finish (a frame-only signal is sent
                            max(min_repeats, 1) times). Spaces above 65535 us (hub u16) are clamped
                            and noted. Uses much more of the hub's 16384-word pool.

Nothing is dropped silently: non-IR commands (hid, roku_ecp) are skipped with a warning each,
anything unconvertible is an error (exit status 1, no config written), and a per-device coverage
report reconciles converted + skipped + errors == input.
"""

from __future__ import annotations

import argparse
import json
import sys
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import yaml

ROOT = Path(__file__).resolve().parents[1]

MAX_US = 65535  # hub stores durations as u16
MAX_DUR = 512  # HP_IR_MAX_DUR: durations per signal
HZ_RANGE = (1000, 1_000_000)
NAME_MAX_BYTES = 31
SKIP_TRANSPORTS = {
    "hid": "Bluetooth HID usage command, not IR; the hub cannot send it as an IR action",
    "roku_ecp": "Roku ECP over IP (HTTP keypress), not IR; needs a network integration instead",
}

# Stock device id -> (config slug, display name). Override or extend with --device-name.
KNOWN_DEVICES: dict[str, tuple[str, str]] = {
    "51886472": ("samsung_tv", "Samsung TV"),
    "67368870": ("nvidia_shield", "NVIDIA Game Console"),
    "81729894": ("kvm_1", "KVM 1"),
    "64694265": ("jtech_switch", "J-Tech AV Switch"),
    "80756020": ("roku_stick", "Roku Streaming Stick"),
    "51887071": ("audioengine_a5", "Audioengine A5+"),
}


class ConvertError(ValueError):
    """An entry that cannot be converted; always fatal."""


@dataclass
class CmdResult:
    device: str
    command: str
    status: str  # converted | skipped | error
    reason: str = ""
    notes: list[str] = field(default_factory=list)
    stock: dict[str, Any] = field(default_factory=dict)
    emitted_us: int = 0


# ----------------------------------------------------------------------------- loading
def _no_dup_pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    out: dict[str, Any] = {}
    dups = []
    for k, v in pairs:
        if k in out:
            dups.append(k)
        out[k] = v
    if dups:
        out["\0duplicates"] = dups
    return out


def locate(path: Path) -> Path:
    p = path / "ir_codes.json" if path.is_dir() else path
    if not p.is_file():
        raise SystemExit(f"error: no ir_codes.json at {p}")
    return p


def load_stock(path: Path) -> dict[str, Any]:
    data = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=_no_dup_pairs)
    if not isinstance(data, dict) or not isinstance(data.get("devices"), dict):
        raise SystemExit(f"error: {path}: expected an object with a `devices` mapping")
    return data


# ----------------------------------------------------------------------------- conversion
def _int_list(entry: dict[str, Any], key: str) -> list[int]:
    v = entry.get(key)
    if not isinstance(v, list) or any(isinstance(x, bool) or not isinstance(x, int) for x in v):
        raise ConvertError(f"`{key}` must be a list of integers")
    return v


def convert_ir(entry: dict[str, Any], mode: str) -> tuple[int, list[int], list[str]]:
    """One stock IR entry -> (carrier Hz, durations, notes). Raises ConvertError."""
    hz = entry.get("carrier_hz")
    if isinstance(hz, bool) or not isinstance(hz, int) or not HZ_RANGE[0] <= hz <= HZ_RANGE[1]:
        raise ConvertError(f"carrier_hz {hz!r} is not an integer in {HZ_RANGE[0]}..{HZ_RANGE[1]}")
    once, rep, fin = (_int_list(entry, k) for k in ("once", "repeat", "finish"))
    mr = entry.get("min_repeats")
    if isinstance(mr, bool) or not isinstance(mr, int) or mr < 0:
        raise ConvertError(f"min_repeats {mr!r} is not a non-negative integer")
    if not once and not rep:
        raise ConvertError("signal has neither `once` nor `repeat` durations")
    notes: list[str] = []
    if mode == "single":
        us = list(once or rep)
        if once and rep:
            notes.append("repeat_frame_not_expressible")
        if fin:
            notes.append("finish_not_expressible")
        if not once and mr > 1:
            notes.append(
                "min_repeats_as_repeats"
            )  # `repeats: N` on the command (hub config format 2)
        elif once and mr:
            notes.append("min_repeats_not_expressed")
    else:
        n = mr if once else max(mr, 1)
        us = once + rep * n + fin
    for i, v in enumerate(us):
        if v <= 0:
            raise ConvertError(f"duration #{i} is {v}; durations must be positive")
        if v > MAX_US:
            if i % 2 == 0:
                raise ConvertError(f"mark #{i} is {v} us, above the hub's {MAX_US} us limit")
            us[i] = MAX_US
            notes.append(f"space_clamped_to_{MAX_US}")
    if len(us) % 2:
        raise ConvertError(f"odd number of durations ({len(us)}); a space is missing")
    if len(us) > MAX_DUR:
        raise ConvertError(f"{len(us)} durations exceed the hub limit of {MAX_DUR}")
    return hz, us, notes


def command_name(name: str) -> str:
    """Stock command key -> config command name (kept verbatim; only validated)."""
    if not name or any(ord(c) < 32 for c in name):
        raise ConvertError("empty or control-character command name")
    if len(name.encode()) > NAME_MAX_BYTES:
        raise ConvertError(f"command name over {NAME_MAX_BYTES} bytes")
    if name in ("power_on", "power_off") or name.startswith("input_"):
        raise ConvertError("command name is reserved by the hub (power_on/power_off/input_*)")
    return name


def device_ident(dev_id: str, names: dict[str, tuple[str, str]]) -> tuple[str, str]:
    if dev_id in names:
        return names[dev_id]
    slug = "dev_" + "".join(c if c.isalnum() else "_" for c in dev_id.lower())[:27]
    return slug, f"Harmony device {dev_id}"


@dataclass
class Conversion:
    config: dict[str, Any]
    results: list[CmdResult]
    warnings: list[str]
    omitted_devices: list[str]


def convert(
    stock: dict[str, Any],
    *,
    mode: str = "single",
    only: list[str] | None = None,
    names: dict[str, tuple[str, str]] | None = None,
) -> Conversion:
    names = {**KNOWN_DEVICES, **(names or {})}
    devices = stock["devices"]
    warnings: list[str] = []
    results: list[CmdResult] = []
    out_devices: list[dict[str, Any]] = []
    omitted: list[str] = []
    if "\0duplicates" in devices:
        dups = devices.pop("\0duplicates")
        warnings.append(f"duplicate device ids in input (last one kept): {dups}")
        results.append(CmdResult(",".join(dups), "*", "error", "duplicate device id in input"))
    for want in only or []:
        if want not in devices:
            raise SystemExit(f"error: --device {want}: not in the input ({', '.join(devices)})")
    seen_slugs: dict[str, str] = {}
    for dev_id, cmds in devices.items():
        if only and dev_id not in only:
            continue
        slug, dname = device_ident(dev_id, names)
        if slug in seen_slugs:
            results.append(
                CmdResult(
                    dev_id,
                    "*",
                    "error",
                    f"slug {slug!r} also used by device {seen_slugs[slug]}; pass --device-name",
                )
            )
            continue
        seen_slugs[slug] = dev_id
        if not isinstance(cmds, dict):
            results.append(CmdResult(dev_id, "*", "error", "device is not a mapping"))
            continue
        cmds = dict(cmds)
        for dup in cmds.pop("\0duplicates", []):
            results.append(CmdResult(dev_id, dup, "error", "duplicate command name in input"))
        if not cmds:
            warnings.append(f"device {dev_id} ({dname}): no commands in the input")
        converted: dict[str, Any] = {}
        for cname, entry in cmds.items():
            r = CmdResult(dev_id, cname, "converted")
            results.append(r)
            try:
                if not isinstance(entry, dict):
                    raise ConvertError(f"entry is {type(entry).__name__}, not an object")
                r.stock = {
                    k: entry.get(k)
                    for k in ("protocol", "transport", "carrier_hz", "min_repeats", "key")
                    if k in entry
                }
                if entry.get("transport") == "ir":
                    r.stock.update(
                        once_n=len(entry.get("once") or []),
                        repeat_n=len(entry.get("repeat") or []),
                        finish_n=len(entry.get("finish") or []),
                    )
                    hz, us, r.notes = convert_ir(entry, mode)
                    converted[command_name(cname)] = {"raw": {"hz": hz, "us": us}}
                    if "min_repeats_as_repeats" in r.notes:  # frame-only signal sent N times
                        converted[command_name(cname)]["repeats"] = min(entry["min_repeats"], 8)
                    r.emitted_us = len(us)
                elif entry.get("transport") in SKIP_TRANSPORTS:
                    r.status = "skipped"
                    r.reason = f"{entry['transport']}: {SKIP_TRANSPORTS[entry['transport']]}"
                else:
                    raise ConvertError(f"unknown transport {entry.get('transport')!r}")
            except ConvertError as exc:
                r.status, r.reason = "error", str(exc)
        if converted:
            out_devices.append({"id": slug, "name": dname, "commands": converted})
        else:
            omitted.append(f"{dev_id} ({dname})")
    return Conversion({"devices": out_devices}, results, warnings, omitted)


# ----------------------------------------------------------------------------- power, inputs, activities
# Optional second pass. ir_codes.json carries only IR codes; the stock hub's full device database
# (DeviceList.json, on the data partition) says which command powers each device on/off/toggles it,
# how long it needs after power-on, and what each input is called; the stock engine config holds the
# activities as per-device target states ("fixit"). Both are the stock hub's own data, verbatim.
MAX_SETTLE_MS = 60000  # config_model's settle_ms bound
_SLUG_OK = set("abcdefghijklmnopqrstuvwxyz0123456789_")


@dataclass
class StockDevice:
    id: str
    name: str
    transport: Any = None
    power_on: str | None = None
    power_off: str | None = None
    power_toggle: str | None = None
    power_other: str = ""  # set when stock power exists but is not one single IR press
    power_on_delay_ms: int = 0
    inputs: list[tuple[str, str]] = field(default_factory=list)  # (label, IR command), stock order
    inputs_unexpressible: list[str] = field(default_factory=list)


def _single_ir_press(actions: Any) -> str | None:
    """Stock action list -> the command name if it is exactly one plain IR press, else None."""
    if not isinstance(actions, list) or len(actions) != 1 or not isinstance(actions[0], dict):
        return None
    a = actions[0]
    if a.get("__type") != "IRPressAction" or a.get("Duration") not in (None, 0):
        return None
    name = a.get("IRCommandName")
    return name if isinstance(name, str) and name else None


def load_device_list(path: Path) -> dict[str, StockDevice]:
    """Stock DeviceList.json -> {stock device id: power/input/timing facts}."""
    data = json.loads(path.read_text(encoding="utf-8"))
    out: dict[str, StockDevice] = {}
    for entry in data.get("DevicesWithFeatures", []):
        dev = entry.get("Device") or {}
        feats = entry.get("DeviceFeatures") or []
        dev_id = next(
            (str(f["DeviceId-"]) for f in feats if isinstance(f, dict) and f.get("DeviceId-")),
            None,
        )
        if dev_id is None:
            continue
        sd = StockDevice(dev_id, dev.get("Name") or dev_id, dev.get("Transport"))
        for f in feats:
            if not isinstance(f, dict):
                continue
            if f.get("__type") == "PowerFeature":
                sd.power_on = _single_ir_press(f.get("PowerOnActions"))
                sd.power_off = _single_ir_press(f.get("PowerOffActions"))
                sd.power_toggle = _single_ir_press(f.get("PowerToggleActions"))
                has_any = any(
                    f.get(k) for k in ("PowerOnActions", "PowerOffActions", "PowerToggleActions")
                )
                if has_any and not (sd.power_on and sd.power_off) and not sd.power_toggle:
                    sd.power_other = "power actions are not single IR presses"
                delay = f.get("PowerOnDelay")
                sd.power_on_delay_ms = delay if isinstance(delay, int) and delay > 0 else 0
            elif f.get("__type") == "InputFeature":
                for inp in f.get("Inputs") or []:
                    label = inp.get("InputName") if isinstance(inp, dict) else None
                    if not label:
                        continue
                    cmd = _single_ir_press(inp.get("Actions"))
                    if cmd:
                        sd.inputs.append((label, cmd))
                    elif inp.get("Actions"):
                        sd.inputs_unexpressible.append(f"{label!r}: not a single IR press")
        out[dev_id] = sd
    return out


def load_activities(path: Path) -> list[dict[str, Any]]:
    """Stock engine config -> its activity list (each carries `fixit` target states)."""
    data = json.loads(path.read_text(encoding="utf-8"))
    data = data.get("data", data)
    acts = data.get("activity", [])
    if not isinstance(acts, list):
        raise SystemExit(f"error: {path}: expected `activity` to be a list")
    return acts


def _resolve_cmd(name: str | None, cmds: dict[str, Any]) -> str | None:
    """Find a stock command by name among the converted ones.

    Stock's two databases disagree on case for the same command (DeviceList says `InputTv`, the IR
    database `InputTV`). Exact match first; otherwise a case-insensitive match, but only when it is
    unique -- if two commands differ only by case, refuse rather than pick one.
    """
    if name is None:
        return None
    if name in cmds:
        return name
    hits = [c for c in cmds if c.lower() == name.lower()]
    return hits[0] if len(hits) == 1 else None


def _label_ok(label: str) -> bool:
    return (
        bool(label)
        and not any(ord(c) < 32 for c in label)
        and len(label.encode()) <= NAME_MAX_BYTES
    )


def _slugify(label: str) -> str:
    s = "".join(c if c in _SLUG_OK else "_" for c in label.lower())
    while "__" in s:
        s = s.replace("__", "_")
    return s.strip("_")[:31] or "activity"


def apply_device_features(
    conv: Conversion, stock_devs: dict[str, StockDevice], names: dict[str, tuple[str, str]]
) -> None:
    """Add power / settle_ms / inputs to each converted device, from stock's own definitions.

    Every value is a copy of an already-converted command's raw code, so power and inputs fire
    exactly the signal the matching button does. Anything not expressible is a warning, never a guess.
    """
    names = {**KNOWN_DEVICES, **names}
    by_slug = {device_ident(sid, names)[0]: sd for sid, sd in stock_devs.items()}
    rebuilt: list[dict[str, Any]] = []
    for d in conv.config["devices"]:
        sd = by_slug.get(d["id"])
        cmds = d["commands"]
        if sd is None:
            conv.warnings.append(f"{d['name']}: not in DeviceList.json; no power/inputs added")
            rebuilt.append(d)
            continue

        power: dict[str, Any] | None = None
        if sd.power_on and sd.power_off:
            on, off = _resolve_cmd(sd.power_on, cmds), _resolve_cmd(sd.power_off, cmds)
            if on is None or off is None:
                missing = [c for c, r in ((sd.power_on, on), (sd.power_off, off)) if r is None]
                conv.warnings.append(f"{d['name']}: power uses {missing}, not converted; no power")
            else:
                power = {"on": json.loads(json.dumps(cmds[on])),
                         "off": json.loads(json.dumps(cmds[off]))}  # fmt: skip
        elif sd.power_toggle:
            tog = _resolve_cmd(sd.power_toggle, cmds)
            if tog is not None:
                power = {"toggle": json.loads(json.dumps(cmds[tog]))}
            else:
                conv.warnings.append(f"{d['name']}: toggle {sd.power_toggle!r} not converted")
        elif sd.power_other:
            conv.warnings.append(f"{d['name']}: {sd.power_other}; no power control emitted")

        inputs: dict[str, Any] = {}
        for label, cmd in sd.inputs:
            found = _resolve_cmd(cmd, cmds)
            if not _label_ok(label):
                conv.warnings.append(
                    f"{d['name']}: input {label!r} skipped (names are at most {NAME_MAX_BYTES} bytes)"
                )
            elif found is None:
                conv.warnings.append(f"{d['name']}: input {label!r} uses {cmd!r}, not converted")
            elif label in inputs:
                conv.warnings.append(f"{d['name']}: duplicate input label {label!r}; first kept")
            else:
                inputs[label] = json.loads(json.dumps(cmds[found]))
        for why in sd.inputs_unexpressible:
            conv.warnings.append(f"{d['name']}: input {why}")

        out: dict[str, Any] = {"id": d["id"], "name": d["name"]}
        # Stock's numeric id and stock's own device name, so automations written against the
        # stock integration (`remote.send_command device: "51887071"`) work after an entity swap.
        aliases = [a for a in dict.fromkeys((sd.id, sd.name)) if a != d["id"] and _label_ok(a)]
        if aliases:
            out["aliases"] = aliases
        if power and sd.power_on_delay_ms:
            out["settle_ms"] = min(sd.power_on_delay_ms, MAX_SETTLE_MS)
        if power:
            out["power"] = power
        if inputs:
            out["inputs"] = inputs
        out["commands"] = cmds
        rebuilt.append(out)
    conv.config["devices"] = rebuilt


def apply_activities(
    conv: Conversion,
    activities: list[dict[str, Any]],
    stock_devs: dict[str, StockDevice],
    names: dict[str, tuple[str, str]],
) -> None:
    """Stock activities -> config activities. A device stock wants ON is listed (with its input);
    everything else is off, which is the config's own semantics, so stock's explicit Off is implied.

    A device the hub cannot drive (no IR: Bluetooth, IP) is reported, not dropped silently: stock
    powers it some other way, and Home Assistant has to do that part now.
    """
    names = {**KNOWN_DEVICES, **names}
    devs = {d["id"]: d for d in conv.config["devices"]}
    out: list[dict[str, Any]] = []
    used: set[str] = {"off"}
    for act in activities:
        label = act.get("label")
        if act.get("type") == "PowerOff" or str(act.get("id")) == "-1":
            continue  # the config's built-in `off` activity
        if not isinstance(label, str) or not _label_ok(label):
            conv.warnings.append(f"activity {label!r}: unusable name; skipped")
            continue
        slug = base = _slugify(label)
        n = 2
        while slug in used:
            slug, n = f"{base[:28]}_{n}", n + 1
        used.add(slug)

        on: list[dict[str, Any]] = []
        for stock_id, state in (act.get("fixit") or {}).items():
            if not isinstance(state, dict) or state.get("Power") != "On":
                continue
            dslug = device_ident(stock_id, names)[0]
            sd = stock_devs.get(stock_id)
            dname = sd.name if sd else stock_id
            d = devs.get(dslug)
            if d is None:
                how = f"transport {sd.transport}" if sd else "unknown device"
                conv.warnings.append(
                    f"activity {label!r}: wants {dname} ON, but the hub has no IR for it ({how});"
                    " do that part with a Home Assistant automation on this activity"
                )
                continue
            entry: dict[str, Any] = {"device": dslug}
            want_input = state.get("Input")
            if want_input:
                if want_input in d.get("inputs", {}):
                    entry["input"] = want_input
                else:
                    conv.warnings.append(
                        f"activity {label!r}: {dname} input {want_input!r} has no matching input"
                    )
            if "power" not in d and "input" not in entry:
                conv.warnings.append(
                    f"activity {label!r}: {dname} has no power control or input; listing it does nothing"
                )
                continue
            on.append(entry)
        out.append({"id": slug, "name": label, "devices": on})
    conv.config["activities"] = out


# ----------------------------------------------------------------------------- reporting
def summarise(conv: Conversion) -> dict[str, Any]:
    per: dict[str, Counter[str]] = {}
    for r in conv.results:
        per.setdefault(r.device, Counter())[r.status] += 1
    tot: Counter[str] = Counter()
    for c in per.values():
        tot.update(c)
    total_in = sum(tot.values())
    assert tot["converted"] + tot["skipped"] + tot["error"] == total_in, "counts do not reconcile"
    return {"devices": {d: dict(c) for d, c in per.items()}, "total": {**tot, "input": total_in}}


def render_report(conv: Conversion, summary: dict[str, Any], *, only_ir: bool, mode: str) -> str:
    lines = [f"stock_to_ha_config: repeats={mode}"]
    lines += [f"warning: {w}" for w in conv.warnings]
    skipped = [r for r in conv.results if r.status == "skipped"]
    if not only_ir:
        lines += [f"warning: skipped {r.device}/{r.command}: {r.reason}" for r in skipped]
    lines += [
        f"ERROR: {r.device}/{r.command}: {r.reason}" for r in conv.results if r.status == "error"
    ]
    notes = Counter(n for r in conv.results for n in r.notes)
    for n, c in sorted(notes.items()):
        lines.append(f"note: {c} command(s): {n} (details per command in --report)")
    lines.append("coverage (in / converted / skipped / errors):")
    for dev, c in summary["devices"].items():
        n = sum(c.values())
        lines.append(
            f"  {dev}: {n} / {c.get('converted', 0)} / {c.get('skipped', 0)} / {c.get('error', 0)}"
        )
    t = summary["total"]
    lines.append(
        f"total: {t['input']} in = {t.get('converted', 0)} converted + {t.get('skipped', 0)} "
        f"skipped + {t.get('error', 0)} errors"
    )
    if skipped:
        by = Counter(r.stock.get("transport", "?") for r in skipped)
        lines.append(
            f"skipped by transport: {dict(by)}" + (" (--only-ir: not listed)" if only_ir else "")
        )
    if conv.omitted_devices:
        lines.append(
            f"devices omitted (no convertible commands): {', '.join(conv.omitted_devices)}"
        )
    ok = t["converted"] + t.get("skipped", 0) + t.get("error", 0) == t["input"]
    lines.append(f"reconciliation: converted + skipped + errors == input: {'OK' if ok else 'FAIL'}")
    return "\n".join(lines)


def cross_check_coverage(stock_path: Path, summary: dict[str, Any], all_devices: bool) -> list[str]:
    cov_path = stock_path.with_name("coverage.json")
    if not (all_devices and cov_path.is_file()):
        return []
    try:
        cov = json.loads(cov_path.read_text(encoding="utf-8"))
        out = []
        for dev, c in cov["devices"].items():
            have = sum(summary["devices"].get(dev, {}).values())
            if have != c["commands"]:
                out.append(
                    f"coverage.json counts {c['commands']} commands for {dev}, "
                    f"ir_codes.json has {have}"
                )
        return out
    except (OSError, ValueError, KeyError, TypeError) as exc:
        return [f"coverage.json unreadable: {exc}"]


# ----------------------------------------------------------------------------- output
def dump_config(config: dict[str, Any], fmt: str, header: str) -> str:
    if fmt == "json":
        return json.dumps(config, indent=1, ensure_ascii=False) + "\n"
    body = yaml.safe_dump(
        config, sort_keys=False, allow_unicode=True, width=100, default_flow_style=None
    )
    return "".join(f"# {ln}\n" for ln in header.splitlines()) + body


def validate_config(text: str, fmt: str) -> tuple[int, int]:
    """Run the real config_model + compiler. Returns (config bytes, pool words)."""
    sys.path.insert(0, str(ROOT))
    from custom_components.ha_harmony.harmony_core.compiler import (
        _pool_words,
        compact_json,
        compile_config,
    )
    from custom_components.ha_harmony.harmony_core.config_model import parse_model

    model = parse_model(text, "<generated>")  # JSON is valid YAML
    compiled = compile_config(model)
    size = len(compact_json(compiled.hub_json).encode())
    # The compiler's own count over everything it sends: commands AND power/inputs, which each
    # carry their own copy of a raw code (the hub pool has no dedup).
    return size, _pool_words(compiled.hub_json)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("input", type=Path, help="extracted dir or ir_codes.json")
    ap.add_argument("--out", type=Path, required=True, help="output .yaml/.yml/.json")
    ap.add_argument("--device", action="append", default=[], metavar="ID", help="only this device")
    ap.add_argument("--only-ir", action="store_true", help="no per-command skip warnings")
    ap.add_argument("--format", choices=("yaml", "json"), help="default: from --out suffix")
    ap.add_argument("--repeats", choices=("single", "expand"), default="single")
    ap.add_argument("--report", type=Path, help="write the full per-command report (JSON)")
    ap.add_argument("--device-name", action="append", default=[], metavar="ID=slug:Name")
    ap.add_argument("--no-validate", action="store_true", help="skip config_model/compiler check")
    ap.add_argument(
        "--device-list",
        type=Path,
        help="stock DeviceList.json: adds power on/off/toggle, settle_ms and named inputs",
    )
    ap.add_argument(
        "--engine-config",
        type=Path,
        help="stock engine config JSON: adds activities (needs --device-list)",
    )
    a = ap.parse_args(argv)
    if a.engine_config and not a.device_list:
        ap.error("--engine-config needs --device-list (activities refer to power and inputs)")

    fmt = a.format or ("json" if a.out.suffix == ".json" else "yaml")
    names: dict[str, tuple[str, str]] = {}
    for spec in a.device_name:
        dev, _, rest = spec.partition("=")
        slug, _, nm = rest.partition(":")
        if not dev or not slug:
            ap.error(f"--device-name {spec!r}: expected ID=slug[:Name]")
        names[dev] = (slug, nm or slug)

    src = locate(a.input)
    stock = load_stock(src)
    conv = convert(stock, mode=a.repeats, only=a.device or None, names=names)
    if a.device_list:
        stock_devs = load_device_list(a.device_list)
        apply_device_features(conv, stock_devs, names)
        if a.engine_config:
            apply_activities(conv, load_activities(a.engine_config), stock_devs, names)
    summary = summarise(conv)
    conv.warnings += cross_check_coverage(src, summary, not a.device)
    report = render_report(conv, summary, only_ir=a.only_ir, mode=a.repeats)
    print(report, file=sys.stderr)

    if a.report:
        a.report.write_text(
            json.dumps(
                {
                    "repeats": a.repeats,
                    "summary": summary,
                    "warnings": conv.warnings,
                    "omitted_devices": conv.omitted_devices,
                    "commands": [r.__dict__ for r in conv.results],
                },
                indent=1,
            )
            + "\n",
            encoding="utf-8",
        )
    if summary["total"].get("error"):
        print("FAILED: unconvertible entries; no config written", file=sys.stderr)
        return 1
    header = (
        f"Generated by tools/stock_to_ha_config.py from {src.name} (repeats={a.repeats}).\n"
        "Carrier and durations are the stock hub's own values, verbatim."
    )
    text = dump_config(conv.config, fmt, header)
    if not a.no_validate:
        try:
            size, words = validate_config(text, fmt)
        except Exception as exc:  # noqa: BLE001 - surface any schema/limit failure
            print(f"FAILED: generated config rejected by ha_harmony: {exc}", file=sys.stderr)
            return 1
        print(
            f"validated against config_model + compiler: {size} bytes, {words} pool words",
            file=sys.stderr,
        )
    a.out.write_text(text, encoding="utf-8")
    print(f"wrote {a.out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
