"""The hub daemon's Bluetooth link end to end over the websocket: `hub --sim --bt-helper <fake>` supervises the test-only
fake helper (tests/c/bt_fake_helper.c, speaks the real bt_ipc wire format, no Bluetooth). Remote pairing flow:
bt_pair remote -> candidates -> pick one -> pairing ok -> bt_state, plus config binding (`configured`) and helper death."""

import asyncio
import json
import os
import signal
import subprocess
import time

import aiohttp
import pytest
from conftest import TOKEN

CONFIG = {
    "format": 2,
    "keys": {"1": {"type": "bt", "usage": 233, "host": 3}},
    "bt_hosts": [{"slot": 3, "addr": "AA:BB:CC:DD:EE:01", "name": "Shield", "transport": "le"}],
    "bt_remotes": [{"slot": 2, "addr": "11:22:33:44:55:01", "name": "R", "keys": [[12, 233, 1]]}],
}


async def send(ws, t, d, id):
    await ws.send_str(json.dumps({"v": 1, "id": id, "t": t, "d": d}))


async def until(ws, pred, timeout=8):
    """The next message satisfying pred (everything else is skipped)."""
    end = time.monotonic() + timeout
    while True:
        msg = json.loads((await ws.receive(timeout=max(0.1, end - time.monotonic()))).data)
        if pred(msg):
            return msg


def bt_state(extra=lambda d: True):
    return lambda m: m["t"] == "bt_state" and extra(m["d"])


async def open_session(hub):
    s = aiohttp.ClientSession()
    ws = await s.ws_connect(hub.url)
    await send(ws, "hello", {"token": TOKEN}, 1)
    ack = await until(ws, lambda m: m["t"] == "hello_ack")
    return s, ws, ack


@pytest.fixture
def bt_hub(start_hub, built_binaries, tmp_path, monkeypatch):
    monkeypatch.setenv("BT_FAKE_MODE", "normal")
    monkeypatch.setenv("BT_FAKE_LOG", str(tmp_path / "helper.log"))
    return start_hub(
        "--bt-helper",
        str(built_binaries / "tests" / "c" / "bt_fake_helper"),
        "--bt-dir",
        str(tmp_path),
        "--state",
        str(tmp_path / "state.json"),
        "--cache",
        str(tmp_path / "cache.json"),
    )


async def test_remote_pairing_flow_over_the_websocket(bt_hub, tmp_path):
    s, ws, ack = await open_session(bt_hub)
    async with s, ws:
        assert ack["d"]["bt"]["remotes_max"] == 32
        st = await until(
            ws, bt_state()
        )  # pushed after hello_ack: nothing configured, no window, no helper
        assert st["d"]["state"] == "off" and "window" not in st["d"]
        await send(ws, "bt_pair", {"on": True, "mode": "remote", "secs": 60}, 2)
        rep = await until(ws, lambda m: m["id"] == 2)
        assert (
            rep["t"] == "bt_state"
            and rep["d"]["window"] == "remote"
            and rep["d"]["state"] == "starting"
        )
        # the helper starts, the window opens, two advertisers are listed (the second has no name yet)
        st = await until(ws, bt_state(lambda d: len(d.get("candidates", [])) == 2))
        assert st["d"]["state"] == "ready" and st["d"]["window"] == "remote"
        assert st["d"]["candidates"] == [
            {"addr": "11:22:33:44:55:77", "name": "Click", "rssi": -50},
            {"addr": "11:22:33:44:55:88", "rssi": -70},
        ]
        # a remote that is not listed is refused; the listed one is bonded
        await send(ws, "bt_pair", {"on": True, "mode": "remote", "addr": "11:22:33:44:55:AA"}, 3)
        assert (await until(ws, lambda m: m["id"] == 3))["d"]["code"] == "no_candidate"
        await send(ws, "bt_pair", {"on": True, "mode": "remote", "addr": "11:22:33:44:55:77"}, 4)
        assert (await until(ws, lambda m: m["id"] == 4))["t"] == "bt_state"
        done = await until(
            ws, bt_state(lambda d: d.get("pairing", {}).get("result") == "ok" and "window" not in d)
        )
        assert done["d"]["pairing"] == {
            "result": "ok",
            "addr": "11:22:33:44:55:09",
            "name": "Picked",
            "kind": "remote",
            "transport": "le",
        }
        assert "candidates" not in done["d"]
        picked = [r for r in done["d"]["remotes"] if r.get("name") == "Picked"]
        assert (
            picked and picked[0]["configured"] is False
        )  # bonded in the hub, not in the config: offered to the editor
        log = (tmp_path / "helper.log").read_text()
        assert "PAIR 1 60 1" in log and "PAIR_REMOTE 11:22:33:44:55:77 1" in log


async def test_config_binds_slots_by_address_and_death_is_reported(bt_hub, tmp_path):
    s, ws, _ = await open_session(bt_hub)
    async with s, ws:
        await until(ws, bt_state())
        await send(ws, "set_config", {"config": CONFIG}, 2)
        assert (await until(ws, lambda m: m["id"] == 2))["t"] == "config_ack"
        st = await until(
            ws,
            bt_state(
                lambda d: d["state"] == "ready" and len(d.get("remotes", [])) == 2 and d["hosts"]
            ),
        )
        hosts = {h["slot"]: h for h in st["d"]["hosts"]}
        assert hosts[3]["configured"] is True and hosts[3]["addr"] == "AA:BB:CC:DD:EE:01"
        old = next(h for h in st["d"]["hosts"] if h.get("name") == "Old TV")
        assert old["configured"] is False and old["slot"] != 3
        remotes = {r["slot"]: r for r in st["d"]["remotes"]}
        assert remotes[2]["configured"] is True and remotes[2]["connected"] is True
        # a key to the configured host goes to the helper under its own slot for that address
        await send(ws, "bt_key", {"slot": 3, "page": 7, "usage": 40, "action": "press"}, 3)
        assert (await until(ws, lambda m: m["id"] == 3))["t"] != "error"
        await send(ws, "bt_key", {"slot": 9, "page": 7, "usage": 40, "action": "press"}, 4)
        assert (await until(ws, lambda m: m["id"] == 4))["d"]["code"] == "no_slot"
        log = tmp_path / "helper.log"
        end = time.monotonic() + 5
        while (
            not log.exists() or "KEY 0 7 40 0" not in log.read_text()
        ) and time.monotonic() < end:
            await asyncio.sleep(0.02)
        assert "KEY 0 7 40 1" in log.read_text()
        # the helper dies: state leaves ready, the lists are cleared, the hub carries on
        pid = int(subprocess.check_output(["pgrep", "-P", str(bt_hub.proc.pid)]).split()[0])  # noqa: ASYNC221
        os.kill(pid, signal.SIGKILL)
        gone = await until(ws, bt_state(lambda d: d["state"] != "ready"))
        assert (
            gone["d"]["state"] == "off"
            and gone["d"]["hosts"] == []
            and not gone["d"].get("remotes")
        )
        await send(ws, "ping", {}, 5)
        assert (await until(ws, lambda m: m["id"] == 5))["t"] == "pong"
