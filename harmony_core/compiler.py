"""Compile a validated model into the hub's config format 2 plus remote pages. Pure, no HA imports."""

from __future__ import annotations

import base64
import json
import zlib
from collections.abc import Callable
from dataclasses import dataclass, field, replace
from functools import cached_property
from typing import Any

from .config_model import Model
from .ha_actions import HaCall, call_of
from .harmony_proto import SOFT_KEY_BASE
from .hub_limits import CURRENT, HubLimits, estimate_bytes
from .keymap import effective
from .keys import KEY_NAMES, UNKNOWN_KEY, key_code
from .page_build import (
    MAX_PAGES,
    MAX_SOFT_KEYS,
    CompileError,
    Tile,
    build_pages,
    encode_pages,
    overflow_warnings,
)
from .page_codec import MAX_BYTES, MAX_WIDGETS, Page, page_size
from .theme_build import (
    CAP_THEME_COLOR,
    CAP_THEME_CUSTOM,
    CAPS_ALL,
    ActiveTheme,
    active_theme,
    degrade_pages,
    has_theming,
    theme_warnings,
)
from .theme_math import ThemeError

__all__ = ["MAX_SOFT_KEYS", "CompileError", "Tile", "config_stats", "fit_error"]


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
    start: tuple[
        HaStep, ...
    ]  # power-on, inputs, then extra steps: after the hub reports it running
    off: tuple[HaStep, ...]  # extra `off` steps: when the hub reports everything off


@dataclass(frozen=True)
class RemotePages:
    """The v2 page set for one remote: the compiled pages degraded to what its caps can show."""

    models: list[Page]
    pages_v2: list[bytes]
    pages_v2_b64: list[str]
    gen: int
    theme_id: int
    theme_wire: bytes | None  # a user theme's 99 bytes, only when the caps can take it
    notices: list[dict[str, Any]]


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
    theme_id: int = 0  # preset id 0..6, or 7 for a user theme
    theme_wire: bytes | None = None  # the 99 wire bytes of a user theme (None for presets)
    theme_name: str = "default"
    theme_base: str = "default"  # the preset a user theme extends (itself for a preset)
    # Non-blocking problems ({path, msg, severity: "warning"}); not part of equality.
    warnings: list[dict[str, Any]] = field(default_factory=list, compare=False)
    cfg_ids: list[str] = field(default_factory=list, compare=False)
    seed: bytes = field(default=b"", compare=False)  # the gen seed without the theme bytes

    @cached_property
    def _variants(self) -> dict[int, RemotePages]:
        return {}

    def for_caps(self, caps: int) -> RemotePages:
        """The v2 set for a remote announcing `caps` (bit 2 colours, bit 3 free colours + user
        theme): degraded as docs/ha-config.md "Colours and themes" says, with its own gen."""
        tier = caps & (CAP_THEME_COLOR | CAP_THEME_CUSTOM)
        if tier not in self._variants:
            self._variants[tier] = self._for_tier(tier)
        return self._variants[tier]

    def _for_tier(self, tier: int) -> RemotePages:
        enc = base64.b64encode
        if tier == CAPS_ALL or not has_theming(self.models):
            return RemotePages(
                self.models,
                self.pages_v2,
                self._pages_b64[1],
                self.gen,
                self.theme_id,
                self.theme_wire,
                [],
            )
        at = ActiveTheme(self.theme_name, self.theme_id, self.theme_base, self.theme_wire)
        pages, notices = degrade_pages(self.models, self.cfg_ids, at, tier)
        if pages is self.models:  # this remote can show everything there is
            return RemotePages(
                self.models, self.pages_v2, self._pages_b64[1], self.gen, self.theme_id, None, notices
            )
        pages = [replace(p, gen=0) for p in pages]
        models, _v1, v2 = encode_pages(pages, self.cfg_ids, self.seed, [])
        return RemotePages(
            models,
            v2,
            [enc(p).decode() for p in v2],
            models[0].gen if models else 0,
            models[0].theme if models else 0,
            None,
            notices,
        )

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

    @cached_property
    def config_hash(self) -> str:
        """Computed once per compile (a compiled config is never mutated)."""
        return config_hash(self.hub_json)

    @cached_property
    def _pages_b64(self) -> tuple[list[str], list[str]]:
        enc = base64.b64encode
        return [enc(p).decode() for p in self.pages], [enc(p).decode() for p in self.pages_v2]

    def encoded_pages_b64(self, version: int) -> list[str]:
        """The same set as `encoded_pages`, as the base64 text the ws carries (built once)."""
        return self._pages_b64[1 if version >= 2 else 0]

    @cached_property
    def event_types(self) -> list[str]:
        """Every key name the event entity can report, sorted; built once per compile."""
        return sorted({*KEY_NAMES.values(), *self.key_names.values(), UNKNOWN_KEY})

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


def _limit(what: str, n: int, cap: int | None, fail: Callable[..., None]) -> None:
    if cap is not None and n > cap:
        fail(f"too many {what}: {n} (the hub allows at most {cap})")


def config_stats(hub_json: dict[str, Any], max_pronto: int = 520) -> dict[str, int]:
    """Counts the hub's memory estimate is made of, for the compact text of `hub_json`."""
    devs = hub_json.get("devices", [])
    acts = hub_json.get("activities", [])
    return {
        "text": len(compact_json(hub_json).encode()),
        "devices": len(devs),
        "commands": sum(len(d.get("commands", ())) for d in devs),
        "inputs": sum(len(d.get("inputs", ())) for d in devs),
        "keys": len(hub_json.get("keys", ()))
        + sum(len(a.get("keys", ())) for a in acts)
        + sum(len(r["keys"]) for r in hub_json.get("bt_remotes", ())),  # counted as key-map entries
        "words": _pool_words(hub_json, max_pronto),
        "bt_hosts": len(hub_json.get("bt_hosts", ())),
        "bt_remotes": len(hub_json.get("bt_remotes", ())),
    }


def fit_error(stats: dict[str, int], lim: HubLimits) -> str | None:
    """Why a hub with limits `lim` cannot take a config of `stats`, or None. Flash first, then the one byte budget: the message a hub itself would give."""
    if lim.flash_low:
        return "the hub's flash is full: there is no room for a config. Free space on the hub first"
    if lim.flash_free and stats["text"] + 4096 + 13 > lim.flash_free:
        return (
            f"config needs ~{-(-(stats['text'] + 4096) // 1024)} KiB of flash,"
            f" the hub has {lim.flash_free // 1024} KiB free"
        )
    need = estimate_bytes(lim, **stats)
    if need > lim.budget:
        return (
            f"config needs ~{-(-need // 1024)} KiB of hub memory, the hub allows"
            f" {lim.budget // 1024} KiB"
        )
    return None


def _pool_words(obj: Any, max_pronto: int = 520) -> int:
    """Pronto words / raw durations the hub stores for every IR action under `obj`."""
    total = 0
    if isinstance(obj, dict):
        for k, v in obj.items():
            if k == "pronto" and isinstance(v, str):
                n = len(v.split())
                if n > max_pronto:
                    raise CompileError(f"pronto code of {n} words (the hub allows {max_pronto})")
                total += n
            elif k == "raw" and isinstance(v, dict):
                total += len(v.get("us", ()))
            else:
                total += _pool_words(v, max_pronto)
    elif isinstance(obj, list):
        total = sum(_pool_words(v, max_pronto) for v in obj)
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


def compile_config(
    model: Model, *, collect: list[Problem] | None = None, limits: HubLimits = CURRENT
) -> Compiled:
    """Model -> Compiled. Raises CompileError when the hub or the remote cannot take it.

    With `collect` (previews only) nothing is raised: every problem is appended as (path, msg),
    pages that do not fit come back as b"", and the result is not fit to send to a hub.

    `limits` are the connected hub's (hello_ack.limits); the default is a hub with the fixed
    limits of firmware <= 0.3.5."""

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
    bt_hosts: list[dict[str, Any]] = []
    for d in model["devices"]:
        if d["id"] in ha_devs:
            continue
        dev: dict[str, Any] = {"id": d["id"], "name": d.get("name", d["id"])}
        if d.get("kind") == "bt":  # slot = index among the bt devices, in config order
            slot = len(bt_hosts)
            bt_hosts.append(
                {
                    "slot": slot,
                    "addr": d.get("address"),
                    "name": dev["name"],
                    "transport": d.get("transport", "auto"),
                }
            )
            dev["commands"] = {n: {**c, "host": slot} for n, c in d["commands"].items()}
            devices.append(dev)
            continue
        if d.get("power"):
            dev["power"] = d["power"]
        if d["inputs"]:
            dev["inputs"] = d["inputs"]
        if "settle_ms" in d:
            dev["settle_ms"] = d["settle_ms"]
        if d["commands"]:
            dev["commands"] = d["commands"]
        devices.append(dev)
    _limit("devices", len(devices), limits.max_devices, fail)
    ir_models = [d for d in model["devices"] if d["id"] not in ha_devs]

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
                if len(hub_steps) > limits.max_steps:
                    fail(
                        f"activity {a['id']!r}: too many `{sect}`: {len(hub_steps)}"
                        f" (the hub allows at most {limits.max_steps})",
                        [*where, sect],
                    )
        if len(ir_act_devs) > limits.max_act_devs:
            fail(
                f"activity {a['id']!r}: {len(ir_act_devs)} devices"
                f" (the hub allows at most {limits.max_act_devs})",
                [*where, "devices"],
            )
        act_keys[ai] = _keymap(a["keys"], f"activity {a['id']!r}", names, fail)
        activities.append(out)
    _limit("activities", len(activities), limits.max_activities, fail)
    if len({a["name"] for a in activities}) != len(activities):
        fail("activity names must be unique", ["activities"])

    keys = _keymap(model["keys"], "keys", names, fail)

    act_ids = [a["id"] for a in model["activities"]]
    wire_count = 0
    try:
        at = active_theme(model)
    except ThemeError as exc:  # parse_model rejects these; a partial preview model may not
        fail(str(exc), ["theme"])
        at = ActiveTheme()
    try:
        pages, cfg_ids, tiles = build_pages(model, act_ids, names, keys, at.id)
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
            c: h
            for c, (_s, x) in effective(own, default_full).items()
            if (h := ha_of(x)) is not None
        }
        hub_own: dict[str, Any] = {}
        for c, x in own.items():
            if ha_of(x) is None:
                hub_own[str(c)] = x
            elif str(c) in keys:  # an inherited hub action must not fire under the HA one
                hub_own[str(c)] = {"type": "none"}
        if len(hub_own) > limits.max_keys:
            fail(f"activity {a['id']!r}: too many keys: {len(hub_own)}", ["activities", ai, "keys"])
        if hub_own:
            activities[ai]["keys"] = hub_own
    if len(keys) > limits.max_keys:
        fail(
            f"too many keys in the default map: {len(keys)} including page buttons"
            f" (the hub allows at most {limits.max_keys})",
            ["keys"],
        )

    hub: dict[str, Any] = {"format": 2, "devices": devices, "activities": activities, "keys": keys}
    if bt_hosts:
        hub["bt_hosts"] = bt_hosts
    if model.get("bt_remotes"):
        hub["bt_remotes"] = [
            {
                "slot": i,
                "addr": r.get("address"),
                "name": r["name"],
                "keys": [[p, u, key_code(k)] for p, u, k in r["keys"]],
            }
            for i, r in enumerate(model["bt_remotes"])
        ]
    try:
        stats = config_stats(hub, limits.max_pronto)
    except CompileError as exc:
        fail(str(exc))
        stats = dict.fromkeys(("text", "devices", "commands", "inputs", "keys", "words"), 0)
    if (why := fit_error(stats, limits)) is not None:
        fail(why)
    size, words = stats["text"], stats["words"]
    need = estimate_bytes(limits, **stats)

    seed = compact_json(hub).encode()
    try:
        models, pages_v1, pages_v2 = encode_pages(pages, cfg_ids, seed + (at.wire or b""), collect)
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
        "pool_words": words,
        "keys": len(keys),
        "keys_max": limits.max_keys,
        "devices": len(devices),
        "devices_max": limits.max_devices,
        "activities": len(activities),
        "activities_max": limits.max_activities,
        "commands": sum(len(d["commands"]) for d in ir_models),
        "inputs": sum(len(d["inputs"]) for d in ir_models),
        # one byte budget for the whole config
        "config_bytes": need,
        "config_budget": limits.budget,
        "config_overhead": limits.slack_b,  # the hub's fixed part of a load, from its own limits
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
        theme_id=at.id,
        theme_wire=at.wire,
        theme_name=at.name,
        theme_base=at.base,
        warnings=[*theme_warnings(model), *overflow_warnings(models, cfg_ids, model)],
        cfg_ids=cfg_ids,
        seed=seed,
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
