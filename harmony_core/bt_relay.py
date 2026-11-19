"""Bluetooth relay validation shared by the integration and the add-on (docs/addon-api.md "Bluetooth relay").

Nothing here trusts its input: the hub's `bt_state` (nearby devices pick their own names), the integration's report
to the add-on, and the editor's commands all pass through these cleaners, bounded in count and length.
"""

from __future__ import annotations

import re
from typing import Any

ADDR = re.compile(r"^[0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5}$")
STATES = ("off", "starting", "ready", "no_adapter", "disabled")
MAX_HOSTS, MAX_REMOTES, MAX_CANDIDATES = 255, 32, 16
NAME_MAX = 64
SECS_MAX = 300
SECS_DEFAULT = 120
PAGES = (7, 12)
OPS = ("pair", "pair_stop", "pick", "forget", "key")
MAX_COMMANDS = 8  # per exchange, and per hub in the add-on's queue


class RelayError(ValueError):
    """A refused value: `code` is a short machine word, the message is for people."""

    def __init__(self, code: str, msg: str) -> None:
        super().__init__(msg)
        self.code, self.msg = code, msg


def norm_addr(v: Any) -> str | None:
    """`aa:bb:cc:dd:ee:ff` -> `AA:BB:CC:DD:EE:FF`, or None when it is not an address."""
    return v.upper() if isinstance(v, str) and ADDR.fullmatch(v) else None


def addr_key(addr: str) -> str:
    """The form used in entity unique ids: 12 lower-case hex digits."""
    return addr.replace(":", "").lower()


def _int(v: Any, lo: int, hi: int) -> int | None:
    return v if type(v) is int and lo <= v <= hi else None


def _name(v: Any) -> str | None:
    if not isinstance(v, str) or not v or any(ord(c) < 0x20 or ord(c) == 0x7F for c in v):
        return None
    return v[:NAME_MAX]


def _entry(raw: Any, slot_max: int, hosts: bool) -> dict[str, Any] | None:
    if not isinstance(raw, dict):
        return None
    slot, addr = _int(raw.get("slot"), 0, slot_max), norm_addr(raw.get("addr"))
    if slot is None or addr is None:
        return None
    out: dict[str, Any] = {"slot": slot, "addr": addr, "connected": raw.get("connected") is True}
    if (name := _name(raw.get("name"))) is not None:
        out["name"] = name
    if hosts:
        out["transport"] = raw["transport"] if raw.get("transport") in ("classic", "le") else "classic"
    elif (bat := _int(raw.get("battery"), 0, 100)) is not None:
        out["battery"] = bat
    if isinstance(raw.get("configured"), bool):
        out["configured"] = raw["configured"]
    return out


def clean_state(raw: Any) -> dict[str, Any] | None:
    """A `bt_state` as the hub sent it -> the same shape with every field checked and capped, or None
    when it is not a usable snapshot. Unknown fields are dropped."""
    if not isinstance(raw, dict) or raw.get("state") not in STATES:
        return None
    out: dict[str, Any] = {"state": raw["state"], "discoverable": raw.get("discoverable") is True}
    if (v := _int(raw.get("discoverable_secs"), 0, 65535)) is not None:
        out["discoverable_secs"] = v
    if isinstance(raw.get("le"), bool):
        out["le"] = raw["le"]
    if (v := _int(raw.get("gen"), 0, 0xFFFFFFFF)) is not None:
        out["gen"] = v
    if raw.get("window") in ("host", "remote"):
        out["window"] = raw["window"]
    pr = raw.get("pairing")
    if isinstance(pr, dict) and pr.get("result") in ("ok", "failed", "refused"):
        p: dict[str, Any] = {"result": pr["result"]}
        if (a := norm_addr(pr.get("addr"))) is not None:
            p["addr"] = a
        if (n := _name(pr.get("name"))) is not None:
            p["name"] = n
        if pr.get("transport") in ("classic", "le"):
            p["transport"] = pr["transport"]
        if pr.get("kind") in ("host", "remote"):
            p["kind"] = pr["kind"]
        out["pairing"] = p
    for key, cap, smax, hosts in (("hosts", MAX_HOSTS, 254, True), ("remotes", MAX_REMOTES, 31, False)):
        rows = raw.get(key)
        rows = rows if isinstance(rows, list) else []
        out[key] = [e for r in rows[:cap] if (e := _entry(r, smax, hosts)) is not None]
    cands: list[dict[str, Any]] = []
    for c in raw["candidates"][:MAX_CANDIDATES] if isinstance(raw.get("candidates"), list) else []:
        if not isinstance(c, dict) or (a := norm_addr(c.get("addr"))) is None:
            continue
        rssi = _int(c.get("rssi"), -128, 127)
        row: dict[str, Any] = {"addr": a, "rssi": -127 if rssi is None else rssi}
        if (n := _name(c.get("name"))) is not None:
            row["name"] = n
        cands.append(row)
    if cands:
        out["candidates"] = cands
    return out


def clean_limits(raw: Any) -> dict[str, Any] | None:
    """`hello_ack.bt` -> {hosts_max, conn_max, remotes_max, le_bonds_max, pages, le} with sane ints, or None."""
    if not isinstance(raw, dict):
        return None
    out: dict[str, Any] = {}
    for k, hi, absent in (
        ("hosts_max", 255, MAX_HOSTS),  # not reported: the protocol's maximum; reported but invalid: none
        ("conn_max", 255, 0),
        ("remotes_max", 32, MAX_REMOTES),
        ("le_bonds_max", 255, 0),
    ):
        out[k] = absent if k not in raw else (_int(raw[k], 0, hi) or 0)
    pages = raw.get("pages")
    out["pages"] = sorted({p for p in pages if p in PAGES}) if isinstance(pages, list) else list(PAGES)
    out["le"] = raw.get("le") is True
    return out


def newer_gen(new: int, old: int) -> bool:
    """`new` is the same generation or a later one (u32 serial arithmetic: a wrap still counts as later)."""
    return ((new - old) & 0xFFFFFFFF) < 0x80000000


def check_command(raw: Any, limits: dict[str, Any] | None = None) -> dict[str, Any]:
    """Validate one relay command `{op, ...}` and return it normalised (addresses upper-case, defaults filled).
    `limits` (the hub's `hello_ack.bt`, cleaned) tightens the slot, page and LE checks; the add-on has none."""
    if not isinstance(raw, dict):
        raise RelayError("bad_request", "a command is an object")
    op = raw.get("op")
    if op not in OPS:
        raise RelayError("bad_op", "unknown command")
    extra = set(raw) - {"id", "op"}
    lim = limits or {}
    hosts_max = lim.get("hosts_max", MAX_HOSTS)
    remotes_max = lim.get("remotes_max", MAX_REMOTES)
    out: dict[str, Any] = {"op": op}
    if "id" in raw:
        out["id"] = raw["id"]

    def only(*names: str) -> None:
        if extra - set(names):
            raise RelayError("bad_request", "unexpected field")

    if op == "pair":
        only("mode", "transport", "secs")
        mode = raw.get("mode", "host")
        transport = raw.get("transport", "any")
        secs = raw.get("secs", SECS_DEFAULT)
        if mode not in ("host", "remote"):
            raise RelayError("bad_mode", "mode is host or remote")
        if transport not in ("any", "classic", "le"):
            raise RelayError("bad_transport", "transport is any, classic or le")
        if _int(secs, 1, SECS_MAX) is None:
            raise RelayError("bad_secs", f"seconds are 1 to {SECS_MAX}")
        if mode == "remote" and lim and remotes_max == 0:
            raise RelayError("unsupported", "this hub takes no Bluetooth remotes")
        if transport == "le" and lim and not lim.get("le"):
            raise RelayError("no_le", "this hub has no Bluetooth LE")
        out |= {"mode": mode, "transport": transport, "secs": secs}
    elif op == "pair_stop":
        only()
    elif op == "pick":
        only("addr")
        addr = norm_addr(raw.get("addr"))
        if addr is None:
            raise RelayError("bad_addr", "an address looks like AA:BB:CC:DD:EE:FF")
        out["addr"] = addr
    elif op == "forget":
        only("kind", "slot")
        kind = raw.get("kind", "host")
        if kind not in ("host", "remote"):
            raise RelayError("bad_kind", "kind is host or remote")
        cap = hosts_max if kind == "host" else remotes_max
        slot = _int(raw.get("slot"), 0, 254 if kind == "host" else 31)
        if slot is None or slot >= max(cap, 0) and lim:
            raise RelayError("bad_slot", "no such slot")
        out |= {"kind": kind, "slot": slot}
    else:  # key
        only("slot", "page", "usage", "action")
        slot = _int(raw.get("slot"), 0, 254)
        if slot is None or lim and slot >= hosts_max:
            raise RelayError("bad_slot", "no such slot")
        pages = lim.get("pages", PAGES) if lim else PAGES
        if raw.get("page") not in pages or type(raw.get("page")) is not int:
            raise RelayError("bad_page", "page is 7 (keyboard) or 12 (consumer)")
        if _int(raw.get("usage"), 0, 0xFFFF) is None:
            raise RelayError("bad_usage", "usage is 0 to 65535")
        if raw.get("action") not in ("down", "up", "press"):
            raise RelayError("bad_action", "action is down, up or press")
        out |= {k: raw[k] for k in ("slot", "page", "usage", "action")}
    return out
