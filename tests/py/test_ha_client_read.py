"""HttpHaApi must read the whole answer (not one chunk), refuse oversize ones, and say why it failed."""

import asyncio
import json
import logging

import pytest
from aiohttp import web

from harmony_addon import ha_client
from harmony_addon.ha_client import Catalog, HaUnavailable, HttpHaApi


def _reason(exc) -> str:
    return getattr(exc, "reason", str(exc))


async def _api(aiohttp_server, handler):
    app = web.Application()
    app.router.add_get("/core/api/states", handler)
    srv = await aiohttp_server(app)
    return HttpHaApi("tok", f"http://127.0.0.1:{srv.port}/core/api")


async def test_multi_chunk_body_is_read_in_full(aiohttp_server):
    rows = [
        {"entity_id": f"light.l{i}", "state": "on", "attributes": {"a": "x" * 100}}
        for i in range(20000)
    ]
    payload = json.dumps(rows).encode()
    assert len(payload) > 2_000_000

    async def h(request):
        resp = web.StreamResponse()
        resp.enable_chunked_encoding()
        await resp.prepare(request)
        for i in range(0, len(payload), 50_000):
            await resp.write(payload[i : i + 50_000])
            await asyncio.sleep(0.001)
        return resp

    assert await (await _api(aiohttp_server, h)).get("/states") == rows


async def test_oversize_body_is_refused_early(aiohttp_server, monkeypatch):
    monkeypatch.setattr(ha_client, "MAX_BODY", 100_000)
    sent = []

    async def h(request):
        resp = web.StreamResponse()
        resp.enable_chunked_encoding()
        await resp.prepare(request)
        try:
            for _ in range(200):
                await resp.write(b"x" * 10_000)
                sent.append(1)
                await asyncio.sleep(0.005)
        except ConnectionError:
            pass
        return resp

    with pytest.raises(HaUnavailable) as e:
        await (await _api(aiohttp_server, h)).get("/states")
    assert _reason(e.value) == "too_large"
    assert len(sent) < 200


async def test_non_200_and_non_json(aiohttp_server):
    async def bad(request):
        return web.Response(status=503, text="no")

    async def txt(request):
        return web.Response(text="{not json")

    with pytest.raises(HaUnavailable) as e:
        await (await _api(aiohttp_server, bad)).get("/states")
    assert _reason(e.value) == "status_503"
    with pytest.raises(HaUnavailable) as e:
        await (await _api(aiohttp_server, txt)).get("/states")
    assert _reason(e.value) == "not_json"


async def test_503_body_carries_the_reason(aiohttp_client, tmp_path):
    from test_addon_ha import make

    c = await make(aiohttp_client, tmp_path, _Api(HaUnavailable("too_large")))
    r = await c.get("/api/v1/ha/entities")
    assert r.status == 503
    err = (await r.json())["error"]
    assert err["code"] == "ha_unavailable" and err["reason"] == "too_large"


class _Api:
    def __init__(self, *answers):
        self.answers = list(answers)

    async def get(self, path):
        a = self.answers.pop(0)
        if isinstance(a, Exception):
            raise a
        return a


async def test_failures_are_logged_once_per_cause_and_recovery_once(caplog):
    caplog.set_level(logging.INFO)
    t = [0.0]
    api = _Api(
        HaUnavailable("status_500"), HaUnavailable("status_500"), {"x": 1},
        HaUnavailable("ClientOSError"), [], [],
    )  # fmt: skip
    cat = Catalog(api, lambda: t[0])
    for _ in range(4):
        t[0] += 10
        try:
            await cat.entities(None, None)
        except HaUnavailable:
            pass
    warns = [r.getMessage() for r in caplog.records if r.levelno == logging.WARNING]
    assert warns == [
        "Home Assistant /states cannot be read: status_500",
        "Home Assistant /states cannot be read: unexpected_shape",
        "Home Assistant /states cannot be read: ClientOSError",
    ]
    t[0] += 10
    await cat.entities(None, None)
    assert [r.getMessage() for r in caplog.records if r.levelno == logging.INFO] == [
        "Home Assistant /states can be read again"
    ]
