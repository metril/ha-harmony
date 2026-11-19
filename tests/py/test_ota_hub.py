"""Hub daemon OTA over ws: upload, resume, busy, rejections, restart exit code, rollback report."""

import base64
import hashlib
import json
import os

import aiohttp
import pytest

TOKEN = "test-token"
CHUNK = 4096
EXIT_OTA_RESTART = 42
OLD = b"#!/bin/sh\necho old\n"


def env(t, d=None, id=1):
    return json.dumps({"v": 1, "id": id, "t": t, "d": d if d is not None else {}})


async def connect(session, hub):
    ws = await session.ws_connect(hub.url)
    await ws.send_str(env("hello", {"token": TOKEN}))
    assert (await ws.receive_json(timeout=5))["t"] == "hello_ack"
    assert (await ws.receive_json(timeout=5))["t"] == "state_update"
    return ws


async def _hello_ack(session, hub):
    ws = await session.ws_connect(hub.url)
    await ws.send_str(env("hello", {"token": TOKEN}))
    ack = (await ws.receive_json(timeout=5))["d"]
    await ws.close()
    return ack


async def call(ws, t, d=None, id=2):
    await ws.send_str(env(t, d, id))
    msg = await ws.receive_json(timeout=5)
    assert msg["id"] == id
    return msg


def begin(img: bytes, build: int) -> dict:
    return {"target": 1, "size": len(img), "build": build, "sha": hashlib.sha512(img).hexdigest()}


async def upload(ws, img: bytes, start: int = 0, stop: int | None = None) -> None:
    end = len(img) if stop is None else stop
    for off in range(start, end, CHUNK):
        part = img[off : min(off + CHUNK, end)]
        r = await call(ws, "ota_chunk", {"offset": off, "data": base64.b64encode(part).decode()}, id=100 + off)
        assert r["t"] == "ota_status" and r["d"]["state"] == "receiving", r


@pytest.fixture
def ota_hub(start_hub, tmp_path):
    d = tmp_path / "otadir"
    d.mkdir()
    hubfile = d / "hub"
    hubfile.write_bytes(OLD)
    hubfile.chmod(0o755)

    def _start(*extra: str):
        h = start_hub("--ota-dir", str(d), "--ota-target", str(hubfile), "--ota-allow-dev", *extra)
        h.dir = d
        return h

    return _start


async def test_good_update_applies_then_exits_for_the_supervisor(ota_hub, sign_image):
    payload = os.urandom(20000)
    img = sign_image(payload, 5)
    hub = ota_hub()
    async with aiohttp.ClientSession() as s:
        ws = await connect(s, hub)
        r = await call(ws, "ota_begin", begin(img, 5))
        assert r["d"] == {"state": "receiving", "offset": 0, "size": len(img)}
        await upload(ws, img)
        r = await call(ws, "ota_end", id=3)
        assert r["t"] == "ota_status" and r["d"]["state"] == "applied" and r["d"]["build"] == 5
    # the daemon never execs itself: it exits with the dedicated code for the boot hook
    assert hub.proc.wait(timeout=10) == EXIT_OTA_RESTART
    assert (hub.dir / "hub").read_bytes() == payload
    assert (hub.dir / "hub.prev").read_bytes() == OLD
    assert (hub.dir / "ota-restart").exists() and (hub.dir / "ota-pending").read_text() == "build=5\nprev=0\n"
    assert not (hub.dir / "ota-staged.img").exists()


async def test_bad_signature_is_rejected_and_hub_keeps_running(ota_hub, sign_image):
    img = bytearray(sign_image(os.urandom(3000), 5))
    img[100] ^= 1  # inside the Ed25519 signature
    img = bytes(img)
    hub = ota_hub()
    async with aiohttp.ClientSession() as s:
        ws = await connect(s, hub)
        await call(ws, "ota_begin", begin(img, 5))
        await upload(ws, img)
        r = await call(ws, "ota_end", id=3)
        assert r["t"] == "error" and r["d"]["code"] == "ota_verify" and "signature" in r["d"]["message"]
        r = await call(ws, "ping", id=4)  # still serving
        assert r["t"] == "pong"
    assert hub.alive()
    assert (hub.dir / "hub").read_bytes() == OLD
    assert not (hub.dir / "hub.prev").exists() and not (hub.dir / "ota-staged.img").exists()
    assert not (hub.dir / "ota-pending").exists()


async def test_truncated_image_is_rejected_then_resumed_by_offset(ota_hub, sign_image):
    payload = os.urandom(15000)
    img = sign_image(payload, 8)
    hub = ota_hub()
    cut = 2 * CHUNK
    async with aiohttp.ClientSession() as s:
        ws = await connect(s, hub)
        await call(ws, "ota_begin", begin(img, 8))
        await upload(ws, img, 0, cut)
        r = await call(ws, "ota_end", id=3)
        assert r["t"] == "error" and r["d"]["code"] == "ota_size"
        assert (hub.dir / "hub").read_bytes() == OLD
        await ws.close()
        # a new connection resumes at the hub's offset
        ws2 = await connect(s, hub)
        r = await call(ws2, "ota_begin", begin(img, 8))
        assert r["d"]["state"] == "receiving" and r["d"]["offset"] == cut
        # a gap is refused; the right offset goes on
        gap = await call(ws2, "ota_chunk", {"offset": cut + 1, "data": base64.b64encode(b"x").decode()}, id=5)
        assert gap["d"]["code"] == "ota_offset"
        await upload(ws2, img, cut)
        r = await call(ws2, "ota_end", id=6)
        assert r["d"]["state"] == "applied"
    assert hub.proc.wait(timeout=10) == EXIT_OTA_RESTART
    assert (hub.dir / "hub").read_bytes() == payload


async def test_a_second_upload_is_rejected_while_one_is_in_flight(ota_hub, sign_image):
    img = sign_image(os.urandom(6000), 2)
    hub = ota_hub()
    async with aiohttp.ClientSession() as s:
        a, b = await connect(s, hub), await connect(s, hub)
        await call(a, "ota_begin", begin(img, 2))
        r = await call(b, "ota_begin", begin(img, 2))
        assert r["t"] == "error" and r["d"]["code"] == "ota_busy"
        r = await call(b, "ota_abort", id=3)
        assert r["d"]["code"] == "ota_busy"
        r = await call(b, "ota_chunk", {"offset": 0, "data": "AAAA"}, id=4)
        assert r["d"]["code"] == "ota_state"
        # the owner aborts; now the other connection may start
        r = await call(a, "ota_abort", id=5)
        assert r["d"]["state"] == "aborted"
        r = await call(b, "ota_begin", begin(img, 2))
        assert r["d"]["state"] == "receiving"
    assert not (hub.dir / "hub.prev").exists()


async def test_request_validation_errors(ota_hub, sign_image):
    img = sign_image(os.urandom(500), 1)
    hub = ota_hub()
    async with aiohttp.ClientSession() as s:
        ws = await connect(s, hub)
        bad = {**begin(img, 1), "target": 2}
        assert (await call(ws, "ota_begin", bad))["d"]["code"] == "ota_target"
        bad = {**begin(img, 1), "size": 10}
        assert (await call(ws, "ota_begin", bad))["d"]["code"] == "ota_size"
        bad = {**begin(img, 1), "sha": "zz"}
        assert (await call(ws, "ota_begin", bad))["d"]["code"] == "ota_bad_request"
        await call(ws, "ota_begin", begin(img, 1))
        big = base64.b64encode(os.urandom(CHUNK + 1)).decode()
        assert (await call(ws, "ota_chunk", {"offset": 0, "data": big}, id=9))["d"]["code"] == "ota_chunk"
        assert (await call(ws, "ota_end", id=10))["d"]["code"] == "ota_size"  # nothing received
        # a chunk of a corrupted transfer: the digest check names it
        good = img[:-1] + bytes([img[-1] ^ 1])
        await upload(ws, good)
        assert (await call(ws, "ota_end", id=11))["d"]["code"] == "ota_sha"
    assert (hub.dir / "hub").read_bytes() == OLD


async def test_downgrade_is_refused_at_begin(ota_hub, sign_image):
    hub_dir_build = 9
    img_new = sign_image(os.urandom(800), hub_dir_build)
    img_old = sign_image(os.urandom(800), 3)
    hub = ota_hub()
    async with aiohttp.ClientSession() as s:
        ws = await connect(s, hub)
        await call(ws, "ota_begin", begin(img_new, hub_dir_build))
        await upload(ws, img_new)
        assert (await call(ws, "ota_end", id=3))["d"]["state"] == "applied"
    assert hub.proc.wait(timeout=10) == EXIT_OTA_RESTART
    # the next process (what the hook launches) knows the floor from ota-build; the unproven update blocks
    # a new upload until the hook's health mark removes the marker
    hub_blocked = ota_hub()
    async with aiohttp.ClientSession() as s:
        ws = await connect(s, hub_blocked)
        assert (await call(ws, "ota_begin", begin(img_old, 3)))["d"]["code"] == "ota_not_ready"
    hub_blocked.stop()
    (hub.dir / "ota-pending").unlink()
    hub2 = ota_hub()
    async with aiohttp.ClientSession() as s:
        ws = await connect(s, hub2)
        assert (await call(ws, "ota_begin", begin(img_old, 3)))["d"]["code"] == "ota_downgrade"
        assert (await call(ws, "ota_begin", begin(img_new, hub_dir_build)))["d"]["code"] == "ota_downgrade"  # same build
        ack = await _hello_ack(s, hub2)
        assert ack["fw_build"] == hub_dir_build and ack["ota_key"] == "dev"


async def test_updates_are_off_without_an_ota_dir(hub_process):
    async with aiohttp.ClientSession() as s:
        ws = await connect(s, hub_process)
        r = await call(ws, "ota_begin", {"target": 1, "size": 200, "build": 1, "sha": "0" * 128})
        assert r["t"] == "error" and r["d"]["code"] == "ota_unsupported"


async def test_rollback_record_is_pushed_to_each_new_client(start_hub, tmp_path):
    d = tmp_path / "od"
    d.mkdir()
    (d / "ota-rollback").write_text("build=7\nreason=died_early\nat=up 90s\n")
    hub = start_hub("--ota-dir", str(d))
    async with aiohttp.ClientSession() as s:
        ws = await s.ws_connect(hub.url)
        await ws.send_str(env("hello", {"token": TOKEN}))
        assert (await ws.receive_json(timeout=5))["t"] == "hello_ack"
        assert (await ws.receive_json(timeout=5))["t"] == "state_update"
        st = await ws.receive_json(timeout=5)
        assert st["t"] == "ota_status"
        assert st["d"] == {"state": "rolled_back", "offset": 0, "size": 0, "build": 7, "reason": "died_early"}


async def test_dev_key_hub_refuses_updates_without_the_explicit_flag(start_hub, tmp_path, sign_image):
    d = tmp_path / "devdir"
    d.mkdir()
    hub = start_hub("--ota-dir", str(d))  # this build carries the public dev key
    img = sign_image(b"x" * 100, 2)
    async with aiohttp.ClientSession() as s:
        ack = await _hello_ack(s, hub)
        assert ack["ota_key"] == "dev"
        ws = await connect(s, hub)
        r = await call(ws, "ota_begin", begin(img, 2))
        assert r["t"] == "error" and r["d"]["code"] == "ota_dev_key"


async def test_no_update_before_the_hooks_health_mark(start_hub, tmp_path, sign_image):
    d = tmp_path / "hdir"
    d.mkdir()
    mark = d / "healthy"
    hub = start_hub("--ota-dir", str(d), "--ota-allow-dev", "--health-file", str(mark))
    img = sign_image(b"x" * 100, 2)
    async with aiohttp.ClientSession() as s:
        ws = await connect(s, hub)
        assert (await call(ws, "ota_begin", begin(img, 2)))["d"]["code"] == "ota_not_ready"
        mark.write_text("")  # the hook's health mark
        assert (await call(ws, "ota_begin", begin(img, 2), id=3))["d"]["state"] == "receiving"


async def test_hello_ack_has_no_ota_fields_when_updates_are_off(hub_process):
    async with aiohttp.ClientSession() as s:
        ack = await _hello_ack(s, hub_process)
        assert "ota_key" not in ack and "fw_build" not in ack
