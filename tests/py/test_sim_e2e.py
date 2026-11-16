"""End to end over the simulated RF link: hub --sim --rf <path> <-> remote_sim <-> WebSocket client."""

import base64

import aiohttp
from conftest import rf_attach
from test_hub_engine import CONFIG, call, hello, read_lines, recv

from proto import rf_codec

KEY_EVENT, STATE_UPDATE, PAGE_PUSH, BATTERY = 10, 11, 12, 13


async def start_tv(ws):
    await call(ws, "set_config", {"config": CONFIG}, id=2)
    await call(
        ws, "command", {"device": "hub", "command": "start_activity", "args": {"name": "TV"}}, id=3
    )
    assert (await recv(ws))["t"] == "state_update"


async def test_mapped_key_hits_ir_sink_and_ws(hub_rf, run_remote):
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_rf.url) as ws:
        await hello(ws)
        await start_tv(ws)
        read_lines(hub_rf, 4)  # activity steps: ir, bt down/up, ir
        res = run_remote(hub_rf.rf, "--press", "2", "--remote", "1")
        assert res.returncode == 0, res.stderr
        ke = await recv(ws)
        assert ke["t"] == "key_event" and ke["d"] == {"remote": 1, "key": 2, "action": 0}
        assert read_lines(hub_rf, 1) == ["ir port=3 n=4 pronto=0000 006d 0000 0000"]


async def test_unmapped_key_emits_only_key_event(hub_rf, run_remote):
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_rf.url) as ws:
        await hello(ws)
        await start_tv(ws)
        read_lines(hub_rf, 4)
        assert run_remote(hub_rf.rf, "--press", "99").returncode == 0
        ke = await recv(ws)
        assert ke["d"] == {"remote": 1, "key": 99, "action": 0}
        # no sink call for key 99: the next sink line belongs to the following (mapped) press
        assert run_remote(hub_rf.rf, "--press", "2").returncode == 0
        assert (await recv(ws))["d"]["key"] == 2
        assert read_lines(hub_rf, 1) == ["ir port=3 n=4 pronto=0000 006d 0000 0000"]


async def test_page_push_roundtrip(hub_rf, start_remote_dump):
    data = bytes(range(256))[:255]
    remote = start_remote_dump(hub_rf.rf)
    remote.wait_attached()
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_rf.url) as ws:
        await hello(ws)
        r = await call(
            ws,
            "page_push",
            {"remote": 3, "page": 7, "data": base64.b64encode(data).decode()},
            id=5,
        )
        assert r["t"] == "command_result" and r["id"] == 5 and r["d"]["ok"] is True
    code, out = remote.result()
    assert code == 0
    import json

    assert json.loads(out) == {"remote": 3, "page": 7, "data": base64.b64encode(data).decode()}


async def test_page_too_big_is_clean_error(hub_rf, start_remote_dump):
    remote = start_remote_dump(hub_rf.rf, timeout_ms=600)
    remote.wait_attached()
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_rf.url) as ws:
        await hello(ws)
        big = base64.b64encode(bytes(300)).decode()
        r = await call(ws, "page_push", {"remote": 1, "page": 1, "data": big}, id=6)
        assert r["t"] == "error" and r["d"]["code"] == "too_big" and r["id"] == 6
    code, out = remote.result()
    assert code == 3 and out == ""  # nothing, not even a truncated page, reached the remote


async def test_page_push_without_remote(hub_rf):
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_rf.url) as ws:
        await hello(ws)
        r = await call(ws, "page_push", {"remote": 1, "page": 1, "data": "AAAA"}, id=7)
        assert r["t"] == "error" and r["d"]["code"] == "no_remote"


async def test_page_push_without_rf_socket(hub_process):
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_process.url) as ws:
        await hello(ws)
        r = await call(ws, "page_push", {"remote": 1, "page": 1, "data": "AAAA"}, id=7)
        assert r["d"]["code"] == "no_remote"


async def test_second_remote_refused(hub_rf, run_remote, start_remote_dump):
    first = start_remote_dump(hub_rf.rf, timeout_ms=1500)
    first.wait_attached()
    res = run_remote(hub_rf.rf, "--press", "1")
    assert res.returncode == 1 and "another remote is attached" in res.stderr
    assert first.result()[0] == 3  # first remote unaffected (timed out waiting)
    # once the first is gone a new remote can attach
    assert run_remote(hub_rf.rf, "--press", "1").returncode == 0


async def test_remote_cli_errors(hub_rf, run_remote, tmp_path):
    assert run_remote(hub_rf.rf).returncode == 2
    assert run_remote(hub_rf.rf, "--press", "300").returncode == 2
    assert run_remote(hub_rf.rf, "--press", "1", "--dump-page").returncode == 2
    res = run_remote(str(tmp_path / "nope"), "--press", "1")
    assert res.returncode == 1 and "cannot connect" in res.stderr


async def test_reconnect_and_garbage_frames(hub_rf, run_remote):
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_rf.url) as ws:
        await hello(ws)
        sock = rf_attach(hub_rf.rf)
        for junk in (
            b"",
            b"\x01",
            b"\x0a\x00\x00",
            b"\x0a\x01\x1f\xff" + b"x" * 28,
            b"\xff" * 32,
            b"\x0a\x01\x10\x03abc",
        ):
            sock.send(junk)
        # a valid fragmented-then-complete key_event still gets through
        for fr in rf_codec.split(KEY_EVENT, 9, bytes([4, 55, 1]), 32):
            sock.send(fr)
        ke = await recv(ws)
        assert ke["d"] == {"remote": 4, "key": 55, "action": 1}
        sock.close()
        assert hub_rf.alive()
        # reconnect through the CLI
        assert run_remote(hub_rf.rf, "--press", "5").returncode == 0
        assert (await recv(ws))["d"]["key"] == 5
        assert hub_rf.alive()


async def test_partial_message_dropped_on_disconnect(hub_rf):
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_rf.url) as ws:
        await hello(ws)
        a = rf_attach(hub_rf.rf)
        a.send(bytes([PAGE_PUSH, 1, 0x02, 0x01, 0]))  # fragment 0 of 2, then vanish
        a.close()
        b = rf_attach(hub_rf.rf)
        b.send(bytes([KEY_EVENT, 1, 0x01, 3, 1, 2, 0]))
        assert (await recv(ws))["d"] == {"remote": 1, "key": 2, "action": 0}


async def test_state_update_forwarded_and_battery_relayed(hub_rf):
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_rf.url) as ws:
        await hello(ws)
        sock = rf_attach(hub_rf.rf)
        await call(ws, "set_config", {"config": CONFIG}, id=2)
        await call(
            ws,
            "command",
            {"device": "hub", "command": "start_activity", "args": {"name": "Music"}},
            id=3,
        )
        assert (await recv(ws))["t"] == "state_update"
        frame = sock.recv(64)
        assert len(frame) <= 30
        asm = rf_codec.Reassembler()
        assert asm.push(frame) == (STATE_UPDATE, bytes([1, 1]))
        for fr in rf_codec.split(BATTERY, 2, bytes([1, 77, 0x0E, 0xD8]), 32):
            sock.send(fr)
        b = await recv(ws)
        assert b["t"] == "battery" and b["d"] == {"remote": 1, "percent": 77, "millivolts": 3800}
