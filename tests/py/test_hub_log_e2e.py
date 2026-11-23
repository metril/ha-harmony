"""The real (sim) hub binary ships its stderr lines as hub_log to a home-assistant client only, and keeps the tmpfs log."""

import json
import re

import aiohttp

TOKEN = "test-token"


def env(t, d=None, id=1):
    return json.dumps({"v": 1, "id": id, "t": t, "d": d or {}})


async def frames_until(ws, stop_t):
    out = []
    while True:
        m = await ws.receive_json(timeout=5)
        out.append(m)
        if m["t"] == stop_t:
            return out


async def test_startup_lines_are_shipped_after_hello_and_still_in_the_log(start_hub):
    hub = start_hub()
    async with aiohttp.ClientSession() as session:
        ws = await session.ws_connect(hub.url)
        await ws.send_str(env("hello", {"token": TOKEN, "client": "home-assistant"}))
        await ws.send_str(env("ping", id=9))  # answered after everything queued before it
        got = await frames_until(ws, "pong")
        logs = [m["d"] for m in got if m["t"] == "hub_log"]
        assert any(
            re.match(r"\[[\d:.]+ \+\d+ms\] hub: build ", d["msg"]) and d["level"] == "info"
            for d in logs
        ), got
        assert all(isinstance(d["ts"], int) for d in logs)
        await ws.close()
        plain = await session.ws_connect(hub.url)
        await plain.send_str(env("hello", {"token": TOKEN, "client": "deploy"}))
        await plain.send_str(env("ping", id=9))
        assert not [m for m in await frames_until(plain, "pong") if m["t"] == "hub_log"]
        await plain.close()
    assert b"hub: build " in hub.err_path.read_bytes()
