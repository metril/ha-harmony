"""IR test-fire: validation, rate limit, the one hard-coded service path, token hygiene."""

import inspect
import logging

import pytest
from aiohttp import web
from test_addon import H

from harmony_addon import ir_tester
from harmony_addon.app import create_app
from harmony_addon.auth import NoAuth
from harmony_addon.ha_client import HaUnavailable
from harmony_addon.ir_tester import MAX_PER_MINUTE, HttpIrSender, SendLimiter, SendRefused

NEC = {"proto": "nec", "addr": 4, "cmd": 8}


class FakeSender:
    def __init__(self, exc=None):
        self.sent, self.exc = [], exc

    async def send(self, hub, action):
        if self.exc:
            raise self.exc
        self.sent.append((hub, action))


class Clock:
    t = 100.0

    def __call__(self):
        return self.t


@pytest.fixture
def clock():
    return Clock()


async def make(aiohttp_client, tmp_path, sender, clock):
    return await aiohttp_client(
        create_app(tmp_path, auth=NoAuth(), ir_sender=sender, send_clock=clock)
    )


async def fire(c, **body):
    body = {"hub": "h1", "ir": NEC, **body}
    r = await c.post("/api/v1/ir/test", json=body, headers=H)
    return r.status, await r.json()


async def test_valid_send_reaches_the_sender_validated(aiohttp_client, tmp_path, clock):
    s = FakeSender()
    c = await make(aiohttp_client, tmp_path, s, clock)
    st, body = await fire(c, port_mask=3)
    assert st == 200 and body["ok"] is True
    assert s.sent == [("h1", {**NEC, "port_mask": 3})]
    clock.t += 1
    st, _ = await fire(c, ir={"pronto": "0000 006d 0002 0000 0060 0018 0018 0018"})
    assert st == 200 and s.sent[1][1] == {"pronto": "0000 006D 0002 0000 0060 0018 0018 0018"}
    clock.t += 1
    assert (await fire(c, ir={"raw": {"hz": 38000, "us": [9000, 4500, 560, 560]}}))[0] == 200


@pytest.mark.parametrize(
    "ir",
    [
        {"flipper": {"file": "tv.ir", "signal": "Power"}},
        {"smartir": {"file": "x.json", "command": "a"}},
        {"proto": "nec", "addr": 999, "cmd": 1},
        {"proto": "nope", "addr": 1, "cmd": 1},
        {"proto": "nec", "addr": 1},
        {"raw": {"hz": 38000, "us": [500] * 600}},
        {"raw": {"hz": 38000, "us": [500, 500, 500]}},
        {"pronto": "zzzz"},
        {"proto": "nec", "addr": 1, "cmd": 1, "pronto": "0000 006D 0002 0000 0060 0018 0018 0018"},
        {"proto": "nec", "addr": 1, "cmd": 1, "port_mask": 0},
        {},
    ],
)
async def test_invalid_or_file_referencing_ir_is_422_and_never_sent(
    aiohttp_client, tmp_path, clock, ir
):
    s = FakeSender()
    c = await make(aiohttp_client, tmp_path, s, clock)
    st, body = await fire(c, ir=ir)
    assert st == 422 and body["error"]["code"] == "invalid_ir" and body["error"]["errors"][0]["msg"]
    assert s.sent == []
    assert await _limiter_untouched(c)


async def _limiter_untouched(c):
    from harmony_addon.app import LIMITER_KEY

    return not c.app[LIMITER_KEY]._sent  # invalid requests do not use up the budget


@pytest.mark.parametrize(
    "body",
    [
        {"hub": "../x", "ir": NEC},
        {"hub": "h1"},
        {"ir": NEC},
        {"hub": "h1", "ir": "nec"},
        {"hub": "h1", "ir": NEC, "x": 1},
        {"hub": "h1", "ir": NEC, "port_mask": 256},
        {"hub": "h1", "ir": NEC, "port_mask": "1"},
        {"hub": "h1", "ir": {**NEC, "port_mask": 1}, "port_mask": 2},
    ],
)
async def test_schema(aiohttp_client, tmp_path, clock, body):
    s = FakeSender()
    c = await make(aiohttp_client, tmp_path, s, clock)
    r = await c.post("/api/v1/ir/test", json=body, headers=H)
    assert r.status == 400 and s.sent == []


async def test_min_interval_and_per_minute_limits(aiohttp_client, tmp_path, clock):
    s = FakeSender()
    c = await make(aiohttp_client, tmp_path, s, clock)
    assert (await fire(c))[0] == 200
    clock.t += 0.2
    r = await c.post("/api/v1/ir/test", json={"hub": "h1", "ir": NEC}, headers=H)
    body = await r.json()
    assert r.status == 429 and body["error"]["code"] == "rate_limited"
    assert 250 <= body["error"]["retry_after_ms"] <= 400 and r.headers["Retry-After"] == "1"
    assert len(s.sent) == 1
    for _ in range(MAX_PER_MINUTE - 1):  # fill the minute at 1 s spacing
        clock.t += 1.0
        assert (await fire(c))[0] == 200
    clock.t += 1.0
    st, body = await fire(c)
    assert st == 429 and body["error"]["retry_after_ms"] > 1000
    assert len(s.sent) == MAX_PER_MINUTE
    clock.t += 61
    assert (await fire(c))[0] == 200


def test_limiter_unit():
    c = Clock()
    lim = SendLimiter(c)
    assert lim.acquire() is None
    assert lim.acquire() == 501
    c.t += 0.5
    assert lim.acquire() is None


async def test_no_token_is_503(aiohttp_client, tmp_path, clock):
    c = await make(aiohttp_client, tmp_path, None, clock)
    st, body = await fire(c)
    assert st == 503 and body["error"]["code"] == "ha_unavailable"


async def test_ha_failures_are_stable_errors(aiohttp_client, tmp_path, clock):
    c = await make(aiohttp_client, tmp_path, FakeSender(SendRefused(400)), clock)
    st, body = await fire(c)
    assert (
        st == 502 and body["error"]["code"] == "send_failed" and body["error"]["http_status"] == 400
    )
    clock.t += 1
    c2 = await make(aiohttp_client, tmp_path / "x", FakeSender(HaUnavailable("x")), clock)
    assert (await fire(c2))[0] == 503


async def test_csrf_and_ingress_rules_apply(aiohttp_client, tmp_path, clock):
    c = await make(aiohttp_client, tmp_path, FakeSender(), clock)
    r = await c.post("/api/v1/ir/test", json={"hub": "h1", "ir": NEC})  # no CSRF header
    assert r.status == 403
    assert (await c.get("/api/v1/ir/test")).status == 405


async def test_only_the_exact_service_path_is_requested_and_token_never_leaks(
    aiohttp_client, aiohttp_server, tmp_path, clock, caplog
):
    caplog.set_level(logging.DEBUG)
    secret = "SUPERSECRET-" + "q" * 20
    seen = []

    async def handler(request):
        seen.append(
            (
                request.method,
                request.path_qs,
                request.headers.get("Authorization"),
                await request.json(),
            )
        )
        return web.json_response([])

    async def other(request):  # anything else is a bug
        seen.append(("UNEXPECTED", request.path))
        return web.Response(status=500, text=secret)

    core = web.Application()
    core.router.add_post("/core/api/services/ha_harmony/send_ir", handler)
    core.router.add_route("*", "/{tail:.*}", other)
    srv = await aiohttp_server(core)
    sender = HttpIrSender(secret, f"http://127.0.0.1:{srv.port}/core/api")
    assert secret not in repr(sender)
    c = await make(aiohttp_client, tmp_path, sender, clock)
    st, body = await fire(c, port_mask=5)
    assert st == 200
    assert seen == [
        (
            "POST",
            "/core/api/services/ha_harmony/send_ir",
            f"Bearer {secret}",
            {"config_entry": "h1", "ir": {**NEC, "port_mask": 5}},
        )
    ]
    assert secret not in str(body) + caplog.text


async def test_ha_error_text_and_token_are_not_echoed(
    aiohttp_client, aiohttp_server, tmp_path, clock, caplog
):
    caplog.set_level(logging.DEBUG)
    secret = "SUPERSECRET-" + "w" * 20

    async def handler(request):
        return web.Response(status=500, text=f"boom {secret}")

    core = web.Application()
    core.router.add_post("/core/api/services/ha_harmony/send_ir", handler)
    srv = await aiohttp_server(core)
    c = await make(
        aiohttp_client,
        tmp_path,
        HttpIrSender(secret, f"http://127.0.0.1:{srv.port}/core/api"),
        clock,
    )
    r = await c.post("/api/v1/ir/test", json={"hub": "h1", "ir": NEC}, headers=H)
    assert r.status == 502 and secret not in await r.text() + caplog.text


def test_sender_has_one_capability_and_no_generic_post():
    public = {
        n for n, _ in inspect.getmembers(HttpIrSender, inspect.isfunction) if not n.startswith("_")
    }
    assert public == {"send"}
    assert list(inspect.signature(HttpIrSender.send).parameters) == ["self", "hub", "action"]
    src = inspect.getsource(ir_tester)
    assert src.count("SERVICE_PATH") >= 2 and src.count(".post(") == 1
    from harmony_addon import ha_client

    assert not hasattr(ha_client.HttpHaApi, "post") and not hasattr(ha_client.HttpHaApi, "send")


def test_default_sender_needs_the_supervisor_token(monkeypatch):
    monkeypatch.delenv("SUPERVISOR_TOKEN", raising=False)
    assert ir_tester.default_ir_sender() is None
    monkeypatch.setenv("SUPERVISOR_TOKEN", "t")
    assert isinstance(ir_tester.default_ir_sender(), HttpIrSender)
