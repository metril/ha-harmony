"""Engine part 1 against `hub --sim`: config format 2, real IR output lines, hold/repeat,
ir_send, activity-from-key, config_hash and the activity_list push."""

import asyncio
import re
import time

import aiohttp
import pytest
from test_hub_engine import call, env, hello, read_lines, recv

NEC_LINE = re.compile(r"^ir port=(\d+) hz=38000 n=(\d+) us=([\d,]+)$")

CONFIG2 = {
    "format": 2,
    "devices": [
        {
            "id": "tv",
            "name": "TV",
            "settle_ms": 100,
            "power": {
                "on": {"proto": "nec", "addr": 4, "cmd": 2},
                "off": {"proto": "nec", "addr": 4, "cmd": 3},
            },
            "inputs": {"hdmi1": {"proto": "nec", "addr": 4, "cmd": 9}},
            "commands": {
                "vol_up": {"proto": "nec", "addr": 4, "cmd": 16, "port_mask": 3},
                "blip": {"raw": {"hz": 38000, "us": [9000, 4500, 560, 560]}},
                "pr": {"pronto": "0000 006d 0001 0000 0018 0018"},
            },
        },
        {"id": "sony", "commands": {"ok": {"proto": "sirc12", "addr": 1, "cmd": 11}}},
    ],
    "activities": [
        {
            "id": "watch",
            "name": "Watch TV",
            "devices": [{"device": "tv", "input": "hdmi1"}],
            "steps": [{"type": "device", "device": "tv", "command": "blip"}],
            "off": [{"type": "device", "device": "tv", "command": "power_off"}],
            "keys": {"7": {"type": "device", "device": "tv", "command": "pr"}},
        },
        {"id": "music", "name": "Music", "steps": [{"type": "none"}]},
    ],
    "keys": {
        "1": {"type": "activity", "activity": "watch"},
        "2": {"type": "activity", "activity": "off"},
        "5": {"type": "device", "device": "tv", "command": "vol_up"},
        "6": {"type": "ir", "proto": "nec", "addr": 4, "cmd": 8},
        "8": {"type": "device", "device": "sony", "command": "ok"},
    },
}

BLIP = "ir port=255 hz=38000 n=4 us=9000,4500,560,560"
PR = "ir port=255 hz=38029 n=2 us=631,631"


def pending_lines(hub):
    return hub.out_path.read_bytes()[hub.out_pos :].count(b"\n")


async def cfg(ws, config=CONFIG2, id=2, new_devices=None):
    """set_config, then swallow the device_state pushes for devices that are new to the hub."""
    r = await call(ws, "set_config", {"config": config}, id=id)
    assert r["t"] == "config_ack" and r["d"]["ok"] is True, r
    await drain_devices(ws, len(config.get("devices", [])) if new_devices is None else new_devices)


async def drain_devices(ws, n):
    for _ in range(n):
        assert (await recv(ws))["t"] == "device_state"


async def until(ws, t="state_update", timeout=5):
    """Messages up to and including the first of type t."""
    msgs = []
    while True:
        msgs.append(await recv(ws, timeout))
        if msgs[-1]["t"] == t:
            return msgs


async def ir_send(ws, args, id=9):
    return (
        await call(ws, "command", {"device": "hub", "command": "ir_send", "args": args}, id=id)
    )["d"]


async def test_ir_send_each_form(hub_process):
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_process.url) as ws:
        await hello(ws)
        assert (await ir_send(ws, {"proto": "nec", "addr": 4, "cmd": 8})) == {"ok": True}
        assert (
            await ir_send(ws, {"raw": {"hz": 38000, "us": [9000, 4500, 560, 560]}, "port_mask": 5})
        )["ok"]
        assert (await ir_send(ws, {"pronto": "0000 006d 0001 0000 0018 0018"}))["ok"]
        nec, raw, pr = read_lines(hub_process, 3)
        m = NEC_LINE.match(nec)
        assert m and m[1] == "255" and m[2] == "68"
        assert m[3].startswith("9000,4500,562,562,562,562,562,1687")
        assert raw == "ir port=5 hz=38000 n=4 us=9000,4500,560,560"
        assert pr == PR


@pytest.mark.parametrize(
    "args",
    [
        {},
        {"proto": "nec", "addr": 4, "cmd": 8, "pronto": "0000 006d 0001 0000 0018 0018"},
        {"proto": "nec", "addr": 999, "cmd": 8},
        {"proto": "warp", "addr": 1, "cmd": 1},
        {"raw": {"hz": 38000, "us": [1, 2, 3]}},
        {"pronto": "0000 006d 0005 0000 0018"},
        {"proto": "nec", "addr": 4, "cmd": 8, "port_mask": 0},
    ],
)
async def test_ir_send_rejects_bad_actions(hub_process, args):
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_process.url) as ws:
        await hello(ws)
        d = await ir_send(ws, args)
        assert d["ok"] is False and d["message"]
        await asyncio.sleep(0.1)
        assert pending_lines(hub_process) == 0


async def test_ir_send_needs_object_args(hub_process):
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_process.url) as ws:
        await hello(ws)
        r = await call(ws, "command", {"device": "hub", "command": "ir_send"}, id=3)
        assert r["d"]["ok"] is False


async def test_format2_activity_and_device_commands(hub_process):
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_process.url) as ws:
        await hello(ws)
        await cfg(ws)
        r = await call(
            ws,
            "command",
            {"device": "hub", "command": "start_activity", "args": {"name": "Watch TV"}},
            id=3,
        )
        assert r["d"]["ok"] is True
        msgs = await until(
            ws
        )  # the transition runs after the reply: tv on, tv input, then the step
        assert msgs[-1]["d"] == {"activity": 0, "power": True}
        assert [m["d"] for m in msgs[:-1]] == [
            {"device": "tv", "power": True},
            {"device": "tv", "power": True, "input": "hdmi1"},
        ]
        on, inp, blip = read_lines(hub_process, 3)
        assert on.startswith("ir port=255 hz=38000 n=68 ") and inp.startswith(
            "ir port=255 hz=38000 n=68 "
        )
        assert on != inp and blip == BLIP
        r = await call(ws, "command", {"device": "hub", "command": "power_off"}, id=4)
        assert r["d"]["ok"] is True
        msgs = await until(ws)
        assert msgs[-1]["d"] == {"activity": 0, "power": False}
        assert [m["d"] for m in msgs[:-1]] == [{"device": "tv", "power": False}]
        off1, off2 = read_lines(hub_process, 2)  # power-down, then the activity's own off step
        assert off1 == off2 and off1.startswith("ir port=255 hz=38000 n=68 ")


async def test_format2_bad_configs_are_rejected_with_reason(hub_process):
    bad = [
        {"format": 2, "keys": {"1": {"type": "ir", "proto": "nec", "addr": 300, "cmd": 1}}},
        {"format": 2, "keys": {"256": {"type": "none"}}},
        {"format": 2, "keys": {"1": {"type": "device", "device": "ghost", "command": "x"}}},
        {"format": 2, "keys": {"1": {"type": "activity", "activity": "ghost"}}},
        {"format": 2, "keys": {"1": {"type": "ir"}}},
        {"format": 3},
    ]
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_process.url) as ws:
        await hello(ws)
        await cfg(ws)
        for b in bad:
            r = await call(ws, "set_config", {"config": b}, id=3)
            assert r["d"]["ok"] is False and r["d"]["message"] != "invalid config", (b, r)
        # the good config is still active
        assert (await call(ws, "ping", id=4))["t"] == "pong"
        assert read_lines(hub_process, 0) == []


async def test_key_presses_send_ir(hub_rf, run_remote):
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_rf.url) as ws:
        await hello(ws)
        await cfg(ws)
        for key, port in ((5, "3"), (6, "255")):
            assert run_remote(hub_rf.rf, "--press", str(key)).returncode == 0
            assert (await recv(ws))["t"] == "key_event"
            m = NEC_LINE.match(read_lines(hub_rf, 1)[0])
            assert m and m[1] == port and m[2] == "68"


async def test_activity_from_key_starts_and_powers_down(hub_rf, run_remote):
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_rf.url) as ws:
        await hello(ws)
        await cfg(ws)
        assert run_remote(hub_rf.rf, "--press", "1").returncode == 0
        ke = await recv(ws)
        assert ke["t"] == "key_event" and ke["d"]["key"] == 1
        msgs = await until(ws)
        assert msgs[-1] == {
            "v": 1,
            "id": 0,
            "t": "state_update",
            "d": {"activity": 0, "power": True},
        }
        assert read_lines(hub_rf, 3)[2] == BLIP
        # per-activity key layer: key 7 only exists in "watch"
        assert run_remote(hub_rf.rf, "--press", "7").returncode == 0
        assert (await recv(ws))["t"] == "key_event"
        assert read_lines(hub_rf, 1) == [PR]
        assert run_remote(hub_rf.rf, "--press", "2").returncode == 0
        assert (await recv(ws))["t"] == "key_event"
        assert (await until(ws))[-1]["d"] == {"activity": 0, "power": False}
        assert read_lines(hub_rf, 2)[0].startswith("ir port=255 hz=38000 n=68")
        # key 7 is no longer mapped now that no activity runs
        assert run_remote(hub_rf.rf, "--press", "7").returncode == 0
        assert (await recv(ws))["t"] == "key_event"
        await asyncio.sleep(0.2)
        assert pending_lines(hub_rf) == 0


async def test_hold_repeats_until_release(hub_rf, run_remote):
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_rf.url) as ws:
        await hello(ws)
        await cfg(ws)
        assert run_remote(hub_rf.rf, "--press", "6").returncode == 0
        assert run_remote(hub_rf.rf, "--press", "6", "--action", "2").returncode == 0
        t0 = time.monotonic()
        while pending_lines(hub_rf) < 4 and time.monotonic() - t0 < 5:
            await asyncio.sleep(0.02)
        assert run_remote(hub_rf.rf, "--press", "6", "--action", "1").returncode == 0
        lines = read_lines(hub_rf, pending_lines(hub_rf))
        assert len(lines) >= 4
        assert NEC_LINE.match(lines[0]).group(2) == "68"
        assert all(NEC_LINE.match(ln).group(2) == "4" for ln in lines[1:])  # NEC repeat code
        n = len(lines)
        await asyncio.sleep(0.4)  # released: nothing else is sent
        assert pending_lines(hub_rf) == 0
        assert n >= 4


async def test_sirc_single_press_emits_three_frames(hub_rf, run_remote):
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_rf.url) as ws:
        await hello(ws)
        await cfg(ws)
        assert run_remote(hub_rf.rf, "--press", "8").returncode == 0
        lines = read_lines(hub_rf, 3)
        assert len(lines) == 3 and len(set(lines)) == 1
        assert lines[0].startswith("ir port=255 hz=40000 n=26 ")
        await asyncio.sleep(0.3)
        assert pending_lines(hub_rf) == 0


async def test_poll_loop_stays_responsive_while_repeating(hub_rf, run_remote):
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_rf.url) as ws:
        await hello(ws)
        await cfg(ws)
        assert run_remote(hub_rf.rf, "--press", "6").returncode == 0
        assert run_remote(hub_rf.rf, "--press", "6", "--action", "2").returncode == 0
        t0 = time.monotonic()
        for i in range(5):
            await ws.send_str(env("ping", id=20 + i))
            while (msg := await recv(ws))["t"] != "pong":
                assert msg["t"] == "key_event"
        assert time.monotonic() - t0 < 1.0
        assert run_remote(hub_rf.rf, "--press", "6", "--action", "1").returncode == 0


# ---------------------------------------------------------------- config_hash / activity list


async def test_config_hash_stable_and_distinct(start_hub):
    h1, h2 = start_hub(), start_hub()
    hashes = []
    for hub, config in ((h1, CONFIG2), (h2, CONFIG2), (h1, {**CONFIG2, "keys": {}})):
        async with aiohttp.ClientSession() as s, s.ws_connect(hub.url) as ws:
            ack = await hello(ws)
            if hub is h1 and not hashes:
                assert "config_hash" not in ack["d"]  # nothing loaded yet
            if len(hashes) >= 2:
                await drain_devices(ws, 2)  # h1 already holds CONFIG2's devices
            await cfg(ws, config, new_devices=None if len(hashes) < 2 else 0)
        async with aiohttp.ClientSession() as s, s.ws_connect(hub.url) as ws:
            ack = await hello(ws)
            await drain_devices(ws, 2)  # the full device set follows the state_update
            assert re.fullmatch(r"[0-9a-f]{8}", ack["d"]["config_hash"])
            hashes.append(ack["d"]["config_hash"])
    assert hashes[0] == hashes[1]
    assert hashes[2] != hashes[0]


async def test_config_ack_carries_the_resulting_hash(hub_process):
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_process.url) as ws:
        await hello(ws)
        ack = await call(ws, "set_config", {"config": CONFIG2}, id=2)
        assert ack["t"] == "config_ack" and ack["d"]["ok"] is True
        assert re.fullmatch(r"[0-9a-f]{8}", ack["d"]["config_hash"])
        await drain_devices(ws, len(CONFIG2.get("devices", [])))
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_process.url) as ws:
        assert (await hello(ws))["d"]["config_hash"] == ack["d"]["config_hash"]
        await drain_devices(ws, len(CONFIG2.get("devices", [])))
        dup = {"activities": [{"name": "A"}, {"name": "A"}]}
        bad = await call(ws, "set_config", {"config": dup}, id=3)
        assert bad["d"]["ok"] is False and "config_hash" not in bad["d"]


async def test_config_hash_survives_restart_via_cache(start_hub, tmp_path):
    cache = str(tmp_path / "c.cache")
    hub = start_hub("--cache", cache)
    async with aiohttp.ClientSession() as s, s.ws_connect(hub.url) as ws:
        await hello(ws)
        await cfg(ws)
    async with aiohttp.ClientSession() as s, s.ws_connect(hub.url) as ws:
        h = (await hello(ws))["d"]["config_hash"]
    hub.stop()
    hub2 = start_hub("--cache", cache)
    async with aiohttp.ClientSession() as s, s.ws_connect(hub2.url) as ws:
        ack = await hello(ws)
        assert ack["d"]["config_hash"] == h
        assert ack["d"]["activities"] == ["Watch TV", "Music"]


async def test_activity_list_pushed_to_other_clients(hub_process):
    async with (
        aiohttp.ClientSession() as s,
        s.ws_connect(hub_process.url) as a,
        s.ws_connect(hub_process.url) as b,
    ):
        await hello(a)
        await hello(b)
        await cfg(a)
        push = await recv(b)
        await drain_devices(b, 2)
        assert push["t"] == "activity_list" and push["id"] == 0
        assert push["d"]["activities"] == ["Watch TV", "Music"]
        assert re.fullmatch(r"[0-9a-f]{8}", push["d"]["config_hash"])
        with pytest.raises(asyncio.TimeoutError):  # the sender already got its config_ack
            await recv(a, timeout=0.3)
        ack = await hello_again(hub_process)
        assert ack["d"]["config_hash"] == push["d"]["config_hash"]
        assert ack["d"]["activities"] == push["d"]["activities"]


async def hello_again(hub):
    async with aiohttp.ClientSession() as s, s.ws_connect(hub.url) as ws:
        return await hello(ws)


async def test_reload_dropping_running_activity_pushes_state(hub_process):
    async with (
        aiohttp.ClientSession() as s,
        s.ws_connect(hub_process.url) as a,
        s.ws_connect(hub_process.url) as b,
    ):
        await hello(a)
        await hello(b)
        await cfg(a)
        assert (await recv(b))["t"] == "activity_list"
        await drain_devices(b, 2)
        await a.send_str(
            '{"v":1,"id":3,"t":"command","d":{"device":"hub","command":"start_activity","args":{"name":"Music"}}}'
        )
        assert (await recv(a))["t"] == "command_result"
        assert (await recv(a))["d"] == {"activity": 1, "power": True}
        assert (await recv(b))["d"] == {"activity": 1, "power": True}
        # new config without "Music": power drops for everyone, list only for the others
        await cfg(a, {"format": 2, "activities": [{"id": "only", "name": "Only"}]}, id=4)
        assert (await recv(a))["d"] == {"activity": 0, "power": False}
        lst = await recv(b)
        assert lst["t"] == "activity_list" and lst["d"]["activities"] == ["Only"]
        assert (await recv(b))["d"] == {"activity": 0, "power": False}


async def test_failed_set_config_pushes_nothing(hub_process):
    async with (
        aiohttp.ClientSession() as s,
        s.ws_connect(hub_process.url) as a,
        s.ws_connect(hub_process.url) as b,
    ):
        await hello(a)
        await hello(b)
        r = await call(
            a, "set_config", {"config": {"format": 2, "keys": {"999": {"type": "none"}}}}, id=3
        )
        assert r["d"]["ok"] is False
        with pytest.raises(asyncio.TimeoutError):
            await recv(b, timeout=0.3)


# ---------------------------------------------------------------- tile_update / --rf


async def test_tile_update_without_remote_and_bad_args(hub_rf):
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_rf.url) as ws:
        await hello(ws)
        args = {"remote": 1, "page": 9, "tile": 3, "text": "21.5"}
        r = await call(
            ws, "command", {"device": "hub", "command": "tile_update", "args": args}, id=3
        )
        assert r["t"] == "command_result" and r["d"]["ok"] is True and r["id"] == 3  # held until link-up
        for bad in ({}, {**args, "tile": 256}, {**args, "text": 5}, {"remote": 1}):
            r = await call(
                ws, "command", {"device": "hub", "command": "tile_update", "args": bad}, id=4
            )
            assert r["t"] == "command_result" and r["d"]["ok"] is False


def test_rf_path_too_long_fails_clearly(built_binaries, tmp_path):
    import subprocess

    path = str(tmp_path / ("x" * 120) / "rf.sock")
    res = subprocess.run(
        [str(built_binaries / "hub"), "--sim", "--token", "t", "--rf", path],
        capture_output=True,
        text=True,
        timeout=10,
        check=False,
    )
    assert res.returncode != 0
    assert "sun_path" in res.stderr and re.search(r"shorter than \d+ bytes", res.stderr)
