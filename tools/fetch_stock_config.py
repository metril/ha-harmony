#!/usr/bin/env python3
"""Read-only dump of a stock Harmony Hub's configuration over its local WebSocket API.

Only `get`-style queries are issued (see QUERIES). Nothing is written to the hub and no cloud
endpoint is contacted. Raw replies are saved verbatim under <out-dir>/raw/, plus normalised
devices.json, activities.json, ir_codes.json and manifest.json.
"""

import argparse
import asyncio
import json
import re
import sys
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

CFG = "vnd.logitech.harmony/vnd.logitech.harmony.engine?config"
# Strictly read-only queries: (command, params).
QUERIES: tuple[tuple[str, dict[str, Any]], ...] = (
    ("setup.account?getProvisionInfo", {}),
    (CFG, {"verb": "get", "format": "json"}),
    ("vnd.logitech.harmony/vnd.logitech.harmony.engine?getCurrentActivity", {}),
    ("vnd.logitech.connect/vnd.logitech.statedigest?get", {"verb": "get", "format": "json"}),
)
ORIGIN = "http://localhost.nebula.myharmony.com"
KNOWN_ENCODINGS = {"IRCommand"}


def safe_name(cmd: str) -> str:
    return re.sub(r"[^A-Za-z0-9._-]+", "_", cmd).strip("_")


def decode_reply(raw: str | bytes) -> Any:
    return json.loads(raw)


def reply_data(reply: Any) -> Any:
    """The payload of a hub reply (`data`), or the reply itself if it has none."""
    if isinstance(reply, dict) and "data" in reply:
        return reply["data"]
    return reply


def parse_action(action: Any) -> tuple[dict[str, Any] | None, str | None]:
    """A function's `action` is a JSON string; return (parsed, warning)."""
    if isinstance(action, dict):
        return action, None
    if not isinstance(action, str):
        return None, f"action is {type(action).__name__}, not a JSON string"
    try:
        parsed = json.loads(action)
    except ValueError:
        return None, "action is not valid JSON"
    if not isinstance(parsed, dict):
        return None, "action JSON is not an object"
    return parsed, None


def device_commands(device: dict[str, Any], warnings: list[dict[str, Any]]) -> dict[str, Any]:
    """command name -> {type, payload (the hub's own action), label, group}. Never drops one."""
    dev_id = str(device.get("id"))
    out: dict[str, Any] = {}
    for group in device.get("controlGroup") or []:
        gname = group.get("name")
        for fn in group.get("function") or []:
            name = fn.get("name")
            action = fn.get("action")
            parsed, warn = parse_action(action)
            ctype = parsed.get("type") if parsed else None
            entry = {
                "type": ctype,
                "payload": parsed if parsed is not None else action,
                "label": fn.get("label"),
                "group": gname,
            }
            if warn:
                warnings.append({"device": dev_id, "command": name, "warning": warn})
            elif ctype not in KNOWN_ENCODINGS:
                warnings.append(
                    {
                        "device": dev_id,
                        "command": name,
                        "warning": f"unrecognised encoding {ctype!r}; kept raw",
                    }
                )
            key = str(name)
            if key in out:  # same command in two groups: keep both
                warnings.append(
                    {
                        "device": dev_id,
                        "command": name,
                        "warning": "duplicate command name; kept with group suffix",
                    }
                )
                key = f"{key}@{gname}"
            out[key] = entry
    return out


def normalise(config: Any) -> tuple[list[dict], list[dict], dict[str, Any], list[dict]]:
    """config data -> (devices, activities, ir_codes, warnings)."""
    warnings: list[dict[str, Any]] = []
    cfg = config if isinstance(config, dict) else {}
    devices, ir_codes = [], {}
    for dev in cfg.get("device") or []:
        cmds = device_commands(dev, warnings)
        ir_codes[str(dev.get("id"))] = cmds
        devices.append(
            {
                "id": str(dev.get("id")),
                "label": dev.get("label"),
                "type": dev.get("type"),
                "manufacturer": dev.get("manufacturer"),
                "model": dev.get("model"),
                "controlPort": dev.get("controlPort"),
                "isManualPower": dev.get("isManualPower"),
                "command_count": len(cmds),
            }
        )
    activities = []
    for act in cfg.get("activity") or []:
        activities.append(
            {
                "id": str(act.get("id")),
                "label": act.get("label"),
                "type": act.get("type"),
                "isAVActivity": act.get("isAVActivity"),
                "roles": act.get("roles"),
                "fixit": act.get("fixit"),
                "sequences": act.get("sequences"),
                "controlGroup": act.get("controlGroup"),
                "rules": act.get("rules"),
                "suggestedDisplay": act.get("suggestedDisplay"),
            }
        )
    return devices, activities, ir_codes, warnings


def write_json(path: Path, obj: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(obj, indent=2, ensure_ascii=False) + "\n")


def build_message(hub_id: str, cmd: str, params: dict[str, Any], msg_id: int, timeout: int) -> dict:
    return {
        "hubId": hub_id,
        "timeout": timeout,
        "hbus": {"cmd": cmd, "id": str(msg_id), "params": params},
    }


def write_outputs(
    out: Path, host: str, hub_id: str, replies: dict[str, Any], statuses: dict[str, dict[str, Any]]
) -> dict[str, Any]:
    for cmd, reply in replies.items():
        write_json(out / "raw" / f"{safe_name(cmd)}.json", reply)
    warnings: list[dict[str, Any]] = []
    if CFG in replies:
        devices, activities, ir, warnings = normalise(reply_data(replies[CFG]))
        write_json(out / "devices.json", devices)
        write_json(out / "activities.json", activities)
        write_json(out / "ir_codes.json", {"warnings": warnings, "devices": ir})
    digest = reply_data(replies.get("vnd.logitech.connect/vnd.logitech.statedigest?get")) or {}
    firmware = digest.get("hubSwVersion") if isinstance(digest, dict) else None
    manifest = {
        "host": host,
        "hub_id": hub_id,
        "firmware": firmware,
        "fetched_utc": datetime.now(UTC).isoformat(timespec="seconds"),
        "commands": [{"cmd": c, **s} for c, s in statuses.items()],
        "warnings": len(warnings),
    }
    write_json(out / "manifest.json", manifest)
    return manifest


async def fetch(host: str, hub_id: str, timeout: float) -> tuple[dict, dict]:
    import aiohttp

    replies: dict[str, Any] = {}
    statuses: dict[str, dict[str, Any]] = {}
    url = f"ws://{host}:8088/?domain=svcs.myharmony.com&hubId={hub_id}"
    session = aiohttp.ClientSession(timeout=aiohttp.ClientTimeout(total=None))
    async with session as sess, sess.ws_connect(url, origin=ORIGIN, timeout=timeout) as ws:
        for i, (cmd, params) in enumerate(QUERIES, 1):
            await ws.send_str(json.dumps(build_message(hub_id, cmd, params, i, int(timeout))))
            try:
                while True:
                    msg = await asyncio.wait_for(ws.receive(), timeout)
                    if msg.type != aiohttp.WSMsgType.TEXT:
                        raise RuntimeError(f"ws message type {msg.type.name}")
                    reply = decode_reply(msg.data)
                    # skip unsolicited notifications (they carry no matching id)
                    if isinstance(reply, dict) and reply.get("id") not in (str(i), i, None):
                        continue
                    if isinstance(reply, dict) and reply.get("code") == 100:  # in progress
                        continue
                    break
                replies[cmd] = reply
                code = reply.get("code") if isinstance(reply, dict) else None
                statuses[cmd] = {
                    "status": "ok" if str(code) in ("200", "None") else "error",
                    "code": code,
                }
            except (TimeoutError, RuntimeError, ValueError) as e:
                statuses[cmd] = {"status": "failed", "error": f"{type(e).__name__}: {e}"}
    return replies, statuses


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", required=True)
    ap.add_argument("--hub-id", required=True)
    ap.add_argument("--out-dir", required=True, type=Path)
    ap.add_argument("--timeout", type=float, default=20.0)
    a = ap.parse_args(argv)
    replies, statuses = asyncio.run(fetch(a.host, a.hub_id, a.timeout))
    m = write_outputs(a.out_dir, a.host, a.hub_id, replies, statuses)
    for c in m["commands"]:
        print(f"{c['status']:7} {c.get('code', c.get('error'))}  {c['cmd']}", file=sys.stderr)
    return 0 if CFG in replies else 1


if __name__ == "__main__":
    sys.exit(main())
