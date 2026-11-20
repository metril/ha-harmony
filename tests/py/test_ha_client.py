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
from custom_components.ha_harmony.harmony_core.harmony_proto import MsgType

pytestmark = pytest.mark.usefixtures("enable_custom_integrations")


def _ack(env):
    return {"v": 1, "id": env["id"], "t": "hello_ack", "d": {"hub_id": "h", "activities": ["A"], "tcp_keepalive": True}}


async def _client(hub, **kw):
    session = aiohttp.ClientSession()
    kw = {"backoff_min": 0.05, "backoff_max": 0.1, **kw}
    c = HarmonyHubClient("127.0.0.1", hub.port, TOKEN, session, **kw)
    return c, session


async def test_hub_without_tcp_keepalive_is_refused_with_a_version_error(fake_hub):
    async def handler(ws, env):
        if env["t"] == "hello":
            ack = _ack(env)
            await ws.send_json({**ack, "d": {"hub_id": "h", "activities": []}})

    hub = await fake_hub(handler)
    c, session = await _client(hub)
    try:
        with pytest.raises(HarmonyVersionError, match="firmware"):
            await c.async_connect()
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


async def test_newer_hub_extra_fields_and_unknown_type_keep_connection(fake_hub):
    async def handler(ws, env):
        if env["t"] == "hello":
            ack = _ack(env)
            ack["d"].update({"fw_build": 366, "ota_key": "dev", "future_field": {"x": 1}})
            await ws.send_json(ack)
            await ws.send_json({"v": 1, "id": 0, "t": "from_the_future", "d": {"a": 1}})
            body = {"remote": 1, "percent": 77, "millivolts": 3800, "extra": True}
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
        assert hub.connections == 1 and c.connected and c.hub_id == "h"
    finally:
        await c.async_close()
        await session.close()


async def test_error_envelope_with_unhashable_id_is_ignored():
    c = HarmonyHubClient("127.0.0.1", 1, TOKEN, None)
    c._dispatch({"v": 1, "id": [1], "t": "error", "d": {"code": "x", "message": "y"}})
    c._dispatch({"v": 1, "id": True, "t": "error", "d": {"code": "x", "message": "y"}})


async def test_hub_with_tcp_keepalive_gets_no_ping_timer_and_the_socket_has_keepalive(fake_hub):
    import socket

    pinged = asyncio.Event()

    async def handler(ws, env):
        if env["t"] == "hello":
            await ws.send_json(_ack(env))
        elif env["t"] == "ping":
            pinged.set()

    hub = await fake_hub(handler)
    c, session = await _client(hub)
    await c.async_connect()
    try:
        sock = c._ws._response.connection.transport.get_extra_info("socket")
        assert sock.getsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE) == 1
        if hasattr(socket, "TCP_KEEPIDLE"):
            assert sock.getsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPIDLE) == 60
            assert sock.getsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPINTVL) == 10
            assert sock.getsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPCNT) == 3
        with pytest.raises(TimeoutError):  # no ping is ever sent
            await asyncio.wait_for(pinged.wait(), 0.3)
        assert hub.connections == 1
    finally:
        await c.async_close()
        await session.close()
