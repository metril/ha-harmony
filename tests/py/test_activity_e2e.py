"""Engine part 2 against `hub --sim`: hub-executed activities, device_state pushes, set_device_state,
state persistence across restarts, remote key hold events."""

import asyncio
import json
import time

import aiohttp
import pytest
from conftest import rf_attach
from ir_ref import encode
from test_engine_ir import cfg, drain_devices, pending_lines, until
from test_hub_engine import call, hello, read_lines, recv

SETTLE_TV, SETTLE_AMP = 40, 20


def nec(addr, cmd):
    return {"proto": "nec", "addr": addr, "cmd": cmd}


def ir_line(code: dict, port: int = 255) -> str:
    hz, once, _ = encode(code)
    return f"ir port={port} hz={hz} n={len(once)} us={','.join(str(d) for d in once)}"


TV_ON, TV_OFF = nec(4, 1), nec(4, 2)
TV_H1, TV_H2 = nec(4, 10), nec(4, 11)
AMP_TOGGLE, AMP_OPT = nec(9, 1), nec(9, 5)

CONFIG = {
    "format": 2,
    "devices": [
        {
            "id": "tv",
            "settle_ms": SETTLE_TV,
            "power": {"on": TV_ON, "off": TV_OFF},
            "inputs": {"hdmi1": TV_H1, "hdmi2": TV_H2},
        },
        {
            "id": "amp",
            "settle_ms": SETTLE_AMP,
            "power": {"toggle": AMP_TOGGLE},
            "inputs": {"opt": AMP_OPT},
        },
    ],
    "activities": [
        {
            "id": "watch",
            "name": "Watch",
            "devices": [{"device": "tv", "input": "hdmi1"}, {"device": "amp", "input": "opt"}],
        },
        {"id": "movie", "name": "Movie", "devices": [{"device": "tv", "input": "hdmi2"}]},
        {"id": "music", "name": "Music", "devices": [{"device": "amp", "input": "opt"}]},
    ],
    "keys": {
        "1": {"type": "activity", "activity": "watch"},
        "2": {"type": "activity", "activity": "off"},
        "3": {"type": "activity", "activity": "movie"},
        "6": nec(5, 5) | {"type": "ir"},
    },
}


def start(ws, name, id=3):
    return call(
        ws, "command", {"device": "hub", "command": "start_activity", "args": {"name": name}}, id=id
    )


def dev(device, power, input=None):
    d = {"device": device, "power": power}
    if input:
        d["input"] = input
    return d


async def test_transition_order_and_pushes(hub_process):
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_process.url) as ws:
        await hello(ws)
        await cfg(ws, CONFIG)
        r = await start(ws, "Watch")
        assert r["d"] == {"ok": True}  # accepted; the transition runs after the reply
        msgs = await until(ws)
        assert msgs[-1]["d"] == {"activity": 0, "power": True}  # reported once, at completion
        assert [m["d"] for m in msgs[:-1]] == [
            dev("tv", True),
            dev("amp", True),
            dev("tv", True, "hdmi1"),
            dev("amp", True, "opt"),
        ]
        assert read_lines(hub_process, 4) == [
            ir_line(c) for c in (TV_ON, AMP_TOGGLE, TV_H1, AMP_OPT)
        ]

        # watch -> movie: tv stays on (input change only), the toggle amp is switched off
        assert (await start(ws, "Movie"))["d"] == {"ok": True}
        msgs = await until(ws)
        assert msgs[-1]["d"] == {"activity": 1, "power": True}
        assert [m["d"] for m in msgs[:-1]] == [dev("amp", False), dev("tv", True, "hdmi2")]
        assert read_lines(hub_process, 2) == [ir_line(c) for c in (AMP_TOGGLE, TV_H2)]

        # watch again: never a power frame for the tv that is already on
        assert (await start(ws, "Watch"))["d"] == {"ok": True}
        msgs = await until(ws)
        assert read_lines(hub_process, 3) == [ir_line(c) for c in (AMP_TOGGLE, TV_H1, AMP_OPT)]

        r = await call(ws, "command", {"device": "hub", "command": "power_off"}, id=9)
        assert r["d"] == {"ok": True}
        msgs = await until(ws)
        assert msgs[-1]["d"] == {"activity": 0, "power": False}
        assert [m["d"] for m in msgs[:-1]] == [dev("tv", False), dev("amp", False)]
        assert read_lines(hub_process, 2) == [ir_line(c) for c in (TV_OFF, AMP_TOGGLE)]


async def test_new_client_gets_state_then_all_devices(hub_process):
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_process.url) as ws:
        await hello(ws)
        await cfg(ws, CONFIG)
        await start(ws, "Music")
        await until(ws)
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_process.url) as ws2:
        await ws2.send_str(
            json.dumps({"v": 1, "id": 1, "t": "hello", "d": {"token": "test-token"}})
        )
        assert (await recv(ws2))["t"] == "hello_ack"
        assert (await recv(ws2))["d"] == {"activity": 2, "power": True}
        pushes = [(await recv(ws2))["d"] for _ in range(2)]
        assert pushes == [dev("tv", False), dev("amp", True, "opt")]


async def test_set_device_state_sends_no_ir(hub_process):
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_process.url) as ws:
        await hello(ws)
        await cfg(ws, CONFIG)

        async def sds(args, id=5):
            return await call(
                ws, "command", {"device": "hub", "command": "set_device_state", "args": args}, id=id
            )

        r = await sds({"device": "tv", "power": True, "input": "hdmi2"})
        assert r["t"] == "command_result" and r["d"] == {"ok": True}
        assert (await recv(ws))["d"] == dev("tv", True, "hdmi2")
        r = await sds({"device": "tv", "input": None})  # input now unknown
        assert r["d"]["ok"] is True
        assert (await recv(ws))["d"] == dev("tv", True)
        for bad in (
            {"device": "ghost", "power": True},
            {"device": "tv", "input": "vga"},
            {"device": "tv"},
            {"power": True},
            {"device": "tv", "power": "yes"},
        ):
            r = await sds(bad)
            assert r["d"]["ok"] is False and r["d"]["message"], bad
        # the corrected state drives the next transition: tv is on, input unknown
        await start(ws, "Movie")
        await until(ws)
        assert read_lines(hub_process, 1) == [ir_line(TV_H2)]
        await asyncio.sleep(0.2)
        assert read_lines(hub_process, 0) == []  # set_device_state itself sent nothing


async def test_state_survives_restart(start_hub, tmp_path):
    cache, state = str(tmp_path / "c.cache"), str(tmp_path / "s.state")
    hub = start_hub("--cache", cache, "--state", state)
    async with aiohttp.ClientSession() as s, s.ws_connect(hub.url) as ws:
        await hello(ws)
        await cfg(ws, CONFIG)
        await start(ws, "Watch")
        await until(ws)
    read_lines(hub, 4)
    hub.stop()  # clean shutdown flushes the debounced state write

    hub2 = start_hub("--cache", cache, "--state", state)
    async with aiohttp.ClientSession() as s, s.ws_connect(hub2.url) as ws:
        await ws.send_str(json.dumps({"v": 1, "id": 1, "t": "hello", "d": {"token": "test-token"}}))
        assert (await recv(ws))["t"] == "hello_ack"
        assert (await recv(ws))["d"] == {"activity": 0, "power": True}
        assert [(await recv(ws))["d"] for _ in range(2)] == [
            dev("tv", True, "hdmi1"),
            dev("amp", True, "opt"),
        ]
        # restored state is used for planning: music only needs the tv switched off
        await start(ws, "Music")
        await until(ws)
        assert read_lines(hub2, 1) == [ir_line(TV_OFF)]


@pytest.mark.parametrize("damage", ["garbage", "truncated", "hash"])
async def test_bad_state_file_starts_fresh(start_hub, tmp_path, damage):
    cache, state = tmp_path / "c.cache", tmp_path / "s.state"
    hub = start_hub("--cache", str(cache), "--state", str(state))
    async with aiohttp.ClientSession() as s, s.ws_connect(hub.url) as ws:
        await hello(ws)
        await cfg(ws, CONFIG)
        await start(ws, "Watch")
        await until(ws)
    hub.stop()
    raw = state.read_bytes()
    assert raw[:4] == b"HPST"
    if damage == "garbage":
        state.write_bytes(b"not a state file at all")
    elif damage == "truncated":
        state.write_bytes(raw[: len(raw) // 2])
    else:  # intact file, but the stored config has since changed (different config hash)
        hub_b = start_hub("--cache", str(cache), "--state", str(state))
        async with aiohttp.ClientSession() as s, s.ws_connect(hub_b.url) as ws:
            await hello(ws)
            await drain_devices(ws, 2)
            await cfg(ws, {**CONFIG, "keys": {}}, new_devices=0)
        hub_b.stop()
        state.write_bytes(raw)
    hub2 = start_hub("--cache", str(cache), "--state", str(state))
    assert hub2.alive()
    async with aiohttp.ClientSession() as s, s.ws_connect(hub2.url) as ws:
        await ws.send_str(json.dumps({"v": 1, "id": 1, "t": "hello", "d": {"token": "test-token"}}))
        assert (await recv(ws))["t"] == "hello_ack"
        su = await recv(ws)
        assert su["d"] == {"activity": 0, "power": False}
    hub2.stop()
    log = hub2.err_path.read_text()
    assert "starting all off/unknown" in log


async def test_key_starts_activity_through_state_machine(hub_rf, run_remote):
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_rf.url) as ws:
        await hello(ws)
        await cfg(ws, CONFIG)
        assert run_remote(hub_rf.rf, "--press", "1").returncode == 0
        msgs = await until(ws)
        assert msgs[0]["t"] == "key_event"
        assert msgs[-1]["d"] == {"activity": 0, "power": True}
        assert read_lines(hub_rf, 4) == [ir_line(c) for c in (TV_ON, AMP_TOGGLE, TV_H1, AMP_OPT)]
        assert run_remote(hub_rf.rf, "--press", "2").returncode == 0
        msgs = await until(ws)
        assert msgs[-1]["d"] == {"activity": 0, "power": False}
        assert read_lines(hub_rf, 2) == [ir_line(c) for c in (TV_OFF, AMP_TOGGLE)]


async def test_remote_hold_sends_press_holds_release(hub_rf, run_remote):
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_rf.url) as ws:
        await hello(ws)
        await cfg(ws, CONFIG)
        t0 = time.monotonic()
        res = run_remote(hub_rf.rf, "--hold", "6,600", "--hold-interval-ms", "200")
        assert res.returncode == 0, res.stderr
        assert time.monotonic() - t0 >= 0.55
        actions = []
        while len(actions) < 5:  # press, holds at 200/400, release (+ any extra hold)
            m = await recv(ws)
            assert m["t"] == "key_event" and m["d"]["key"] == 6
            actions.append(m["d"]["action"])
            if actions[-1] == 1:
                break
        assert actions[0] == 0 and actions[-1] == 1 and actions[1:-1] == [2] * (len(actions) - 2)
        assert len(actions) >= 4
        await asyncio.sleep(0.3)
        lines = read_lines(hub_rf, pending_lines(hub_rf))
        assert len(lines) >= 3 and " n=68 " in lines[0]
        assert all(" n=4 " in ln for ln in lines[1:])  # NEC repeat codes while held
        await asyncio.sleep(0.4)
        assert pending_lines(hub_rf) == 0  # released: no further repeats


async def test_hold_interval_default_is_400ms(built_binaries, hub_rf, run_remote):
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_rf.url) as ws:
        await hello(ws)
        res = run_remote(hub_rf.rf, "--hold", "6,850")
        assert res.returncode == 0, res.stderr
        acts = [(await recv(ws))["d"]["action"] for _ in range(4)]
        assert acts == [0, 2, 2, 1]  # holds at 400 and 800 ms, release at 850


async def test_rf_reattach_cycles_do_not_lose_the_next_remote(hub_rf):
    """Regression for the flaky partial-message test: a remote that vanishes with a queued
    successor must not get that successor refused as a 'second remote'."""
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_rf.url) as ws:
        await hello(ws)
        for i in range(25):
            a = rf_attach(hub_rf.rf)
            a.send(bytes([12, 1, 0x02, 0x01, 0]))  # fragment 0 of 2, then vanish
            a.close()
            b = rf_attach(hub_rf.rf)
            b.send(bytes([10, 1, 0x01, 3, 1, 2, 0]))
            assert (await recv(ws))["d"] == {"remote": 1, "key": 2, "action": 0}, i
            b.close()


async def test_device_command_tracks_state_and_skips_redundant_toggle(hub_process):
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_process.url) as ws:
        await hello(ws)
        await cfg(ws, CONFIG)

        async def dc(device, command, id=5):
            args = {"device": device, "command": command}
            msg = {"device": "hub", "command": "device_command", "args": args}
            return await call(ws, "command", msg, id=id)

        # toggle amp, tracked off: power_off sends nothing and is ok
        assert (await dc("amp", "power_off"))["d"] == {"ok": True}
        await asyncio.sleep(0.2)
        assert read_lines(hub_process, 0) == []
        assert (await dc("amp", "power_on"))["d"] == {"ok": True}
        assert (await recv(ws))["d"] == dev("amp", True)
        assert read_lines(hub_process, 1) == [ir_line(AMP_TOGGLE)]
        assert (await dc("amp", "power_on"))["d"] == {"ok": True}  # tracked on: no IR
        await asyncio.sleep(0.2)
        assert read_lines(hub_process, 0) == []
        # discrete tv: input implies on; unknown device/command are refused
        assert (await dc("tv", "input_hdmi2"))["d"] == {"ok": True}
        assert (await recv(ws))["d"] == dev("tv", True, "hdmi2")
        assert read_lines(hub_process, 1) == [ir_line(TV_H2)]
        for bad in (("ghost", "power_on"), ("tv", "nope"), ("tv", "input_vga")):
            r = (await dc(*bad))["d"]
            assert r["ok"] is False and r["message"], bad
        msg = {"device": "hub", "command": "device_command", "args": {}}
        assert (await call(ws, "command", msg, id=6))["d"]["ok"] is False


async def test_device_command_refused_during_transition(hub_process):
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_process.url) as ws:
        await hello(ws)
        await cfg(ws, CONFIG)
        await start(ws, "Watch")  # tv/amp settle times keep it running for a while
        args = {"device": "tv", "command": "power_off"}
        msg = {"device": "hub", "command": "device_command", "args": args}
        await ws.send_str(json.dumps({"v": 1, "id": 7, "t": "command", "d": msg}))
        while (r := await recv(ws))["id"] != 7:  # device_state pushes may come first
            pass
        assert r["d"] == {"ok": False, "message": "transition in progress"}
        await until(ws)


async def test_device_command_queue_full_is_not_reported_as_a_transition(hub_process):
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_process.url) as ws:
        await hello(ws)
        await cfg(ws, CONFIG)
        args = {"device": "tv", "command": "input_hdmi1"}  # discrete: always sent
        msg = {"device": "hub", "command": "device_command", "args": args}
        for i in range(10):  # no pause: the 3-slot deferred IR queue overflows
            await ws.send_str(json.dumps({"v": 1, "id": 100 + i, "t": "command", "d": msg}))
        replies = {}
        while len(replies) < 10:
            m = await recv(ws)
            if m["t"] == "command_result":
                replies[m["id"]] = m["d"]
        msgs = {r.get("message") for r in replies.values() if not r["ok"]}
        assert msgs == {"IR line busy, queue full"}, replies


async def test_set_config_reports_the_current_activity_to_every_client(hub_process):
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_process.url) as ws:
        await hello(ws)
        await cfg(ws, CONFIG)
        await start(ws, "Movie")
        await until(ws)
        read_lines(hub_process, 2)
        # rename the running activity (same id, same index)
        acts = [dict(a) for a in CONFIG["activities"]]
        acts[1]["name"] = "Cinema"
        r = await call(ws, "set_config", {"config": {**CONFIG, "activities": acts}}, id=4)
        assert r["t"] == "config_ack" and r["d"]["ok"] is True
        su = await until(ws)  # the state_update follows the ack, even to the sender
        assert su[-1]["d"] == {"activity": 1, "power": True}
