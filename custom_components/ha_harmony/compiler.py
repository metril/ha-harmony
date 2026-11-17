"""Compile a validated model into the hub's config format 2 plus remote pages. Pure, no HA imports."""

from __future__ import annotations

import json
import zlib
from dataclasses import dataclass, field
from typing import Any

from .config_model import Model
from .const import KEY_NAMES, SOFT_KEY_BASE, key_code
from .page_codec import MAX_WIDGETS, Widget, encode_page

# Must equal the HP_MAP_* defines in hub/engine/mapping.h (tests/py/test_compiler.py enforces it).
LIMITS: dict[str, int] = {
    "MAX_JSON": 131072,
    "MAX_DEVICES": 16,
    "MAX_COMMANDS": 256,
    "MAX_INPUTS": 64,
    "MAX_ACTIVITIES": 16,
    "MAX_KEYS": 128,
    "MAX_STEPS": 16,
    "MAX_ACT_DEVS": 8,
    "MAX_PRONTO": 520,
    "POOL_WORDS": 16384,
    "NAME_MAX": 32,
}
MAX_SOFT_KEYS = 256 - SOFT_KEY_BASE  # widget wire ids 0..127


class CompileError(ValueError):
    """The config is valid YAML but does not fit the hub or the remote."""


@dataclass(frozen=True)
class Tile:
    page: int
    widget: int  # wire id
    entity_id: str
    label: str = ""


@dataclass(frozen=True)
class CompiledDevice:
    id: str
    name: str
    power: dict[str, Any] | None
    inputs: tuple[str, ...]
    commands: tuple[str, ...]
    aliases: tuple[str, ...] = ()  # HA-side only: resolved by the coordinator, never sent


@dataclass(frozen=True)
class Compiled:
    hub_json: dict[str, Any]
    pages: list[bytes]
    key_names: dict[int, str]
    tiles: list[Tile]
    activities: list[str] = field(default_factory=list)  # hub activity names, index order
    devices: dict[str, CompiledDevice] = field(default_factory=dict)

    @property
    def config_hash(self) -> str:
        return config_hash(self.hub_json)


def compact_json(obj: Any) -> str:
    """The text the hub reconstructs with cJSON_PrintUnformatted (and hashes)."""
    return json.dumps(obj, separators=(",", ":"), ensure_ascii=False)


def config_hash(hub_json: dict[str, Any]) -> str:
    """Lowercase hex CRC-32 of the compact JSON: equals hello_ack.config_hash once pushed."""
    return f"{zlib.crc32(compact_json(hub_json).encode()):08x}"


def _limit(what: str, n: int, key: str) -> None:
    if n > LIMITS[key]:
        raise CompileError(f"too many {what}: {n} (the hub allows at most {LIMITS[key]})")


def _pool_words(obj: Any) -> int:
    """Pronto words / raw durations the hub stores for every IR action under `obj`."""
    total = 0
    if isinstance(obj, dict):
        for k, v in obj.items():
            if k == "pronto" and isinstance(v, str):
                n = len(v.split())
                if n > LIMITS["MAX_PRONTO"]:
                    raise CompileError(
                        f"pronto code of {n} words (the hub allows {LIMITS['MAX_PRONTO']})"
                    )
                total += n
            elif k == "raw" and isinstance(v, dict):
                total += len(v.get("us", ()))
            else:
                total += _pool_words(v)
    elif isinstance(obj, list):
        total = sum(_pool_words(v) for v in obj)
    return total


def _keymap(keys: dict[str, Any], where: str, names: dict[int, str]) -> dict[str, Any]:
    out: dict[str, Any] = {}
    for name, action in keys.items():
        code = key_code(name)
        assert code is not None  # schema-validated
        if str(code) in out:
            raise CompileError(f"{where}: key {name!r} (code {code}) is mapped twice")
        out[str(code)] = action
        names[code] = KEY_NAMES.get(code, f"key_{code}")
    return out


def compile_config(model: Model) -> Compiled:
    """Model -> Compiled. Raises CompileError when the hub or the remote cannot take it."""
    names: dict[int, str] = {}
    devices: list[dict[str, Any]] = []
    for d in model["devices"]:
        dev: dict[str, Any] = {"id": d["id"], "name": d.get("name", d["id"])}
        if d.get("power"):
            dev["power"] = d["power"]
        if d["inputs"]:
            dev["inputs"] = d["inputs"]
        if "settle_ms" in d:
            dev["settle_ms"] = d["settle_ms"]
        if d["commands"]:
            dev["commands"] = d["commands"]
        devices.append(dev)
    _limit("devices", len(devices), "MAX_DEVICES")
    _limit("commands", sum(len(d["commands"]) for d in model["devices"]), "MAX_COMMANDS")
    _limit("inputs", sum(len(d["inputs"]) for d in model["devices"]), "MAX_INPUTS")

    activities: list[dict[str, Any]] = []
    for a in model["activities"]:
        out: dict[str, Any] = {"id": a["id"], "name": a.get("name", a["id"])}
        if a["devices"]:
            out["devices"] = a["devices"]
        for sect in ("steps", "off"):
            if a[sect]:
                out[sect] = a[sect]
                if len(a[sect]) > LIMITS["MAX_STEPS"]:
                    raise CompileError(
                        f"activity {a['id']!r}: too many `{sect}`: {len(a[sect])}"
                        f" (the hub allows at most {LIMITS['MAX_STEPS']})"
                    )
        if len(a["devices"]) > LIMITS["MAX_ACT_DEVS"]:
            raise CompileError(
                f"activity {a['id']!r}: {len(a['devices'])} devices"
                f" (the hub allows at most {LIMITS['MAX_ACT_DEVS']})"
            )
        keys = _keymap(a["keys"], f"activity {a['id']!r}", names)
        if len(keys) > LIMITS["MAX_KEYS"]:
            raise CompileError(f"activity {a['id']!r}: too many keys: {len(keys)}")
        if keys:
            out["keys"] = keys
        activities.append(out)
    _limit("activities", len(activities), "MAX_ACTIVITIES")
    if len({a["name"] for a in activities}) != len(activities):
        raise CompileError("activity names must be unique")

    keys = _keymap(model["keys"], "keys", names)

    pages: list[bytes] = []
    tiles: list[Tile] = []
    wire = 0
    for pi, p in enumerate(model["pages"]):
        widgets: list[Widget] = []
        for w in p["widgets"]:
            if wire >= MAX_SOFT_KEYS:
                raise CompileError(
                    f"too many widgets across all pages: more than {MAX_SOFT_KEYS} soft keys"
                )
            widgets.append(Widget(w["type"], wire, w["x"], w["y"], w["w"], w["h"], w["label"]))
            code = SOFT_KEY_BASE + wire
            if w["type"] != "label":
                names[code] = f"tap_{w['id']}"
            if w["type"] == "button":
                keys[str(code)] = w["action"]
            elif w["type"] == "tile":
                tiles.append(Tile(pi, wire, w["entity"], w["label"]))
            wire += 1
        if len(widgets) > MAX_WIDGETS:
            raise CompileError(
                f"page {p['id']!r}: too many widgets: {len(widgets)} (max {MAX_WIDGETS})"
            )
        try:
            pages.append(encode_page(pi, p["cols"], p["rows"], widgets))
        except ValueError as exc:
            raise CompileError(f"page {p['id']!r}: {exc}") from None
    if len(keys) > LIMITS["MAX_KEYS"]:
        raise CompileError(
            f"too many keys in the default map: {len(keys)} including page buttons"
            f" (the hub allows at most {LIMITS['MAX_KEYS']})"
        )

    hub: dict[str, Any] = {"format": 2, "devices": devices, "activities": activities, "keys": keys}
    words = _pool_words(hub)
    if words > LIMITS["POOL_WORDS"]:
        raise CompileError(
            f"IR data is {words} words in total (the hub's pool holds {LIMITS['POOL_WORDS']});"
            " use protocol codes or fewer raw/pronto codes"
        )
    size = len(compact_json(hub).encode())
    if size > LIMITS["MAX_JSON"]:
        raise CompileError(f"compiled config is {size} bytes (the hub allows {LIMITS['MAX_JSON']})")

    cdevs = {
        d["id"]: CompiledDevice(
            d["id"],
            d.get("name", d["id"]),
            d.get("power"),
            tuple(d["inputs"]),
            tuple(d["commands"]),
            tuple(d.get("aliases", ())),
        )
        for d in model["devices"]
    }
    return Compiled(
        hub_json=hub,
        pages=pages,
        key_names=names,
        tiles=tiles,
        activities=[a["name"] for a in activities],
        devices=cdevs,
    )
