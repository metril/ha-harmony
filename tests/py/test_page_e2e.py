"""Pages over the simulated RF link: hub page_push -> remote_sim --dump-layout / --tap."""

import base64
import json
import socket
import subprocess

import aiohttp
import pytest
from conftest import RemoteDump
from test_hub_engine import call, hello, recv

from custom_components.ha_harmony.page_codec import Widget, encode_page
from proto import rf_codec

PAGE_PUSH, TILE_UPDATE = 12, 15
WIDGETS = [
    Widget("button", 2, 1, 0, 2, 1, "Mute"),
    Widget("tile", 3, 0, 1, 4, 2, "Temp"),
    Widget("label", 4, 0, 5, 4, 1, "é日"),
]
PAGE = encode_page(9, 4, 6, WIDGETS)
LAYOUT = {
    "remote": 1,
    "page": 9,
    "cols": 4,
    "rows": 6,
    "widgets": [
        {"kind": "button", "id": 2, "x": 1, "y": 0, "w": 2, "h": 1, "label": "Mute"},
        {"kind": "tile", "id": 3, "x": 0, "y": 1, "w": 4, "h": 2, "label": "Temp", "text": ""},
        {"kind": "label", "id": 4, "x": 0, "y": 5, "w": 4, "h": 1, "label": "é日"},
    ],
}


@pytest.fixture
def start_remote(built_binaries):
    procs: list[subprocess.Popen] = []

    def _start(rf: str, *args: str) -> RemoteDump:
        proc = subprocess.Popen(
            [str(built_binaries / "remote_sim"), "--rf", rf, "--timeout-ms", "5000", *args],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        procs.append(proc)
        return RemoteDump(proc)

    yield _start
    for p in procs:
        if p.poll() is None:
            p.kill()
        p.wait()
        p.stdout.close()
        p.stderr.close()


def finish(r: RemoteDump) -> tuple[int, str, str]:
    out, err = r.proc.communicate(timeout=10)
    return r.proc.returncode, out.decode(), err.decode()


async def push(ws, data: bytes, page=9, remote=1, id=5):
    r = await call(
        ws, "page_push", {"remote": remote, "page": page, "data": base64.b64encode(data).decode()}, id=id
    )
    assert r["t"] == "command_result" and r["d"]["ok"] is True


async def test_push_then_dump_layout(hub_rf, start_remote):
    r = start_remote(hub_rf.rf, "--dump-layout")
    r.wait_attached()
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_rf.url) as ws:
        await hello(ws)
        await push(ws, PAGE)
    code, out, _ = finish(r)
    assert code == 0 and json.loads(out) == LAYOUT


async def test_remote_attach_is_announced_to_ws_clients(hub_rf):
    from conftest import rf_attach

    async with aiohttp.ClientSession() as s, s.ws_connect(hub_rf.url) as ws:
        await hello(ws)
        sock = rf_attach(hub_rf.rf)
        msg = await recv(ws, keep_attached=True)
        assert msg["t"] == "remote_attached" and msg["d"] == {}
        sock.close()
        sock2 = rf_attach(hub_rf.rf)  # a re-attach is announced again
        assert (await recv(ws, keep_attached=True))["t"] == "remote_attached"
        sock2.close()


async def test_tap_button_sends_soft_key_press_release(hub_rf, start_remote):
    r = start_remote(hub_rf.rf, "--tap", "70,42")  # row 0 starts below the 32 px status bar
    r.wait_attached()
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_rf.url) as ws:
        await hello(ws)
        await push(ws, PAGE)
        code, out, _ = finish(r)
        assert code == 0 and json.loads(out) == {"hit": True, "id": 2, "key": 130}
        assert (await recv(ws))["d"] == {"remote": 1, "key": 130, "action": 0}
        assert (await recv(ws))["d"] == {"remote": 1, "key": 130, "action": 1}


async def test_tap_miss_and_label_send_nothing(hub_rf, start_remote, run_remote):
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_rf.url) as ws:
        await hello(ws)
        for xy, what in (("10,10", "nothing"), ("10,300", "a label")):  # (10,10) is the status bar
            r = start_remote(hub_rf.rf, "--tap", xy)
            r.wait_attached()
            await push(ws, PAGE)
            code, out, err = finish(r)
            assert code == 0 and json.loads(out) == {"hit": False}
            assert f"hit {what}" in err and "no key_event sent" in err
        # the first key_event the hub sees is this later press, so the misses sent none
        assert run_remote(hub_rf.rf, "--press", "5").returncode == 0
        assert (await recv(ws))["d"]["key"] == 5


async def test_rejected_page_is_ignored(hub_rf, start_remote):
    r = start_remote(hub_rf.rf, "--dump-layout")
    r.wait_attached()
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_rf.url) as ws:
        await hello(ws)
        await push(ws, PAGE[:-1], page=9)  # truncated: remote refuses it
        await push(ws, PAGE, page=9, id=6)
    code, out, err = finish(r)
    assert code == 0 and json.loads(out) == LAYOUT and "rejected page_push" in err


async def test_layout_cli_errors(hub_rf, run_remote):
    for args in (
        ["--tap", "5"],
        ["--tap", "240,5"],
        ["--tap", "5,320"],
        ["--tap", "a,b"],
        ["--tap", "1,2", "--dump-layout"],
        ["--dump-layout", "--press", "1"],
        ["--dump-layout", "--settle-ms", "x"],
    ):
        assert run_remote(hub_rf.rf, *args).returncode == 2, args
    res = run_remote(hub_rf.rf, "--dump-layout", "--timeout-ms", "300")
    assert res.returncode == 3 and "timed out" in res.stderr


# ---- tile_update: the hub has no WS command for it yet, so a fake hub end plays the hub.


def tile_update(remote, page, tile, text: bytes) -> bytes:
    return bytes([remote, page, tile, len(text)]) + text


def test_tile_update_shows_in_layout(rf_path, start_remote):
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    srv.bind(rf_path)
    srv.listen(1)
    srv.settimeout(5)
    r = start_remote(rf_path, "--dump-layout", "--settle-ms", "300")
    conn, _ = srv.accept()
    conn.send(b"A")
    seq = iter(range(1, 100))

    def send(mtype, payload):
        for fr in rf_codec.split(mtype, next(seq), payload):
            conn.send(fr)

    send(PAGE_PUSH, bytes([1, 9, len(PAGE)]) + PAGE)
    send(TILE_UPDATE, tile_update(1, 9, 3, "21.5°C".encode()))
    send(TILE_UPDATE, tile_update(1, 9, 2, b"button"))  # not a tile: ignored
    send(TILE_UPDATE, tile_update(1, 8, 3, b"other page"))  # unknown page: ignored
    send(TILE_UPDATE, tile_update(1, 9, 3, b"x" * 40))  # truncated to 31 bytes
    send(TILE_UPDATE, tile_update(1, 9, 3, "é".encode() * 20))  # truncated to 15 chars
    code, out, err = finish(r)
    conn.close()
    srv.close()
    assert code == 0, err
    tile = json.loads(out)["widgets"][1]
    assert tile["text"] == "é" * 15
    assert err.count("ignored tile_update") == 1 and "unknown page 8" in err


async def test_tile_update_via_hub_command(hub_rf, start_remote):
    """Real hub: command hub/tile_update is forwarded over RF and lands on the remote's tile."""
    r = start_remote(hub_rf.rf, "--dump-layout", "--settle-ms", "400")
    r.wait_attached()
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_rf.url) as ws:
        await hello(ws)
        await push(ws, PAGE)
        args = {"remote": 1, "page": 9, "tile": 3, "text": "21.5\u00b0C"}
        res = await call(ws, "command", {"device": "hub", "command": "tile_update", "args": args}, id=6)
        assert res["t"] == "command_result" and res["d"]["ok"] is True and res["id"] == 6
    code, out, err = finish(r)
    assert code == 0, err
    assert json.loads(out)["widgets"][1]["text"] == "21.5\u00b0C"
