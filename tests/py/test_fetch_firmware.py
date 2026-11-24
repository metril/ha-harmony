"""fetch_firmware must return the whole image even when the body arrives in several TCP segments."""

import asyncio

from aiohttp import web

import custom_components.ha_harmony  # noqa: F401  (before the HA plugin shadows it)
from custom_components.ha_harmony.addon_link import AddonClient


async def test_fetch_firmware_reads_whole_body_across_segments(aiohttp_client):
    img = bytes(range(256)) * 400  # 102400 bytes
    parts = [img[:30000], img[30000:70000], img[70000:]]

    async def handler(request):
        resp = web.StreamResponse(headers={"Content-Length": str(len(img))})
        await resp.prepare(request)
        for p in parts:
            await resp.write(p)
            await resp.drain()
            await asyncio.sleep(0.02)  # force separate segments on the client
        return resp

    app = web.Application()
    app.router.add_get("/api/v1/firmware/hub.img", handler)
    client = await aiohttp_client(app)
    link = object.__new__(AddonClient)
    link._session = client.session
    link._headers = {}
    link._fw_url = str(client.make_url("/api/v1/firmware/hub.img"))
    assert await link.fetch_firmware() == img
