"""A persistent client that re-authenticates closes its older connections (dead peers); a transient one
(a config / reconfigure flow probing the hub) never closes anything."""

import aiohttp
from conftest import TOKEN


async def hello(session, hub, **extra):
    ws = await session.ws_connect(hub.url)
    await ws.send_json({"v": 1, "id": 1, "t": "hello", "d": {"token": TOKEN, **extra}})
    ack = await ws.receive_json()
    assert ack["t"] == "hello_ack"
    return ws


async def closed_by_hub(ws) -> bool:
    try:
        while True:
            msg = await ws.receive(timeout=2)
            if msg.type in (
                aiohttp.WSMsgType.CLOSE,
                aiohttp.WSMsgType.CLOSED,
                aiohttp.WSMsgType.CLOSING,
            ):
                return True
    except TimeoutError:
        return False


async def pong(ws, n):
    await ws.send_json({"v": 1, "id": n, "t": "ping", "d": {}})
    while True:  # skip pushes
        if (await ws.receive_json(timeout=2))["t"] == "pong":
            return


async def test_transient_probe_never_closes_the_persistent_link(start_hub):
    hub = start_hub()
    async with aiohttp.ClientSession() as s:
        persistent = await hello(s, hub, client="home-assistant")
        probe = await hello(s, hub, client="home-assistant", transient=True)
        assert not persistent.closed
        await pong(persistent, 2)
        await probe.close()
        await persistent.close()


async def test_persistent_reauth_closes_older_connection_of_same_client_only(start_hub):
    hub = start_hub()
    async with aiohttp.ClientSession() as s:
        old = await hello(s, hub, client="home-assistant")
        probe = await hello(s, hub, client="home-assistant", transient=True)
        other = await hello(s, hub, client="addon")
        new = await hello(s, hub, client="home-assistant")
        assert await closed_by_hub(old)
        for i, ws in enumerate((probe, other, new)):
            await pong(ws, 3 + i)
        for ws in (probe, other, new):
            await ws.close()
