import socket

from harmony_addon.bus import Bus
from harmony_addon.sockopts import set_keepalive


async def test_publish_filters_by_hub_and_topic():
    bus = Bus()
    async with bus.subscribe("a", ["bt"]) as s:
        bus.publish("a", "bt", 1)
        bus.publish("a", "key", 2)
        bus.publish("b", "bt", 3)
        assert await s.get() == ("bt", 1)
        assert not s._q


async def test_overflow_drops_oldest_and_flags_resync():
    bus = Bus()
    async with bus.subscribe("a", ["t"], maxlen=2) as s:
        for i in range(3):
            bus.publish("a", "t", i)
        assert s.take_resync() and not s.take_resync()
        assert [await s.get(), await s.get()] == [("t", 1), ("t", 2)]


async def test_unsubscribe_cleans_up_and_counts():
    bus = Bus()
    async with bus.subscribe("a", ["keys"]):
        assert bus.subscribers("a", "keys") == 1
    assert bus.subscribers("a", "keys") == 0 and not bus._subs


async def test_async_iteration():
    bus = Bus()
    async with bus.subscribe("a", ["t"]) as s:
        bus.publish("a", "t", "x")
        async for item in s:
            assert item == ("t", "x")
            break


def test_set_keepalive():
    assert set_keepalive(None) is False
    a = socket.socket()
    try:
        assert set_keepalive(a) is True
        assert a.getsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE) == 1
        if hasattr(socket, "TCP_KEEPCNT"):
            assert a.getsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPCNT) == 3
    finally:
        a.close()
