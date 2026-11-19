"""What a hub accepts as a config. Pure, no HA imports.

Hubs report their limits in `hello_ack.limits` (see docs/proto/config-format-2.md "Limits"). A hub
that reports none (firmware <= 0.3.5) has the fixed count limits of `LEGACY`; a current hub has no
count limits for commands, inputs, devices' codes or the IR word pool, only ONE memory budget, plus
the per-item bounds below. `CURRENT` mirrors hub/engine/mapping.h; tests/py/test_limits.py parses
the header and fails when the two differ.
"""

from __future__ import annotations

from dataclasses import dataclass, replace
from typing import Any

__all__ = ["CURRENT", "LEGACY", "LIMITS_HEADER", "HubLimits", "estimate_bytes"]

LIMITS_HEADER = (
    "X-Hub-Limits"  # the integration tells the add-on the connected hub's limits (JSON, `to_wire`)
)

_BOUNDS = (  # key of hello_ack.limits -> sane range (a hub reporting anything else is ignored)
    ("budget", 64 * 1024, 1 << 30),
    ("chunk", 256, 60000),
    ("slack_b", 0, 1 << 30),
    ("flash_free", 0, 1 << 40),
    ("dev_b", 16, 8192),
    ("cmd_b", 16, 8192),
    ("key_b", 8, 1024),
    ("word_b", 1, 16),
    ("max_devices", 1, 65535),
    ("max_activities", 1, 255),
    ("max_keys", 1, 65535),
    ("max_steps", 1, 255),
    ("max_act_devs", 1, 255),
    ("max_pronto", 4, 65535),
    ("name_max", 2, 256),
)


@dataclass(frozen=True)
class HubLimits:
    """Per-item bounds, and either a memory budget (budget > 0) or legacy fixed totals."""

    max_devices: int
    max_activities: int
    max_keys: int  # entries per key map
    max_steps: int
    max_act_devs: int
    max_pronto: int
    name_max: int  # bytes + NUL
    budget: int = 0  # bytes one config's tables may need (0 = legacy hub); a load builds beside the old map
    chunk: int = 0  # config_chunk data bytes the hub takes (0 = no chunked transfer)
    slack_b: int = 1536 * 1024  # fixed part of a load (scratch map, the value in hand, sections)
    flash_free: int = 0  # bytes free for the cached config when the hub said hello (0 = unknown, unless flash_low)
    flash_low: bool = False  # the hub has no flash left for a config (flash_free is then 0): not "unknown"
    dev_b: int = 224
    cmd_b: int = 120  # one command or input
    key_b: int = 72
    word_b: int = 2
    # legacy hubs only (None = no such limit)
    max_json: int | None = None
    max_commands: int | None = None
    max_inputs: int | None = None
    pool_words: int | None = None

    @property
    def legacy(self) -> bool:
        return self.budget == 0

    @classmethod
    def from_hello(cls, raw: Any) -> HubLimits:
        """The limits a hello_ack carries; LEGACY when absent or not credible."""
        if not isinstance(raw, dict):
            return LEGACY
        vals: dict[str, int] = {}
        for key, lo, hi in _BOUNDS:
            v = raw.get(key, 0 if key == "flash_free" else None)  # flash_free is optional
            if type(v) is not int or not lo <= v <= hi:
                return LEGACY
            vals[key] = v
        return cls(**vals, flash_low=raw.get("flash_low") is True and vals["flash_free"] == 0)

    def to_wire(self) -> dict[str, Any]:
        """The `limits` object (a hello_ack as sent; also what the add-on is told about a hub)."""
        if self.legacy:
            return {}
        wire: dict[str, Any] = {key: getattr(self, key) for key, _lo, _hi in _BOUNDS}
        if self.flash_low:
            wire["flash_low"] = True
        return wire

    def with_budget(self, budget: int) -> HubLimits:
        return replace(self, budget=budget)


# Firmware <= 0.3.5: fixed totals, no hello_ack.limits, no config_chunk.
LEGACY = HubLimits(
    max_devices=16,
    max_activities=16,
    max_keys=128,
    max_steps=16,
    max_act_devs=8,
    max_pronto=520,
    name_max=32,
    max_json=131072,
    max_commands=256,
    max_inputs=64,
    pool_words=16384,
)

# The newest firmware (hub/engine/mapping.h); what a config is compiled against until a hub reports.
CURRENT = HubLimits(
    max_devices=255,
    max_activities=16,
    max_keys=256,
    max_steps=16,
    max_act_devs=8,
    max_pronto=520,
    name_max=32,
    budget=8 * 1024 * 1024,
    chunk=16384,
)


def bt_fit_error(hub_json: dict[str, Any], bt: Any) -> str | None:
    """Why a hub that reported `bt` (hello_ack.bt, None = no Bluetooth support) cannot take the
    config's Bluetooth devices / remotes, or None."""
    hosts, remotes = len(hub_json.get("bt_hosts", ())), len(hub_json.get("bt_remotes", ()))
    if not hosts and not remotes:
        return None
    if not isinstance(bt, dict):
        return "this config has Bluetooth devices or remotes, but the hub has no Bluetooth support (update its firmware)"
    hmax, rmax = bt.get("hosts_max"), bt.get("remotes_max")
    if isinstance(hmax, int) and hosts > hmax:
        return f"too many Bluetooth devices: {hosts} (the hub allows at most {hmax})"
    if isinstance(rmax, int) and remotes > rmax:
        return f"too many Bluetooth remotes: {remotes} (the hub allows at most {rmax})"
    return None


def estimate_bytes(
    lim: HubLimits,
    *,
    devices: int,
    commands: int,
    inputs: int,
    keys: int,
    words: int,
    bt_hosts: int = 0,
    bt_remotes: int = 0,
    **_ignored: int,
) -> int:
    """What the hub needs to load a config (hub/engine/mapping.c est_full); a budget compares to this."""
    return (
        lim.slack_b
        + (devices + bt_hosts + bt_remotes) * lim.dev_b  # a bt table entry is budgeted as a device
        + (commands + inputs) * lim.cmd_b
        + keys * lim.key_b
        + words * lim.word_b
    )
