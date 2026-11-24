"""The add-on's bundled hub firmware: GET /api/v1/firmware/hub.img (bearer) and the `fw` frame after hello."""

import hashlib
import json

import pytest
from addon_ws import BEARER, TOKEN, connect, recv
from test_addon import clock

from harmony_addon.app import create_app
from harmony_addon.auth import NoAuth
from harmony_addon.options import Options

IMG = b"HOTA" + bytes(300)
META = {
    "version": "0.5.9",
    "build": 760,
    "size": len(IMG),
    "sha512": hashlib.sha512(IMG).hexdigest(),
}


async def make(tmp_path, aiohttp_client, *, bundled):
    fw = tmp_path / "fw"
    fw.mkdir()
    if bundled:
        (fw / "hub.img").write_bytes(IMG)
        (fw / "hub.json").write_text(json.dumps(META))
    app = create_app(
        tmp_path / "data",
        options=Options("info"),
        clock=clock,
        auth=NoAuth(),
        bundle_token=TOKEN,
        bundle_networks=None,
        firmware_dir=fw,
    )
    return await aiohttp_client(app), fw


async def frames_until_settled(ws):
    out = []
    while (m := await recv(ws))["t"] != "nothing_applied":
        out.append(m)
    return out


async def test_image_route_serves_the_bundled_image(tmp_path, aiohttp_client):
    c, _ = await make(tmp_path, aiohttp_client, bundled=True)
    r = await c.get("/api/v1/firmware/hub.img", headers=BEARER)
    assert r.status == 200
    assert await r.read() == IMG


async def test_image_route_needs_the_bearer_token(tmp_path, aiohttp_client):
    c, _ = await make(tmp_path, aiohttp_client, bundled=True)
    assert (await c.get("/api/v1/firmware/hub.img")).status == 401
    bad = {"Authorization": "Bearer nope"}
    assert (await c.get("/api/v1/firmware/hub.img", headers=bad)).status == 401


async def test_image_route_404_without_an_image_or_with_a_bad_meta(tmp_path, aiohttp_client):
    c, fw = await make(tmp_path, aiohttp_client, bundled=False)
    assert (await c.get("/api/v1/firmware/hub.img", headers=BEARER)).status == 404
    (fw / "hub.img").write_bytes(IMG)
    (fw / "hub.json").write_text(json.dumps({**META, "size": 5}))  # size disagrees with the file
    assert (await c.get("/api/v1/firmware/hub.img", headers=BEARER)).status == 404
    (fw / "hub.json").write_text("{not json")
    assert (await c.get("/api/v1/firmware/hub.img", headers=BEARER)).status == 404


async def test_hello_answer_carries_fw_when_an_image_is_bundled(tmp_path, aiohttp_client):
    c, _ = await make(tmp_path, aiohttp_client, bundled=True)
    ws = await connect(c, "default")
    fw = await recv(ws, "fw")
    assert fw["fw"] == META
    await ws.close()


async def test_hello_answer_has_no_fw_without_an_image(tmp_path, aiohttp_client):
    c, _ = await make(tmp_path, aiohttp_client, bundled=False)
    ws = await connect(c, "default")
    assert all(m["t"] != "fw" for m in await frames_until_settled(ws))
    await ws.close()


@pytest.mark.parametrize("bad", [{"build": "7"}, {"version": 5}, {"size": -1}, {"build": True}])
async def test_malformed_meta_is_not_announced(tmp_path, aiohttp_client, bad):
    c, fw = await make(tmp_path, aiohttp_client, bundled=True)
    (fw / "hub.json").write_text(json.dumps({**META, **bad}))
    ws = await connect(c, "default")
    assert all(m["t"] != "fw" for m in await frames_until_settled(ws))
    await ws.close()
