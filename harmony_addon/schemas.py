"""Request schemas (voluptuous). Unknown keys are rejected; sizes and counts are capped."""

from __future__ import annotations

import voluptuous as vol

from .store import HUB_ID

MAX_NODES = 60_000  # JSON values in one config document
MAX_DEPTH = 16
MAX_KEYS_TOP = 16
MAX_STR = 100_000
YAML_MAX = 1_000_000


def bounded_config(v: object) -> dict:
    """A config document: a JSON object with capped depth, node count and string length."""
    if not isinstance(v, dict):
        raise vol.Invalid("must be an object")
    if len(v) > MAX_KEYS_TOP:
        raise vol.Invalid("too many top-level keys")
    nodes = 0
    stack: list[tuple[object, int]] = [(v, 1)]
    while stack:
        cur, depth = stack.pop()
        nodes += 1
        if nodes > MAX_NODES:
            raise vol.Invalid("document too large")
        if depth > MAX_DEPTH:
            raise vol.Invalid("document nested too deeply")
        if isinstance(cur, dict):
            for k, x in cur.items():
                if not isinstance(k, str) or len(k) > 200:
                    raise vol.Invalid("keys must be short strings")
                stack.append((x, depth + 1))
        elif isinstance(cur, list):
            stack.extend((x, depth + 1) for x in cur)
        elif isinstance(cur, str):
            if len(cur) > MAX_STR:
                raise vol.Invalid("string too long")
        elif cur is not None and not isinstance(cur, (bool, int, float)):
            raise vol.Invalid("unsupported value")
    return v


def _hub(v: object) -> str:
    if not isinstance(v, str) or not HUB_ID.fullmatch(v):
        raise vol.Invalid("invalid hub id")
    return v


QUERY = vol.Schema({vol.Optional("hub", default="default"): _hub}, extra=vol.PREVENT_EXTRA)
WAIT_QUERY = vol.Schema(
    {
        vol.Optional("hub", default="default"): _hub,
        vol.Optional("rev", default=""): vol.All(str, vol.Length(max=64)),
    },
    extra=vol.PREVENT_EXTRA,
)
GET_CONFIG_QUERY = vol.Schema(
    {
        vol.Optional("hub", default="default"): _hub,
        vol.Optional("full", default="0"): vol.In(["0", "1"]),
    },
    extra=vol.PREVENT_EXTRA,
)
EXPORT_QUERY = vol.Schema(
    {
        vol.Optional("hub", default="default"): _hub,
        vol.Optional("source"): vol.In(["draft", "applied"]),
    },
    extra=vol.PREVENT_EXTRA,
)

PUT_DRAFT = vol.Schema({vol.Required("config"): bounded_config}, extra=vol.PREVENT_EXTRA)
OPT_CONFIG = vol.Schema({vol.Optional("config"): bounded_config}, extra=vol.PREVENT_EXTRA)
EMPTY = vol.Schema({}, extra=vol.PREVENT_EXTRA)
REVERT = vol.Schema(
    {vol.Required("target"): vol.In(["applied", "previous"])}, extra=vol.PREVENT_EXTRA
)
