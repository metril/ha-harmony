import asyncio
import json
import os
import re
import signal
import socket
import struct
import subprocess
import time
from pathlib import Path

import aiohttp
import pytest

TOKEN = "test-token"  # matches conftest default


def env(t, d=None, id=1, v=1):
    return json.dumps({"v": v, "id": id, "t": t, "d": d if d is not None else {}})


async def hello(ws, token=TOKEN, id=1):
    await ws.send_str(env("hello", {"token": token, "client": "pytest"}, id=id))
    ack = json.loads((await ws.receive(timeout=5)).data)
    if ack["t"] == "hello_ack":  # the hub pushes the live state right after the ack
        su = json.loads((await ws.receive(timeout=5)).data)
        assert su["t"] == "state_update"
    return ack


async def test_hello_pushes_live_state_right_after_ack(hub_process):
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_process.url) as ws:
        await ws.send_str(env("hello", {"token": TOKEN}, id=4))
        ack = json.loads((await ws.receive(timeout=5)).data)
        su = json.loads((await ws.receive(timeout=5)).data)
        assert ack["t"] == "hello_ack" and ack["id"] == 4
        assert su == {"v": 1, "id": 0, "t": "state_update", "d": {"activity": 0, "power": False}}


async def test_hello_ack_and_ping(hub_process):
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_process.url) as ws:
        ack = await hello(ws)
        assert ack["t"] == "hello_ack" and ack["id"] == 1 and ack["d"]["hub_id"]
        await ws.send_str(env("ping", id=2))
        pong = json.loads((await ws.receive(timeout=5)).data)
        assert pong["t"] == "pong" and pong["id"] == 2


async def test_bad_token_rejected_and_closed(hub_process):
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_process.url) as ws:
        err = await hello(ws, token="nope", id=9)
        assert err["t"] == "error" and err["d"]["code"] == "unauthorized" and err["id"] == 9
        msg = await ws.receive(timeout=5)
        assert msg.type in (aiohttp.WSMsgType.CLOSE, aiohttp.WSMsgType.CLOSED)
    assert hub_process.alive()


async def test_message_before_hello_unauthorized(hub_process):
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_process.url) as ws:
        await ws.send_str(env("ping", id=4))
        err = json.loads((await ws.receive(timeout=5)).data)
        assert err["d"]["code"] == "unauthorized" and err["id"] == 4
        assert (await ws.receive(timeout=5)).type in (
            aiohttp.WSMsgType.CLOSE,
            aiohttp.WSMsgType.CLOSED,
        )


async def test_protocol_errors_keep_connection(hub_process):
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_process.url) as ws:
        await hello(ws)
        cases = [
            (env("bogus", id=5), "unknown_type", 5),
            (env("ping", id=6, v=2), "version", 6),
            (env("set_config", id=7), "bad_request", 7),
            (env("key_event", {"remote": "x", "key": 1, "action": 0}, id=8), "bad_request", 8),
            ("not json", "bad_request", 0),
            (json.dumps([1, 2]), "bad_request", 0),
        ]
        for text, code, rid in cases:
            await ws.send_str(text)
            err = json.loads((await ws.receive(timeout=5)).data)
            assert (err["t"], err["d"]["code"], err["id"]) == ("error", code, rid), text
        await ws.send_str(env("ping", id=99))  # still usable
        assert json.loads((await ws.receive(timeout=5)).data)["t"] == "pong"


async def test_invalid_utf8_text_closes_1007(hub_process):
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_process.url) as ws:
        await hello(ws)
        # aiohttp can only send valid text; send raw bytes as a text frame via the writer.
        await ws._writer.send_frame(b"\xc3\x28", 0x1)
        msg = await ws.receive(timeout=5)
        assert msg.type == aiohttp.WSMsgType.CLOSE and msg.data == 1007
    assert hub_process.alive()


async def test_oversized_frame_closes_without_crash(hub_process):
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_process.url, max_msg_size=0) as ws:
        await hello(ws)
        await ws.send_str("x" * (64 * 1024 + 1))
        msg = await ws.receive(timeout=5)
        assert msg.type == aiohttp.WSMsgType.CLOSE and msg.data == 1009
    assert hub_process.alive()
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_process.url) as ws:
        assert (await hello(ws))["t"] == "hello_ack"


def _raw_upgrade(port):
    sock = socket.create_connection(("127.0.0.1", port), timeout=5)
    sock.sendall(
        b"GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        b"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n"
    )
    buf = b""
    while b"\r\n\r\n" not in buf:
        buf += sock.recv(4096)
    assert buf.startswith(b"HTTP/1.1 101") and b"s3pPLMBiTxaQ9kYGzzhZRbK+xOo=" in buf
    return sock


def test_raw_oversized_length_header_only(hub_process):
    sock = _raw_upgrade(hub_process.port)
    # 64-bit length of 2**40, header only: must be refused without waiting for payload.
    sock.sendall(bytes([0x81, 0xFF]) + struct.pack(">Q", 2**40) + b"\0\0\0\0")
    data = sock.recv(64)
    assert data[0] == 0x88 and struct.unpack(">H", data[2:4])[0] == 1009
    assert sock.recv(64) == b""  # server closed
    assert hub_process.alive()


def test_garbage_http_and_slow_handshake(hub_process):
    slow = socket.create_connection(("127.0.0.1", hub_process.port), timeout=5)
    slow.sendall(b"GET / HT")  # never completes
    junk = socket.create_connection(("127.0.0.1", hub_process.port), timeout=5)
    junk.sendall(b"POST /x HTTP/1.1\r\n\r\n")
    assert junk.recv(64).startswith(b"HTTP/1.1 400")
    sock = _raw_upgrade(hub_process.port)  # not blocked by the stalled client
    sock.close()
    slow.close()
    junk.close()
    assert hub_process.alive()


def test_sigterm_clean_exit(hub_process):
    hub_process.proc.send_signal(signal.SIGTERM)
    assert hub_process.proc.wait(timeout=5) == 0


async def test_concurrent_clients(hub_process):
    async with aiohttp.ClientSession() as s:
        wss = [await s.ws_connect(hub_process.url) for _ in range(5)]
        acks = await asyncio.gather(*(hello(w, id=i + 1) for i, w in enumerate(wss)))
        assert [a["id"] for a in acks] == [1, 2, 3, 4, 5]
        for w in wss:
            await w.close()


def test_real_backend_without_i2s_exits_nonzero(built_binaries):
    # No /dev/i2s on a dev host (or it is held by stock hal): the daemon must refuse to start.
    res = subprocess.run(
        [str(built_binaries / "hub"), "--token", "t", "--no-mdns"],
        check=False, capture_output=True, text=True, timeout=5,
    )
    assert res.returncode != 0 and "/dev/i2s" in res.stderr


def test_real_backend_requires_the_i2s_fd_from_the_boot_hook(built_binaries):
    # The daemon never opens /dev/i2s itself (a close of it freezes the hub): no --i2s-fd, no hardware.
    res = subprocess.run(
        [str(built_binaries / "hub"), "--token", "t", "--no-mdns", "--port", "0"],
        check=False, capture_output=True, text=True, timeout=5,
    )
    assert res.returncode == 1 and "--i2s-fd" in res.stderr


@pytest.mark.parametrize("fd", ["2", "1024", "x"])
def test_i2s_fd_is_range_checked(built_binaries, fd):
    res = subprocess.run(
        [str(built_binaries / "hub"), "--token", "t", "--no-mdns", "--i2s-fd", fd],
        check=False, capture_output=True, text=True, timeout=5,
    )
    assert res.returncode != 0 and "--i2s-fd" in res.stderr


def _hub_with_fd9(built_binaries, src_fd, *args, **kw):
    """Start the hub with src_fd as its fd 9, set up in the child only (never clobbers the test runner's fds)."""
    # bash does the dup2 (dash cannot name fds above 9) in the child (preexec_fn is unsafe with threads): `9>&N` then exec the hub
    return subprocess.Popen(
        ["bash", "-c", f'exec "$0" "$@" 9>&{src_fd}', str(built_binaries / "hub"),
         "--token", "t", "--no-mdns", "--i2s-fd", "9", *args],
        pass_fds=(src_fd,), text=True, **kw,
    )  # fmt: skip


def test_i2s_fd_must_be_a_writable_char_device(built_binaries, tmp_path):
    plain = os.open(tmp_path / "f", os.O_WRONLY | os.O_CREAT)  # a regular file, not a device
    ro = os.open("/dev/null", os.O_RDONLY)
    try:
        for src in (plain, ro):
            p = _hub_with_fd9(built_binaries, src, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            err = p.communicate(timeout=5)[1]
            assert p.returncode == 1 and "writable character device" in err
    finally:
        os.close(plain)
        os.close(ro)


def test_hub_starts_with_a_held_i2s_fd_and_never_closes_it(built_binaries, tmp_path):
    src = os.open("/dev/null", os.O_WRONLY)  # stands in for the hook's `9>/dev/i2s`
    try:
        proc = _hub_with_fd9(
            built_binaries, src, "--port", "0", "--cache", str(tmp_path / "c.json"),
            "--state", str(tmp_path / "s.json"), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )  # fmt: skip
    finally:
        os.close(src)
    try:
        time.sleep(1.0)
        assert proc.poll() is None, "the hub must start with the adopted fd"
        info = Path(f"/proc/{proc.pid}/fdinfo/9").read_text()  # close-on-exec: bt-helper must not inherit it
        assert int(re.search(r"flags:\s*(\d+)", info).group(1), 8) & 0o2000000
    finally:
        proc.terminate()
        proc.wait(timeout=5)


def test_hub_sources_never_open_or_close_the_i2s_fd():
    """No open()/close() of the i2s fd anywhere in hub/: the boot hook is its only opener."""
    root = Path(__file__).resolve().parents[2] / "hub"
    bad = re.compile(r"\bopen\s*\([^)]*i2s|\bclose\s*\([^)]*(i2s|dev_fd|\bh->fd)|HP_HW_DEFAULT_DEV")
    for src in [*root.rglob("*.c"), *root.rglob("*.h")]:
        code = re.sub(r"/\*.*?\*/|//[^\n]*", "", src.read_text(), flags=re.DOTALL)
        if src.name.startswith("hw_sinks") and re.search(r"ops\.(open|close)|\(\*(open|close)\)", code):
            raise AssertionError(f"{src}: the hw ops table must have no open/close")
        for m in bad.finditer(code):
            line = code[: m.start()].count("\n")
            assert False, f"{src}:{line}: {m.group(0)!r} opens or closes the /dev/i2s fd"


def test_rf_debug_flag_parses(built_binaries):
    # --rf-debug is a valueless flag: parsing succeeds (exit is the missing /dev/i2s, never usage code 2).
    res = subprocess.run(
        [str(built_binaries / "hub"), "--token", "t", "--no-mdns", "--rf-spi", "--rf-slot", "2", "--rf-debug"],
        check=False, capture_output=True, text=True, timeout=5,
    )
    assert res.returncode not in (0, 2) and "usage" not in res.stderr


@pytest.mark.parametrize(
    ("flag", "val"),
    [
        ("--port", "abc"),
        ("--port", "70000"),
        ("--port", "-1"),
        ("--port", "80x"),
        ("--auth-timeout-ms", "abc"),
        ("--idle-timeout-ms", "0"),
        ("--idle-timeout-ms", "9999999999"),
        ("--rf-slot", "0"),
        ("--rf-slot", "16"),
        ("--rf-slot", "x"),
    ],
)
def test_bad_numeric_flag_exits_2(built_binaries, flag, val):
    args = [str(built_binaries / "hub"), "--sim", "--token", "t", flag, val]
    res = subprocess.run(args, check=False, capture_output=True, text=True, timeout=5)
    assert res.returncode == 2 and flag in res.stderr and val in res.stderr


def test_heartbeat_file_tracks_monotonic_uptime(start_hub, tmp_path):
    # The boot hook's wedge watcher compares this to /proc/uptime: written at startup, refreshed.
    # The deadline is generous and reads tolerate a half-written file: under a loaded -n auto run the 1 s
    # refresh can be late by seconds, which made a 4 s window flaky.
    import time

    hb = tmp_path / "hb"
    start_hub("--heartbeat", str(hb), "--heartbeat-secs", "1")

    def read() -> int | None:
        try:
            return int(hb.read_text())
        except (ValueError, OSError):
            return None

    deadline = time.monotonic() + 15
    first = read()
    while first is None and time.monotonic() < deadline:
        time.sleep(0.05)
        first = read()
    assert first is not None and first >= 0
    now = first
    while time.monotonic() < deadline and now <= first:
        time.sleep(0.1)
        now = read() or now
    assert now > first, "heartbeat was not refreshed"
    up = float(Path("/proc/uptime").read_text().split()[0])
    assert abs(up - now) < 3600 * 24  # same scale (seconds), not epoch
    assert now < 10**8  # not a wall-clock epoch


def test_heartbeat_secs_is_validated(built_binaries):
    res = subprocess.run(
        [str(built_binaries / "hub"), "--sim", "--token", "t", "--heartbeat-secs", "0"],
        check=False, capture_output=True, text=True, timeout=5,
    )
    assert res.returncode == 2
