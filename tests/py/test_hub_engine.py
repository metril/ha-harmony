"""Drives the real `hub --sim` binary over WebSocket: config, activities, sinks, cache, deadlines."""

import asyncio
import json
import os
import signal
import time
from pathlib import Path

import aiohttp

TOKEN = "test-token"

CONFIG = {
    "keys": {"1": {"type": "ha", "event": "menu"}},
    "activities": [
        {
            "name": "TV",
            "keys": {"2": {"type": "ir", "port_mask": 3, "pronto": [0, 109, 1, 0, 24, 24]}},
            "steps": [
                {"type": "ir", "port_mask": 1, "pronto": [0, 109, 1, 0, 10, 20]},
                {"type": "bt", "usage": 233},
                {"type": "ir", "port_mask": 4, "pronto": [0, 109, 1, 0, 30, 40]},
            ],
            "off": [{"type": "ir", "port_mask": 255, "pronto": [0, 109, 1, 0, 50, 60]}],
        },
        {"name": "Music", "steps": [{"type": "none"}]},
    ],
}


def env(t, d=None, id=1):
    return json.dumps({"v": 1, "id": id, "t": t, "d": d if d is not None else {}})


async def call(ws, t, d=None, id=1):
    await ws.send_str(env(t, d, id))
    return await recv(ws)


async def recv(ws, timeout=5, keep_attached=False):
    """Next message; `remote_attached` pushes (an RF remote showing up) are skipped by default."""
    while True:
        msg = json.loads((await ws.receive(timeout=timeout)).data)
        if keep_attached or msg.get("t") != "remote_attached":
            return msg


async def hello(ws):
    ack = await call(ws, "hello", {"token": TOKEN})
    assert ack["t"] == "hello_ack"
    su = await recv(ws)  # live state pushed right after hello_ack
    assert su["t"] == "state_update" and su["id"] == 0
    return ack


def read_lines(hub, n, timeout=5.0):
    """Reads n stdout lines (sink mirror) from the hub's stdout file."""
    deadline = time.monotonic() + timeout
    while True:
        data = hub.out_path.read_bytes()[hub.out_pos :]
        if data.count(b"\n") >= n or time.monotonic() >= deadline:
            break
        time.sleep(0.02)
    lines = data.split(b"\n")
    hub.out_pos += sum(len(ln) + 1 for ln in lines[:n])
    return [ln.decode() for ln in lines[:n]]


def stop(hub):
    hub.proc.send_signal(signal.SIGTERM)
    assert hub.proc.wait(timeout=5) == 0


async def test_config_activity_and_sink_order(hub_process):
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_process.url) as ws:
        ack = await hello(ws)
        assert ack["d"]["activities"] == []
        r = await call(ws, "set_config", {"config": CONFIG}, id=2)
        assert r["t"] == "config_ack" and r["d"]["ok"] is True and r["id"] == 2
        r = await call(
            ws,
            "command",
            {"device": "hub", "command": "start_activity", "args": {"name": "TV"}},
            id=3,
        )
        assert r["t"] == "command_result" and r["d"]["ok"] is True and r["id"] == 3
        st = await recv(ws)
        assert st["t"] == "state_update" and st["d"] == {"activity": 0, "power": True}
        assert read_lines(hub_process, 4) == [
            "ir port=1 hz=38029 n=2 us=263,526",
            "bt host=0 page=0c usage=00e9 down=1",
            "bt host=0 page=0c usage=00e9 down=0",
            "ir port=4 hz=38029 n=2 us=789,1052",
        ]
        r = await call(ws, "command", {"device": "hub", "command": "power_off"}, id=4)
        assert r["d"]["ok"] is True
        st = await recv(ws)
        assert st["d"] == {"activity": 0, "power": False}
        assert read_lines(hub_process, 1) == ["ir port=255 hz=38029 n=2 us=1315,1578"]
        r = await call(
            ws,
            "command",
            {"device": "hub", "command": "start_activity", "args": {"name": "x"}},
            id=5,
        )
        assert r["d"]["ok"] is False and r["d"]["message"]
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_process.url) as ws:
        ack = await hello(ws)
        assert ack["d"]["activities"] == ["TV", "Music"]


async def test_state_update_reaches_other_clients_only_when_authed(hub_process):
    async with (
        aiohttp.ClientSession() as s,
        s.ws_connect(hub_process.url) as a,
        s.ws_connect(hub_process.url) as b,
        s.ws_connect(hub_process.url) as lurker,
    ):
        await hello(a)
        await hello(b)
        await call(a, "set_config", {"config": CONFIG}, id=2)
        await a.send_str(
            env(
                "command",
                {"device": "hub", "command": "start_activity", "args": {"name": "Music"}},
                3,
            )
        )
        assert (await recv(a))["t"] == "command_result"
        assert (await recv(a))["d"] == {"activity": 1, "power": True}
        assert (await recv(b))[
            "t"
        ] == "activity_list"  # pushed to the other clients after set_config
        assert (await recv(b))["d"] == {"activity": 1, "power": True}
        await asyncio.sleep(0.2)
        await lurker.send_str(env("ping", id=9))  # not authenticated: never received a broadcast
        err = await recv(lurker)
        assert err["d"]["code"] == "unauthorized"


async def test_malformed_config_keeps_old_map(hub_rf, run_remote):
    hub_process = hub_rf
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_process.url) as ws:
        await hello(ws)
        assert (await call(ws, "set_config", {"config": CONFIG}, id=2))["d"]["ok"] is True
        for bad in (
            {"activities": [{"name": "A"}, {"name": "A"}]},
            {"keys": {"1": {"type": "warp"}}},
            {"activities": [{"name": "X" * 40}]},
            [1, 2],
        ):
            r = await call(ws, "set_config", {"config": bad}, id=3)
            assert r["t"] == "config_ack" and r["d"]["ok"] is False and r["d"]["message"]
        # old map still works: TV key 2 is IR once TV is running
        await call(
            ws,
            "command",
            {"device": "hub", "command": "start_activity", "args": {"name": "TV"}},
            id=4,
        )
        await recv(ws)  # state_update
        read_lines(hub_process, 4)
        assert run_remote(hub_rf.rf, "--press", "2", "--remote", "1").returncode == 0
        ke = await recv(ws)
        assert ke["t"] == "key_event" and ke["d"] == {
            "remote": 1,
            "key": 2,
            "action": 0,
            "activity": 0,
        }
        assert read_lines(hub_process, 1) == ["ir port=3 hz=38029 n=2 us=631,631"]


async def test_unmapped_key_still_emits(hub_rf, run_remote):
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_rf.url) as ws:
        await hello(ws)
        await call(ws, "set_config", {"config": CONFIG}, id=2)
        res = run_remote(hub_rf.rf, "--press", "200", "--remote", "2", "--action", "1")
        assert res.returncode == 0, res.stderr
        ke = await recv(ws)
        assert ke["d"] == {"remote": 2, "key": 200, "action": 1, "activity": 255}


async def test_cache_restores_map_after_restart(start_hub, tmp_path):
    cache = str(tmp_path / "map.cache")
    hub = start_hub("--cache", cache)
    async with aiohttp.ClientSession() as s, s.ws_connect(hub.url) as ws:
        await hello(ws)
        assert (await call(ws, "set_config", {"config": CONFIG}, id=2))["d"]["ok"] is True
    stop(hub)
    assert os.path.exists(cache)
    assert not [p for p in os.listdir(tmp_path) if ".tmp." in p]
    hub2 = start_hub("--cache", cache)
    async with aiohttp.ClientSession() as s, s.ws_connect(hub2.url) as ws:
        ack = await hello(ws)
        assert ack["d"]["activities"] == ["TV", "Music"]
        r = await call(
            ws,
            "command",
            {"device": "hub", "command": "start_activity", "args": {"name": "TV"}},
            id=2,
        )
        assert r["d"]["ok"] is True
    assert len(read_lines(hub2, 4)) == 4
    stop(hub2)
    # a corrupt cache is ignored, hub still starts with an empty map
    raw = bytearray(Path(cache).read_bytes())
    raw[12] ^= 0xFF
    Path(cache).write_bytes(bytes(raw))
    hub3 = start_hub("--cache", cache)
    async with aiohttp.ClientSession() as s, s.ws_connect(hub3.url) as ws:
        assert (await hello(ws))["d"]["activities"] == []


async def test_auth_deadline_closes_unauthenticated(start_hub):
    hub = start_hub("--auth-timeout-ms", "800")
    async with aiohttp.ClientSession() as s, s.ws_connect(hub.url) as ws:
        t0 = time.monotonic()
        msg = await ws.receive(timeout=5)
        assert msg.type == aiohttp.WSMsgType.CLOSE and msg.data == 1008
        assert 0.6 < time.monotonic() - t0 < 5
        # an authenticated connection is not subject to the auth deadline
    async with aiohttp.ClientSession() as s, s.ws_connect(hub.url) as ws:
        await hello(ws)
        await asyncio.sleep(1.0)  # past the auth deadline, with margin for a slow hello
        assert (await call(ws, "ping", id=7))["t"] == "pong"
    assert hub.alive()


async def test_authenticated_client_is_never_closed_for_silence(start_hub):
    """Liveness is the kernel's TCP keepalive: --idle-timeout-ms only bounds a not-yet-authenticated socket."""
    hub = start_hub("--idle-timeout-ms", "500")
    async with aiohttp.ClientSession() as s, s.ws_connect(hub.url) as ws:
        ack = await hello(ws)
        assert ack["d"]["tcp_keepalive"] is True
        await asyncio.sleep(1.2)  # well past the old idle limit, no traffic
        assert (await call(ws, "ping", id=10))[
            "t"
        ] == "pong"  # app PINGs from old integrations still answered
    assert hub.alive()
