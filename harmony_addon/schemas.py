"""Request schemas (voluptuous). Unknown keys are rejected; sizes and counts are capped."""

from __future__ import annotations

import voluptuous as vol

from .ha_client import ACTION_DOMAINS, ENTITY_DOMAINS
from .store import HUB_ID

MAX_NODES = 250_000  # JSON values in one config document (a hub takes ~13k protocol-coded commands, ~4 values each)
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
VALIDATE_QUERY = vol.Schema(
    {
        vol.Optional("hub", default="default"): _hub,
        vol.Optional("all", default="1"): vol.In(["0", "1"]),  # 0: first error only
    },
    extra=vol.PREVENT_EXTRA,
)
EXPORT_QUERY = vol.Schema(
    {vol.Optional("source", default="draft"): vol.All(str, vol.Match(r"^(draft|rev:[0-9]{1,12})$"))},
    extra=vol.PREVENT_EXTRA,
)

PUT_DRAFT = vol.Schema({vol.Required("config"): bounded_config}, extra=vol.PREVENT_EXTRA)
NEED_CONFIG = vol.Schema({vol.Required("config"): bounded_config}, extra=vol.PREVENT_EXTRA)
EMPTY = vol.Schema({}, extra=vol.PREVENT_EXTRA)


def _name(v: object) -> str:
    if not isinstance(v, str) or not v.strip() or len(v) > 200 or any(ord(c) < 32 for c in v):
        raise vol.Invalid("invalid name")
    return v


_INT = vol.All(int, vol.Range(min=1, max=10**9))
CREATE_CONFIG = vol.Schema(
    {
        vol.Required("name"): _name,
        vol.Exclusive("from", "src"): vol.Schema(
            {vol.Required("id"): _hub, vol.Optional("rev"): _INT}, extra=vol.PREVENT_EXTRA
        ),
        vol.Exclusive("yaml", "src"): vol.All(str, vol.Length(max=YAML_MAX)),
    },
    extra=vol.PREVENT_EXTRA,
)
RENAME = vol.Schema({vol.Required("name"): _name}, extra=vol.PREVENT_EXTRA)
SAVE_REV = vol.Schema(
    {vol.Optional("note", default=""): vol.All(str, vol.Length(max=200))}, extra=vol.PREVENT_EXTRA
)
RESTORE = vol.Schema({vol.Required("rev"): _INT}, extra=vol.PREVENT_EXTRA)
APPLY = vol.Schema(
    {vol.Required("config_id"): _hub, vol.Optional("rev"): _INT}, extra=vol.PREVENT_EXTRA
)

_Q = vol.All(str, vol.Length(max=64))
HA_ENTITIES_QUERY = vol.Schema(
    {
        vol.Optional("domain"): vol.In(sorted(ENTITY_DOMAINS)),
        vol.Optional("q", default=""): _Q,
        vol.Optional("area"): _Q,
        vol.Optional("diagnostic", default="0"): vol.In(["0", "1"]),
        vol.Optional("state", default="0"): vol.In(["0", "1"]),
    },
    extra=vol.PREVENT_EXTRA,
)
HA_SERVICES_QUERY = vol.Schema(
    {vol.Optional("domain"): vol.In(sorted(ACTION_DOMAINS))}, extra=vol.PREVENT_EXTRA
)

_REV = vol.All(str, vol.Match(r"^[A-Za-z0-9._-]{1,64}$"))
ACK = vol.Schema(
    {
        vol.Required("rev"): _REV,
        vol.Required("result"): vol.In(["ok", "error"]),
        vol.Optional("detail"): vol.All(str, vol.Length(max=200)),
    },
    extra=vol.ALLOW_EXTRA,  # a channel frame: unknown fields are ignored
)

TEST_IR = vol.Schema(
    {
        vol.Required("hub"): _hub,
        vol.Required("ir"): bounded_config,
        vol.Optional("port_mask"): vol.All(int, vol.Range(min=1, max=255)),
    },
    extra=vol.PREVENT_EXTRA,
)

ADOPT = vol.Schema(
    {vol.Optional("from", default="default"): _hub, vol.Required("to"): _hub},
    extra=vol.PREVENT_EXTRA,
)
