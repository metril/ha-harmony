"""Compile a validated model into the hub's config format 2 plus remote pages. Pure, no HA imports."""

from __future__ import annotations

import json
import zlib
from collections.abc import Callable
from dataclasses import dataclass, field
from typing import Any

from .config_model import Model
from .ha_actions import HaCall, call_of
from .harmony_proto import SOFT_KEY_BASE
from .keymap import effective
from .keys import KEY_NAMES, key_code
from .page_build import MAX_PAGES, MAX_SOFT_KEYS, CompileError, Tile, build_pages, encode_pages
from .page_codec import MAX_BYTES, MAX_WIDGETS, Page, page_size

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
__all__ = ["MAX_SOFT_KEYS", "CompileError", "Tile"]


@dataclass(frozen=True)
class CompiledDevice:
    id: str
    name: str
    power: dict[str, Any] | None
    inputs: tuple[str, ...]
    commands: tuple[str, ...]
    aliases: tuple[str, ...] = ()  # HA-side only: resolved by the coordinator, never sent
    ha: bool = False  # a Home Assistant device: no IR, run by the integration


@dataclass(frozen=True)
class HaDevice:
    """A device run by Home Assistant services (never sent to the hub)."""

    id: str
    entity: str | None
    settle_ms: int
    on: HaCall | None
    off: HaCall | None
    inputs: dict[str, HaCall] = field(default_factory=dict)
    commands: dict[str, HaCall] = field(default_factory=dict)

    def lookup(self, command: str) -> HaCall | None:
        """`power_on`, `power_off`, `input_<name>` or a configured command -> its call."""
        if command == "power_on":
            return self.on
        if command == "power_off":
            return self.off
        if command.startswith("input_") and command[6:] in self.inputs:
            return self.inputs[command[6:]]
        return self.commands.get(command)


@dataclass(frozen=True)
class HaStep:
    call: HaCall
    device: str | None = None  # the HA device it belongs to (power steps check its state)
    wait_ms: int = 0  # settle time before this step
    kind: str = "step"  # power_on | input | step


@dataclass(frozen=True)
class HaActivity:
    """What the integration does around the hub's own IR sequence (docs/ha-config.md)."""

    id: str
    devices: tuple[str, ...]  # HA devices the activity uses, in order
    start: tuple[HaStep, ...]  # power-on, inputs, then extra steps: after the hub reports it running
    off: tuple[HaStep, ...]  # extra `off` steps: when the hub reports everything off


@dataclass(frozen=True)
class Compiled:
    hub_json: dict[str, Any]
    pages: list[bytes]  # page format v1: title as a label, no extras (remotes without v2)
    key_names: dict[int, str]
    tiles: list[Tile]
    activities: list[str] = field(default_factory=list)  # hub activity names, index order
    devices: dict[str, CompiledDevice] = field(default_factory=dict)
    pages_v2: list[bytes] = field(default_factory=list)  # page format v2, same page ids
    models: list[Page] = field(default_factory=list)  # the v2 page models (gen filled)

    def encoded_pages(self, version: int) -> list[bytes]:
        """The page set for a remote speaking page format `version` (1 or 2)."""
        return self.pages_v2 if version >= 2 else self.pages
    page_of: list[int] = field(default_factory=list)  # config page index behind each wire page
    # Home Assistant side (never sent to the hub): the action table is flattened with the keymap's
    # inheritance rule: layer (activity id, None = default / nothing running) -> key code -> call.
    ha_keys: dict[str | None, dict[int, HaCall]] = field(default_factory=dict)
    ha_devices: dict[str, HaDevice] = field(default_factory=dict)
    ha_activities: dict[str, HaActivity] = field(default_factory=dict)
    activity_ids: list[str] = field(default_factory=list)  # index = the hub's activity index
    # Measured sizes against the hub's and remote's limits (the editor's meters). Not part of
    # equality: two compiles of the same config are the same config.
    usage: dict[str, Any] = field(default_factory=dict, compare=False)

    @property
    def config_hash(self) -> str:
        return config_hash(self.hub_json)

    @property
    def gen(self) -> int:
        """The generation byte of the v2 page set (0 when there are no pages)."""
        return self.models[0].gen if self.models else 0


def compact_json(obj: Any) -> str:
    """The text the hub reconstructs with cJSON_PrintUnformatted (and hashes)."""
    return json.dumps(obj, separators=(",", ":"), ensure_ascii=False)


def config_hash(hub_json: dict[str, Any]) -> str:
    """Lowercase hex CRC-32 of the compact JSON: equals hello_ack.config_hash once pushed."""
    return f"{zlib.crc32(compact_json(hub_json).encode()):08x}"


Problem = tuple[list[Any], str]  # (path in the config, message)


def _limit(what: str, n: int, key: str, fail: Callable[..., None]) -> None:
    if n > LIMITS[key]:
        fail(f"too many {what}: {n} (the hub allows at most {LIMITS[key]})")


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


def _keymap(
    keys: dict[str, Any], where: str, names: dict[int, str], fail: Callable[..., None]
) -> dict[str, Any]:
    out: dict[str, Any] = {}
    for name, action in keys.items():
        code = key_code(name)
        assert code is not None  # schema-validated
        if str(code) in out:
            fail(f"{where}: key {name!r} (code {code}) is mapped twice")
        out[str(code)] = action
        names[code] = KEY_NAMES.get(code, f"key_{code}")
    return out


def compile_config(model: Model, *, collect: list[Problem] | None = None) -> Compiled:
    """Model -> Compiled. Raises CompileError when the hub or the remote cannot take it.

    With `collect` (previews only) nothing is raised: every problem is appended as (path, msg),
    pages that do not fit come back as b"", and the result is not fit to send to a hub."""

    def fail(msg: str, path: list[Any] | None = None) -> None:
        if collect is None:
            raise CompileError(msg)
        collect.append((path or [], msg))

    names: dict[int, str] = {}
    ha_devs = {d["id"]: d for d in model["devices"] if d.get("kind") == "ha"}

    def ha_of(a: dict[str, Any]) -> HaCall | None:
        """The HA call behind an action: an `ha:` action, or a reference to an HA device."""
        if a["type"] == "ha_call":
            return call_of(a)
        if a["type"] == "device" and a["device"] in ha_devs:
            d = ha_devs[a["device"]]
            if "input" in a:
                return call_of(d["inputs"][a["input"]])
            if a["command"] in ("power_on", "power_off"):
                return call_of(d["power"]["on" if a["command"] == "power_on" else "off"])
            return call_of(d["commands"][a["command"]])
        return None

    devices: list[dict[str, Any]] = []
    for d in model["devices"]:
        if d["id"] in ha_devs:
            continue
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
    _limit("devices", len(devices), "MAX_DEVICES", fail)
    ir_models = [d for d in model["devices"] if d["id"] not in ha_devs]
    _limit("commands", sum(len(d["commands"]) for d in ir_models), "MAX_COMMANDS", fail)
    _limit("inputs", sum(len(d["inputs"]) for d in ir_models), "MAX_INPUTS", fail)

    activities: list[dict[str, Any]] = []
    act_keys: dict[int, dict[str, Any]] = {}
    for ai, a in enumerate(model["activities"]):
        where = ["activities", ai]
        out: dict[str, Any] = {"id": a["id"], "name": a.get("name", a["id"])}
        ir_act_devs = [x for x in a["devices"] if x["device"] not in ha_devs]
        if ir_act_devs:
            out["devices"] = ir_act_devs
        for sect in ("steps", "off"):
            hub_steps = [x for x in a[sect] if ha_of(x) is None]  # HA steps run in the integration
            if hub_steps:
                out[sect] = hub_steps
                if len(hub_steps) > LIMITS["MAX_STEPS"]:
                    fail(
                        f"activity {a['id']!r}: too many `{sect}`: {len(hub_steps)}"
                        f" (the hub allows at most {LIMITS['MAX_STEPS']})",
                        [*where, sect],
                    )
        if len(ir_act_devs) > LIMITS["MAX_ACT_DEVS"]:
            fail(
                f"activity {a['id']!r}: {len(ir_act_devs)} devices"
                f" (the hub allows at most {LIMITS['MAX_ACT_DEVS']})",
                [*where, "devices"],
            )
        act_keys[ai] = _keymap(a["keys"], f"activity {a['id']!r}", names, fail)
        activities.append(out)
    _limit("activities", len(activities), "MAX_ACTIVITIES", fail)
    if len({a["name"] for a in activities}) != len(activities):
        fail("activity names must be unique", ["activities"])

    keys = _keymap(model["keys"], "keys", names, fail)

    act_ids = [a["id"] for a in model["activities"]]
    wire_count = 0
    try:
        pages, cfg_ids, tiles = build_pages(model, act_ids, names, keys)
    except CompileError as exc:
        fail(str(exc), exc.path or ["pages"])
        pages, cfg_ids, tiles = [], [], []
        wire_count = exc.count or 0

    # Split off what Home Assistant runs: the hub only ever sees (and forwards) the rest. The
    # table is flattened with keymap.effective, the same rule the hub and the X-ray view follow.
    default_full = {int(c): x for c, x in keys.items()}
    ha_keys: dict[str | None, dict[int, HaCall]] = {
        None: {c: h for c, x in default_full.items() if (h := ha_of(x)) is not None}
    }
    keys = {c: x for c, x in keys.items() if ha_of(x) is None}
    for ai, a in enumerate(model["activities"]):
        own = {int(c): x for c, x in act_keys[ai].items()}
        ha_keys[a["id"]] = {
            c: h for c, (_s, x) in effective(own, default_full).items() if (h := ha_of(x)) is not None
        }
        hub_own: dict[str, Any] = {}
        for c, x in own.items():
            if ha_of(x) is None:
                hub_own[str(c)] = x
            elif str(c) in keys:  # an inherited hub action must not fire under the HA one
                hub_own[str(c)] = {"type": "none"}
        if len(hub_own) > LIMITS["MAX_KEYS"]:
            fail(f"activity {a['id']!r}: too many keys: {len(hub_own)}", ["activities", ai, "keys"])
        if hub_own:
            activities[ai]["keys"] = hub_own
    if len(keys) > LIMITS["MAX_KEYS"]:
        fail(
            f"too many keys in the default map: {len(keys)} including page buttons"
            f" (the hub allows at most {LIMITS['MAX_KEYS']})",
            ["keys"],
        )

    hub: dict[str, Any] = {"format": 2, "devices": devices, "activities": activities, "keys": keys}
    try:
        words = _pool_words(hub)
    except CompileError as exc:
        fail(str(exc))
        words = 0
    if words > LIMITS["POOL_WORDS"]:
        fail(
            f"IR data is {words} words in total (the hub's pool holds {LIMITS['POOL_WORDS']});"
            " use protocol codes or fewer raw/pronto codes"
        )
    size = len(compact_json(hub).encode())
    if size > LIMITS["MAX_JSON"]:
        fail(f"compiled config is {size} bytes (the hub allows {LIMITS['MAX_JSON']})")

    try:
        models, pages_v1, pages_v2 = encode_pages(
            pages, cfg_ids, compact_json(hub).encode(), collect
        )
    except CompileError as exc:
        fail(str(exc), exc.path or ["pages"])
        models, pages_v1, pages_v2 = [], [], []
    order = {cid: i for i, cid in enumerate(dict.fromkeys(cfg_ids))}
    page_of = [order[c] for c in cfg_ids] if models else []
    page_usage = [
        {
            "cfg_page": cfg,
            "bytes": page_size(m, 2),
            "bytes_v1": page_size(m, 1),
            "limit": MAX_BYTES,
            "widgets": len(m.widgets),
            "widget_limit": MAX_WIDGETS,
        }
        for cfg, m in zip(page_of, models, strict=False)
    ]

    cdevs = {
        d["id"]: CompiledDevice(
            d["id"],
            d.get("name", d["id"]),
            d.get("power"),
            tuple(d["inputs"]),
            tuple(d["commands"]),
            tuple(d.get("aliases", ())),
            d["id"] in ha_devs,
        )
        for d in model["devices"]
    }
    ha_devices = {
        i: HaDevice(
            i,
            d.get("entity"),
            d.get("settle_ms", 0),
            call_of(d["power"]["on"]) if d.get("power") else None,
            call_of(d["power"]["off"]) if d.get("power") else None,
            {n: call_of(x) for n, x in d["inputs"].items()},
            {n: call_of(x) for n, x in d["commands"].items()},
        )
        for i, d in ha_devs.items()
    }
    ha_activities: dict[str, HaActivity] = {}
    for a in model["activities"]:
        used = [x for x in a["devices"] if x["device"] in ha_devs]
        start = [
            HaStep(ha_devices[x["device"]].on, x["device"], 0, "power_on")
            for x in used
            if ha_devices[x["device"]].on is not None
        ]
        powered = {s_.device for s_ in start}
        start += [
            HaStep(
                call_of(ha_devs[x["device"]]["inputs"][x["input"]]),
                x["device"],
                ha_devices[x["device"]].settle_ms if x["device"] in powered else 0,
                "input",
            )
            for x in used
            if "input" in x
        ]
        start += [HaStep(h) for s_ in a["steps"] if (h := ha_of(s_)) is not None]
        off = [HaStep(h) for s_ in a["off"] if (h := ha_of(s_)) is not None]
        if used or start or off:
            ha_activities[a["id"]] = HaActivity(
                a["id"], tuple(x["device"] for x in used), tuple(start), tuple(off)
            )
    usage = {
        "json_bytes": size,
        "json_max": LIMITS["MAX_JSON"],
        "pool_words": words,
        "pool_max": LIMITS["POOL_WORDS"],
        "keys": len(keys),
        "keys_max": LIMITS["MAX_KEYS"],
        "devices": len(devices),
        "devices_max": LIMITS["MAX_DEVICES"],
        "activities": len(activities),
        "activities_max": LIMITS["MAX_ACTIVITIES"],
        "commands": sum(len(d["commands"]) for d in ir_models),
        "commands_max": LIMITS["MAX_COMMANDS"],
        "inputs": sum(len(d["inputs"]) for d in ir_models),
        "inputs_max": LIMITS["MAX_INPUTS"],
        "soft_keys": sum(1 for c in names if c >= SOFT_KEY_BASE),
        "soft_keys_max": MAX_SOFT_KEYS,
        "pages": max(wire_count, len(models)),
        "page_limit": MAX_PAGES,
        "page_detail": page_usage,
    }
    return Compiled(
        hub_json=hub,
        pages=pages_v1,
        pages_v2=pages_v2,
        models=models,
        page_of=page_of,
        key_names=names,
        tiles=tiles,
        activities=[a["name"] for a in activities],
        devices=cdevs,
        usage=usage,
        ha_keys=ha_keys,
        ha_devices=ha_devices,
        ha_activities=ha_activities,
        activity_ids=[a["id"] for a in model["activities"]],
    )
