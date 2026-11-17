#!/usr/bin/env python3
"""Extract the stock Harmony Hub IR code database from an unpacked flash tree.

The hub keeps its device database as JSON under the writable data layer (/mnt/data/data/resources,
mtd4): DeviceList.json holds every command as a symbolic KeyCode string such as
`G:GoVideoO1 32 Bit:()(0xE0E040BF)():3`, and ProtocolList.json holds the protocol definitions
(carrier, header/bit/trailer atoms) those codes are expanded with. This tool performs that
expansion offline and writes <out-dir>/ir_codes.json plus a coverage report against a config dump
made by fetch_stock_config.py.

KeyCode grammar: `G:<protocol name>:(<start>)(<repeat>)(<finish>):<min repeats>`. Each paren group
is the payload for that section of the protocol's `KeyCode` template (Start/Repeat/Finish segment
references). A payload `0x...` hex is sent MSB first, one bit per protocol encoding; an empty group
means that section carries no payload of its own.

Durations are alternating mark/space microseconds, mark first. `once` is the Start section and
`repeat` the Repeat section, as the stock protocol defines them. Commands that are not IR
(Bluetooth HID, Roku IP) are recorded with their key and no timings. Anything not understood is
kept raw with a warning; nothing is dropped.

Usage: extract_stock.py <unpacked-tree | image-dir> --out-dir DIR [--config-dir DIR]
"""

import argparse
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any

MAX_US = 65535  # the hub stores durations as u16
KEYCODE_RE = re.compile(r"^G:(?P<proto>[^:]+):(?P<groups>(?:\([^)]*\))+):(?P<n>\d+)$")
HEX_RE = re.compile(r"^0x[0-9A-Fa-f]+$")
SECTIONS = ("Start", "Repeat", "Finish")
NON_IR = {"HID 16 Bit": "hid", "Roku IP": "roku_ecp"}
MARK, SPACE = 1, 0


class DecodeError(ValueError):
    pass


def _atoms(atoms: list[dict[str, Any]]) -> list[tuple[int, int]]:
    out = []
    for a in atoms or []:
        if a.get("Type") not in (MARK, SPACE) or not isinstance(a.get("Value"), int):
            raise DecodeError(f"unsupported atom {a!r}")
        out.append((a["Type"], a["Value"]))
    return out


def flatten(runs: list[tuple[int, int]]) -> list[int]:
    """Merge adjacent same-level runs, drop a leading space; mark-first alternating durations."""
    merged: list[list[int]] = []
    for lvl, d in runs:
        if merged and merged[-1][0] == lvl:
            merged[-1][1] += d
        else:
            merged.append([lvl, d])
    while merged and merged[0][0] == SPACE:
        merged.pop(0)
    return [d for _, d in merged]


def _pad(runs: list[tuple[int, int]], total: int) -> list[tuple[int, int]]:
    used = sum(d for _, d in runs)
    tail = 0
    for lvl, d in reversed(runs):
        if lvl == MARK:
            break
        tail += d
    extra = min(total - used, MAX_US - tail)
    return [*runs, (SPACE, extra)] if extra > 0 else runs


def expand_ir_segment(seg: dict[str, Any], payload: str) -> list[tuple[int, int]]:
    """Expand a protocol IRSegment with a hex payload into (level, us) runs."""
    pl = seg.get("Payload")
    if not pl:
        raise DecodeError("IR segment has no payload definition")
    if pl.get("EncodingType") != 0 or pl.get("ToggleBit") is not None:
        raise DecodeError(f"unsupported payload encoding {pl.get('EncodingType')!r}/toggle")
    if not HEX_RE.match(payload):
        raise DecodeError(f"payload {payload!r} is not 0x-hex")
    nbits = pl["NumberOfBits"]
    value = int(payload, 16)
    if value >> nbits:
        raise DecodeError(f"payload {payload} wider than {nbits} bits")
    enc = {e["BitType"]: _atoms(e["Atoms"]) for e in pl["Encodings"]}
    if set(enc) != {0, 1}:
        raise DecodeError("payload needs encodings for bit 0 and bit 1")
    runs = _atoms(seg.get("Header"))
    for i in range(nbits - 1, -1, -1):
        runs += enc[(value >> i) & 1]
    runs += _atoms(seg.get("Trailer"))
    total = seg.get("TotalLength") or 0
    return _pad(runs, total) if total else runs


def decode_keycode(keycode: str, protocols: dict[str, dict[str, Any]]) -> dict[str, Any]:
    """KeyCode string -> {protocol, carrier_hz, once, repeat, min_repeats, payloads}."""
    m = KEYCODE_RE.match(keycode)
    if not m:
        raise DecodeError("KeyCode does not match G:<proto>:(a)(b)(c):<n>")
    payloads = m["groups"][1:-1].split(")(")
    if len(payloads) != 3:
        raise DecodeError(f"expected 3 payload groups, got {len(payloads)}")
    proto = protocols.get(m["proto"])
    if proto is None:
        raise DecodeError(f"protocol {m['proto']!r} not in ProtocolList")
    if proto.get("__type") != "IrProtocol":
        raise DecodeError(f"protocol type {proto.get('__type')!r} is not IrProtocol")
    segs = {s["Name"]: s for s in proto.get("IRSegments", [])}
    segs.update({s["Name"]: s for s in proto.get("CodeSegments", [])})
    out: dict[str, list[int]] = {}
    template = proto.get("KeyCode") or {}
    for name, payload in zip(SECTIONS, payloads, strict=True):
        refs = template.get(name) or []
        runs: list[tuple[int, int]] = []
        for ref in refs:
            seg = segs.get(ref["SegmentName"])
            if seg is None:
                raise DecodeError(f"segment {ref['SegmentName']!r} missing")
            if seg.get("Payload"):
                runs += expand_ir_segment(seg, payload)
            else:  # fixed code segment: its atoms are the whole signal
                runs += _atoms(seg.get("Atoms"))
        if payload and not refs:
            raise DecodeError(f"{name} payload {payload!r} but protocol has no {name} segment")
        out[name] = flatten(runs)
    return {
        "protocol": m["proto"],
        "carrier_hz": proto["CarrierFrequency"],
        "once": out["Start"],
        "repeat": out["Repeat"],
        "finish": out["Finish"],
        "min_repeats": int(m["n"]),
        "payloads": payloads,
    }


def find_resources(root: Path) -> Path:
    for cand in (root / "data" / "resources", root / "resources", root):
        if (cand / "DeviceList.json").is_file() and (cand / "ProtocolList.json").is_file():
            return cand
    hits = sorted(root.rglob("DeviceList.json"))
    for h in hits:
        if (h.parent / "ProtocolList.json").is_file():
            return h.parent
    raise FileNotFoundError(f"no DeviceList.json + ProtocolList.json under {root}")


def unpack_data_image(image: Path, dest: Path) -> Path:
    """Unpack a jffs2 data image with jefferson (read-only on the image)."""
    exe = shutil.which("jefferson")
    cmd = [exe] if exe else ["uv", "run", "--with", "jefferson", "jefferson"]
    res = subprocess.run(
        [*cmd, "-d", str(dest), str(image)], check=False, capture_output=True, text=True
    )
    if res.returncode:
        raise RuntimeError(f"jefferson failed on {image}: {res.stderr.strip()[-300:]}")
    return dest


def command_key(cmd: dict[str, Any]) -> str:
    ctype = cmd.get("CommandTypeId")
    return ctype if ctype and ctype != "Unknown" else cmd.get("Name", "")


def extract(res_dir: Path, source_root: Path) -> dict[str, Any]:
    warnings: list[dict[str, Any]] = []
    plist = json.loads((res_dir / "ProtocolList.json").read_text())
    protocols = {p["Name"]: p for p in plist.get("Protocols", []) if "Name" in p}
    dl_path = res_dir / "DeviceList.json"
    devs = json.loads(dl_path.read_text())["DevicesWithFeatures"]
    src = (
        str(dl_path.relative_to(source_root))
        if dl_path.is_relative_to(source_root)
        else str(dl_path)
    )
    devices: dict[str, dict[str, Any]] = {}
    for dw in devs:
        dev_id = str(dw["Device"]["Id-"])
        table = devices.setdefault(dev_id, {})
        for cmd in dw.get("Commands", []):
            key = command_key(cmd)
            keycode = cmd.get("KeyCode") or ""
            entry: dict[str, Any] = {
                "name": cmd.get("Name"),
                "key_code": keycode,
                "source_hex": keycode.encode().hex(),
                "source_file": src,
                "learned": bool(cmd.get("IsLearned")),
            }
            kind = next((v for k, v in NON_IR.items() if keycode.startswith(f"G:{k}:")), None)
            if kind:
                m = KEYCODE_RE.match(keycode)
                groups = m["groups"][1:-1].split(")(") if m else []
                entry.update(transport=kind, key=next((g for g in reversed(groups) if g), None))
            elif cmd.get("Raw"):
                entry.update(transport="raw", raw=cmd["Raw"])
                warnings.append(
                    {"device": dev_id, "command": key, "warning": "Raw payload not decoded"}
                )
            else:
                try:
                    entry.update(transport="ir", **decode_keycode(keycode, protocols))
                except (DecodeError, KeyError, TypeError) as e:
                    entry.update(transport="unknown")
                    warnings.append(
                        {"device": dev_id, "command": key, "warning": f"undecoded: {e}"}
                    )
            if key in table:
                warnings.append(
                    {"device": dev_id, "command": key, "warning": "duplicate command key"}
                )
                key = f"{key}#{cmd.get('Id-')}"
            table[key] = entry
    return {"source": src, "warnings": warnings, "devices": devices}


def coverage(db: dict[str, Any], config_dir: Path) -> dict[str, Any]:
    cfg = json.loads((config_dir / "ir_codes.json").read_text())["devices"]
    rows: dict[str, Any] = {}
    tot = {"commands": 0, "ir": 0, "other_transport": 0, "missing": 0}
    for dev_id, cmds in cfg.items():
        have = db["devices"].get(dev_id, {})
        by_name = {e["name"]: e for e in have.values() if e.get("name")}
        row = {"commands": len(cmds), "ir": 0, "other_transport": 0, "missing": [], "other": []}
        for name in cmds:
            e = have.get(name) or by_name.get(name)
            if e and e["transport"] == "ir":
                row["ir"] += 1
            elif e and e["transport"] in ("hid", "roku_ecp"):
                row["other_transport"] += 1
                row["other"].append(name)
            else:
                row["missing"].append(name)
        rows[dev_id] = row
        tot["commands"] += row["commands"]
        tot["ir"] += row["ir"]
        tot["other_transport"] += row["other_transport"]
        tot["missing"] += len(row["missing"])
    return {"total": tot, "devices": rows}


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("source", type=Path, help="unpacked tree, or image dir containing 04_data.img")
    ap.add_argument("--out-dir", type=Path, required=True)
    ap.add_argument("--config-dir", type=Path, help="fetch_stock_config.py output (for coverage)")
    args = ap.parse_args(argv)
    tmp = None
    try:
        root = args.source
        if (root / "04_data.img").is_file():
            tmp = tempfile.TemporaryDirectory()
            root = unpack_data_image(root / "04_data.img", Path(tmp.name) / "data")
        res = find_resources(root)
        db = extract(res, root)
        args.out_dir.mkdir(parents=True, exist_ok=True)
        (args.out_dir / "ir_codes.json").write_text(json.dumps(db, indent=2) + "\n")
        n = sum(len(c) for c in db["devices"].values())
        print(f"{n} commands, {len(db['warnings'])} warnings -> {args.out_dir / 'ir_codes.json'}")
        for w in db["warnings"]:
            print(f"warning: {w['device']}/{w['command']}: {w['warning']}", file=sys.stderr)
        if args.config_dir:
            cov = coverage(db, args.config_dir)
            (args.out_dir / "coverage.json").write_text(json.dumps(cov, indent=2) + "\n")
            t = cov["total"]
            print(
                f"coverage: {t['ir']}/{t['commands']} commands have IR timings; "
                f"{t['other_transport']} are Bluetooth HID / Roku IP (no IR); {t['missing']} missing"
            )
    finally:
        if tmp:
            tmp.cleanup()
    return 0


if __name__ == "__main__":
    sys.exit(main())
