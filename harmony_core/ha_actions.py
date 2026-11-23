"""Home Assistant actions: `{ha: {service, target, data, repeat}}` (validation, defaults, runtime form).

Pure, no Home Assistant imports. The add-on validates, the integration validates again (it never
trusts a bundle) and checks `allowed()` once more when it dispatches. No templates, ever.
"""

from __future__ import annotations

import json
import math
import re
from dataclasses import dataclass, field
from typing import Any

import voluptuous as vol

ALLOWED_DOMAINS = frozenset(
    {
        "light",
        "switch",
        "scene",
        "script",
        "media_player",
        "remote",
        "fan",
        "cover",
        "climate",
        "input_boolean",
        "input_button",
        "button",
        "number",
        "select",
        "input_select",
    }
)
# Per-domain service restriction (domains not listed allow any service).
DOMAIN_SERVICES: dict[str, frozenset[str]] = {
    "input_select": frozenset({"select_option", "select_next", "select_previous"}),
}
SERVICE_RE = re.compile(r"^[a-z_]+\.[a-z0-9_]+$")
ENTITY_RE = re.compile(r"^[a-z0-9_]+\.[a-z0-9_]+$")
MAX_ENTITIES = 8
DATA_MAX_BYTES = 1024
DATA_MAX_DEPTH = 4
DATA_MAX_NODES = 64
DATA_MAX_STR = 200
_TEMPLATE_MARKS = ("{{", "{%", "{#")
_TARGET_KEYS = frozenset({"entity_id", "device_id", "area_id", "floor_id", "label_id"})

# What a tile tap does by default, per entity domain (`homeassistant.toggle` is not allowed).
DEFAULT_TAP: dict[str, str] = {
    "light": "light.toggle",
    "switch": "switch.toggle",
    "fan": "fan.toggle",
    "cover": "cover.toggle",
    "input_boolean": "input_boolean.toggle",
    "media_player": "media_player.toggle",
    "remote": "remote.toggle",
    "scene": "scene.turn_on",
    "script": "script.turn_on",
    "button": "button.press",
    "input_button": "input_button.press",
}


def allowed(service: str) -> bool:
    """True for a well-formed `domain.service` whose domain is on the allow-list."""
    if not SERVICE_RE.match(service):
        return False
    dom, name = service.split(".", 1)
    return dom in ALLOWED_DOMAINS and name in DOMAIN_SERVICES.get(dom, {name})


@dataclass(frozen=True)
class HaCall:
    """One validated service call (what the integration dispatches)."""

    service: str
    entities: tuple[str, ...]
    data: dict[str, Any] = field(default_factory=dict, compare=True, hash=False)
    repeat: bool = False

    @property
    def domain(self) -> str:
        return self.service.split(".", 1)[0]

    @property
    def name(self) -> str:
        return self.service.split(".", 1)[1]

    def to_model(self) -> dict[str, Any]:
        out: dict[str, Any] = {
            "type": "ha_call",
            "service": self.service,
            "target": {"entity_id": list(self.entities)},
            "data": dict(self.data),
            "repeat": self.repeat,
        }
        return out


def call_of(action: dict[str, Any]) -> HaCall:
    """The runtime form of a model action of type `ha_call`."""
    return HaCall(
        action["service"],
        tuple(action["target"]["entity_id"]),
        action.get("data", {}),
        bool(action.get("repeat", False)),
    )


def _no_template(s: str, what: str) -> None:
    if any(m in s for m in _TEMPLATE_MARKS):
        raise vol.Invalid(f"{what} must not contain a template ({{{{ }}}}, {{% %}} or {{# #}})")


def _check_data(data: Any) -> dict[str, Any]:
    if not isinstance(data, dict):
        raise vol.Invalid("data must be a mapping")
    nodes = 0
    stack: list[tuple[Any, int]] = [(data, 1)]
    while stack:
        cur, depth = stack.pop()
        nodes += 1
        if nodes > DATA_MAX_NODES:
            raise vol.Invalid(f"data has more than {DATA_MAX_NODES} values")
        if depth > DATA_MAX_DEPTH:
            raise vol.Invalid(f"data is nested deeper than {DATA_MAX_DEPTH}")
        if isinstance(cur, dict):
            for k, x in cur.items():
                if not isinstance(k, str) or not k or len(k) > 64:
                    raise vol.Invalid("data keys must be short text")
                _no_template(k, "a data key")
                if k in _TARGET_KEYS:
                    raise vol.Invalid(f"put {k!r} in `target`, not in `data`")
                stack.append((x, depth + 1))
        elif isinstance(cur, list):
            stack.extend((x, depth + 1) for x in cur)
        elif isinstance(cur, str):
            if len(cur) > DATA_MAX_STR:
                raise vol.Invalid(f"a data string is over {DATA_MAX_STR} characters")
            _no_template(cur, "data")
        elif isinstance(cur, float):
            if not math.isfinite(cur):
                raise vol.Invalid("data numbers must be finite")
        elif cur is not None and not isinstance(cur, bool | int):
            raise vol.Invalid("data must be plain JSON values")
    try:
        size = len(json.dumps(data, allow_nan=False))
    except TypeError, ValueError:
        raise vol.Invalid("data must be plain JSON values") from None
    if size > DATA_MAX_BYTES:
        raise vol.Invalid(f"data is over {DATA_MAX_BYTES} bytes")
    return data


def _entity(v: Any) -> str:
    if not isinstance(v, str) or not ENTITY_RE.match(v) or len(v) > 100:
        raise vol.Invalid(f"{v!r} is not an entity id")
    return v


def validate_ha(v: Any) -> dict[str, Any]:
    """The body of an `ha:` action -> model action `{"type": "ha_call", ...}`."""
    if not isinstance(v, dict):
        raise vol.Invalid("`ha` must be a mapping with `service`")
    extra = set(v) - {"service", "target", "data", "repeat"}
    if extra:
        raise vol.Invalid(f"unexpected {sorted(extra)} in an ha action")
    svc = v.get("service")
    if not isinstance(svc, str) or not SERVICE_RE.match(svc):
        raise vol.Invalid("service must look like `domain.service` (a-z, 0-9, _)")
    _no_template(svc, "service")
    if svc.split(".", 1)[0] not in ALLOWED_DOMAINS:
        raise vol.Invalid(
            f"service domain {svc.split('.', 1)[0]!r} is not allowed; use one of "
            f"{', '.join(sorted(ALLOWED_DOMAINS))}"
        )
    dom, name = svc.split(".", 1)
    if name not in DOMAIN_SERVICES.get(dom, {name}):
        raise vol.Invalid(f"{dom} allows only {', '.join(sorted(DOMAIN_SERVICES[dom]))}")
    target = v.get("target")
    if not isinstance(target, dict) or set(target) != {"entity_id"}:
        raise vol.Invalid("target must be `{entity_id: <id or list of ids>}`")
    ents = target["entity_id"]
    ents = [ents] if isinstance(ents, str) else ents
    if not isinstance(ents, list) or not 1 <= len(ents) <= MAX_ENTITIES:
        raise vol.Invalid(f"target.entity_id takes 1..{MAX_ENTITIES} entity ids")
    ents = [_entity(e) for e in ents]
    data = _check_data(v["data"]) if "data" in v else {}
    rep = v.get("repeat", False)
    if not isinstance(rep, bool):
        raise vol.Invalid("repeat must be true or false")
    return {
        "type": "ha_call",
        "service": svc,
        "target": {"entity_id": ents},
        "data": data,
        "repeat": rep,
    }


def default_tap(entity_id: str) -> dict[str, Any] | None:
    """The tap action a tile gets when the user does not say: toggle / activate / press."""
    svc = DEFAULT_TAP.get(entity_id.split(".", 1)[0])
    if svc is None:
        return None
    return validate_ha({"service": svc, "target": {"entity_id": entity_id}})


def summary(action: dict[str, Any]) -> dict[str, Any]:
    ents = action["target"]["entity_id"]
    entity = ents[0] if len(ents) == 1 else f"{ents[0]} +{len(ents) - 1}"
    return {
        "kind": "ha",
        "service": action["service"],
        "entity": entity,
        "label": f"{action['service']} {entity}" + (" (repeats)" if action.get("repeat") else ""),
    }
