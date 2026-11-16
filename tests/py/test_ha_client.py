"""Client-level behaviour that needs a misbehaving hub: heartbeat loss, mismatched `v` pushes."""

import asyncio

import aiohttp
import pytest
from conftest import TOKEN

import custom_components.ha_harmony  # noqa: F401  (before the HA plugin shadows it)
from custom_components.ha_harmony.api import (
    HarmonyCommandError,
    HarmonyConnectionError,
    HarmonyHubClient,
    HarmonyVersionError,
)
from custom_components.ha_harmony.harmony_proto import MsgType

pytestmark = pytest.mark.usefixtures("enable_custom_integrations")


def _ack(env):
    return {"v": 1, "id": env["id"], "t": "hello_ack", "d": {"hub_id": "h", "activities": ["A"]}}


async def _client(hub, **kw):
    session = aiohttp.ClientSession()
    kw = {"backoff_min": 0.05, "backoff_max": 0.1, **kw}
    c = HarmonyHubClient("127.0.0.1", hub.port, TOKEN, session, **kw)
    return c, session


async def test_missing_pong_is_a_dead_connection(fake_hub):
    async def handler(ws, env):
        if env["t"] == "hello":
            await ws.send_json(_ack(env))  # never answers ping

    hub = await fake_hub(handler)
    c, session = await _client(hub, heartbeat_interval=0.1, pong_timeout=0.1)
    states = []
    c.async_subscribe_state(lambda up, err: states.append(up))
    await c.async_connect()
    try:
        async with asyncio.timeout(5):
            while hub.connections < 2:
                await asyncio.sleep(0.05)
        assert False in states and True in states  # dropped, then reconnected with a new hello
    finally:
        await c.async_close()
        await session.close()


async def test_pong_keeps_connection_alive(fake_hub):
    async def handler(ws, env):
        reply = (
            _ack(env) if env["t"] == "hello" else {"v": 1, "id": env["id"], "t": "pong", "d": {}}
        )
        await ws.send_json(reply)

    hub = await fake_hub(handler)
    c, session = await _client(hub, heartbeat_interval=0.05, pong_timeout=2.0)
    await c.async_connect()
    try:
        await asyncio.sleep(0.4)
        assert hub.connections == 1 and c.connected
    finally:
        await c.async_close()
        await session.close()


async def test_send_errors_and_pending_failed_on_close(fake_hub):
    async def handler(ws, env):
        if env["t"] == "hello":
            await ws.send_json(_ack(env))
        elif env["t"] == "command":
            await ws.send_json(
                {"v": 1, "id": env["id"], "t": "error", "d": {"code": "bad", "message": "no"}}
            )

    hub = await fake_hub(handler)
    c, session = await _client(hub)
    await c.async_connect()
    try:
        with pytest.raises(HarmonyCommandError):
            await c.async_send(MsgType.COMMAND, {"device": "d", "command": "c"})
    finally:
        await c.async_close()
        await session.close()
    with pytest.raises(HarmonyConnectionError):
        await c.async_send(MsgType.PING, {})


async def test_push_with_other_version_is_fatal(fake_hub):
    async def handler(ws, env):
        await ws.send_json(_ack(env))
        await ws.send_json(
            {"v": 2, "id": 0, "t": "state_update", "d": {"activity": 0, "power": True}}
        )

    hub = await fake_hub(handler)
    c, session = await _client(hub)
    errs = []
    c.async_subscribe_state(lambda up, err: errs.append(err))
    await c.async_connect()
    try:
        async with asyncio.timeout(3):
            while not errs:
                await asyncio.sleep(0.02)
        assert isinstance(errs[-1], HarmonyVersionError)
        await asyncio.sleep(0.3)
        assert hub.connections == 1  # no reconnect loop
    finally:
        await c.async_close()
        await session.close()


async def _wait(pred, timeout=5):
    async with asyncio.timeout(timeout):
        while not pred():
            await asyncio.sleep(0.02)


async def test_unexpected_supervisor_exception_is_a_drop_and_reconnects(fake_hub):
    async def handler(ws, env):
        await ws.send_json(_ack(env))
        if hub.connections == 1:
            await ws.send_json(
                {"v": 1, "id": 0, "t": "state_update", "d": {"activity": 0, "power": True}}
            )

    hub = await fake_hub(handler)
    c, session = await _client(hub)
    states = []
    c.async_subscribe_state(lambda up, err: states.append(up))
    real = c._dispatch
    calls = []

    def boom(env):
        calls.append(env)
        if len(calls) == 1:
            raise RuntimeError("bug")
        real(env)

    c._dispatch = boom
    await c.async_connect()
    try:
        await _wait(lambda: hub.connections >= 2 and True in states)
        assert states[:2] == [False, True] and c.connected
    finally:
        await c.async_close()
        await session.close()


async def test_raising_subscriber_keeps_the_link(fake_hub):
    async def handler(ws, env):
        await ws.send_json(_ack(env))
        await ws.send_json(
            {"v": 1, "id": 0, "t": "state_update", "d": {"activity": 0, "power": True}}
        )

    hub = await fake_hub(handler)
    c, session = await _client(hub)
    seen = []

    def bad(t, d):
        seen.append(t)
        raise ValueError("subscriber bug")

    c.async_subscribe(bad)
    await c.async_connect()
    try:
        await _wait(lambda: seen)
        await asyncio.sleep(0.2)
        assert hub.connections == 1 and c.connected
    finally:
        await c.async_close()
        await session.close()


async def test_error_reply_without_message_fails_request_not_supervisor(fake_hub):
    async def handler(ws, env):
        if env["t"] == "hello":
            await ws.send_json(_ack(env))
        else:
            await ws.send_json({"v": 1, "id": env["id"], "t": "error", "d": {"code": "x"}})

    hub = await fake_hub(handler)
    c, session = await _client(hub, request_timeout=1.0)
    await c.async_connect()
    try:
        with pytest.raises(HarmonyCommandError):
            await c.async_send(MsgType.COMMAND, {"device": "d", "command": "c"})
        with pytest.raises(HarmonyCommandError):  # supervisor still alive and serving
            await c.async_send(MsgType.COMMAND, {"device": "d", "command": "c"})
        assert hub.connections == 1 and c.connected
    finally:
        await c.async_close()
        await session.close()


async def test_battery_push_reaches_subscribers(fake_hub):
    async def handler(ws, env):
        if env["t"] == "hello":
            await ws.send_json(_ack(env))
            body = {"remote": 1, "percent": 77, "millivolts": 3800}
            await ws.send_json({"v": 1, "id": 0, "t": "battery", "d": body})

    hub = await fake_hub(handler)
    c, session = await _client(hub)
    got = []
    c.async_subscribe(lambda t, d: got.append((t, d)))
    await c.async_connect()
    try:
        async with asyncio.timeout(5):
            while not got:
                await asyncio.sleep(0.02)
        assert got == [(MsgType.BATTERY, {"remote": 1, "percent": 77, "millivolts": 3800})]
    finally:
        await c.async_close()
        await session.close()


async def test_error_envelope_with_unhashable_id_is_ignored():
    c = HarmonyHubClient("127.0.0.1", 1, TOKEN, None)
    c._dispatch({"v": 1, "id": [1], "t": "error", "d": {"code": "x", "message": "y"}})
    c._dispatch({"v": 1, "id": True, "t": "error", "d": {"code": "x", "message": "y"}})
