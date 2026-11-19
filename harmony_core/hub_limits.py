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
    ("text_copies", 1, 16),
    ("node_b", 16, 1024),
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
    budget: int = 0  # bytes one config may need at its peak (0 = legacy hub)
    chunk: int = 0  # config_chunk data bytes the hub takes (0 = no chunked transfer)
    text_copies: int = 3
    node_b: int = 80
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
            v = raw.get(key)
            if type(v) is not int or not lo <= v <= hi:
                return LEGACY
            vals[key] = v
        return cls(**vals)

    def to_wire(self) -> dict[str, int]:
        """The `limits` object (a hello_ack as sent; also what the add-on is told about a hub)."""
        if self.legacy:
            return {}
        return {key: getattr(self, key) for key, _lo, _hi in _BOUNDS}

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


def estimate_bytes(
    lim: HubLimits,
    *,
    text: int,
    nodes: int,
    devices: int,
    commands: int,
    inputs: int,
    keys: int,
    words: int,
) -> int:
    """What the hub needs to load a config (hub/engine/mapping.c est_full); a budget compares to this."""
    return (
        lim.text_copies * text
        + nodes * lim.node_b
        + devices * lim.dev_b
        + (commands + inputs) * lim.cmd_b
        + keys * lim.key_b
        + words * lim.word_b
    )
