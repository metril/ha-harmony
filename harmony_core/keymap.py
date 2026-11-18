"""Which action a hard key performs on each layer (the hub's lookup, stated once).

The hub looks a key up in the current activity's `keys`, then in the top-level `keys`; no match
means the press is only forwarded to Home Assistant (docs/proto/config-format-2.md, "Key maps").
The editor's X-ray view calls this instead of re-implementing that rule.
"""

from __future__ import annotations

from typing import Any

from .config_model import Model
from .keys import KEY_CODES, key_code


def summarize(action: dict[str, Any]) -> dict[str, Any]:
    """A short, UI-ready description of a validated action."""
    kind = action["type"]
    if kind == "device":
        what = action.get("command") or action.get("input")
        verb = "command" if "command" in action else "input"
        return {
            "kind": "device",
            "device": action["device"],
            verb: what,
            "label": f"{action['device']}: {what}"
            if verb == "command"
            else f"{action['device']} -> {what}",
        }
    if kind == "activity":
        return {
            "kind": "activity",
            "activity": action["activity"],
            "label": f"Start {action['activity']}" if action["activity"] != "off" else "Power off",
        }
    if kind == "ha":
        return {"kind": "ha", "event": action["event"], "label": f"HA event {action['event']}"}
    if kind == "bt":
        return {
            "kind": "bt",
            "usage": action["usage"],
            "label": f"Bluetooth 0x{action['usage']:04X}",
        }
    if kind == "none":
        return {"kind": "none", "label": "Does nothing"}
    form = "proto" if "proto" in action else "pronto" if "pronto" in action else "raw"
    return {"kind": "ir", "form": form, "label": f"IR {form}"}


def _by_code(keys: dict[str, Any]) -> dict[int, dict[str, Any]]:
    out: dict[int, dict[str, Any]] = {}
    for name, action in keys.items():
        code = key_code(name)
        if code is not None:
            out[code] = action
    return out


def _layer(own: dict[int, dict[str, Any]], default: dict[int, dict[str, Any]] | None) -> list:
    rows = []
    for name, code in sorted(KEY_CODES.items(), key=lambda kv: kv[1]):
        if code in own:
            src, act = ("default" if default is None else "activity"), own[code]
        elif default is not None and code in default:
            src, act = "default", default[code]
        else:
            src, act = "none", None
        rows.append(
            {
                "key": name,
                "code": code,
                "source": src,
                "action": None if act is None else summarize(act),
            }
        )
    return rows


def resolve_keymaps(model: Model) -> dict[str, Any]:
    """`{default: [row...], activities: [{id, name, keys: [row...]}]}`; one row per hard key.
    `source` is "activity" (own entry), "default" (inherited / the default layer) or "none"."""
    default = _by_code(model["keys"])
    return {
        "default": _layer(default, None),
        "activities": [
            {
                "id": a["id"],
                "name": a.get("name", a["id"]),
                "keys": _layer(_by_code(a["keys"]), default),
            }
            for a in model["activities"]
        ],
    }
