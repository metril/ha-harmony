"""The integration against the REAL add-on app over the real channel (no fake add-on): hello -> bundle -> ack, the editor's
Bluetooth commands relayed to a scripted hub (bt_cmd / bt_result), hub Bluetooth state and remote keys to an editor
socket (live), and the 4001 refusal. Everything waits on messages, never on sleeps."""

import asyncio
import json

import pytest
from addon_ws import live, recv
from cfgapi import put_apply
from conftest import ADDON_TOKEN, HUB_ID, make_entry
from test_addon import H
from test_ha_bluetooth import CFG, BtHub, state, until

import custom_components.ha_harmony  # noqa: F401  (before the HA plugin shadows it)
from custom_components.ha_harmony.const import DOMAIN
from harmony_addon.app import create_app
from harmony_addon.auth import NoAuth

pytestmark = pytest.mark.usefixtures("enable_custom_integrations", "fast_client")


@pytest.fixture
async def addon(aiohttp_client, tmp_path):
    app = create_app(
        tmp_path / "addon", auth=NoAuth(), bundle_token=ADDON_TOKEN, bundle_networks=None
    )
    return await aiohttp_client(app)


@pytest.fixture(autouse=True)
async def _unload_first(hass, fake_hub, addon):
    yield
    for entry in hass.config_entries.async_entries(DOMAIN):
        await hass.config_entries.async_unload(entry.entry_id)


class _Link:
    def __init__(self, addon):
        self.addon = addon

    def link_data(self):
        return {
            "addon_host": "127.0.0.1",
            "addon_port": self.addon.server.port,
            "addon_token": ADDON_TOKEN,
        }


async def apply_yaml(addon, text):
    from harmony_core.config_model import load_source

    cfg = json.loads(json.dumps(load_source(text) or {}))
    r = await put_apply(addon, cfg, HUB_ID)
    assert r.status == 200, await r.text()


async def start(hass, fake_hub, addon, bt, *, apply_first=True):
    hub = await fake_hub(bt.handler)
    if apply_first:
        await apply_yaml(addon, CFG)
    entry = make_entry("127.0.0.1", hub.port, addon=_Link(addon))
    entry.add_to_hass(hass)
    assert await hass.config_entries.async_setup(entry.entry_id)
    return entry, entry.runtime_data


async def test_hello_bundle_ack_bt_relay_state_keys_and_live_end_to_end(hass, fake_hub, addon):
    bt = BtHub()
    _entry, c = await start(hass, fake_hub, addon, bt)
    await until(hass, lambda: c.config_state == "accepted" and c.link_state == "ok")

    # editor socket: hub status, Bluetooth status and remote keys
    lv = await live(addon, f"hub:{HUB_ID}", f"bt:{HUB_ID}", f"keys:{HUB_ID}")
    snap = await recv(lv, "snapshot")
    st = snap["topics"][f"hub:{HUB_ID}"]
    assert st["connected"] is True

    # ack: the add-on learns that the integration accepted the applied rev (in the snapshot already, or pushed)
    while not (st["acked_rev"] and st["acked_rev"] == st["applied_rev"] and st["result"] == "ok"):
        st = await recv(lv, "hub", 8)

    # hub bt_state -> add-on -> editor
    await bt.push("bt_state", state(7, window="host"))
    got = await recv(lv, "bt", 8)
    assert got["state"]["window"] == "host" or got["status"] == "ok"

    # editor command: POST -> bt_cmd -> hub bt_pair -> bt_result -> POST answered
    r = await addon.post(
        "/api/v1/bt/pair",
        json={"hub": HUB_ID, "mode": "host", "transport": "le", "secs": 60},
        headers=H,
    )
    assert r.status == 200, await r.text()
    assert bt.sent("bt_pair")[-1]["on"] is True and bt.sent("bt_pair")[-1]["transport"] == "le"
    r = await addon.post("/api/v1/bt/pair/stop", json={"hub": HUB_ID}, headers=H)
    assert r.status == 200 and bt.sent("bt_pair")[-1] == {"on": False}

    # remote keys: the editor's keys subscription makes the integration listen; a hub key reaches the editor
    async with asyncio.timeout(8):
        while not bt.sent("bt_listen"):
            await asyncio.sleep(0.02)
    await bt.push("bt_remote_key", {"slot": 0, "page": 12, "usage": 233, "down": True})
    key = await recv(lv, "key", 8)
    assert (key["page"], key["usage"], key["down"]) == (12, 233, True)
    await lv.close()


async def test_a_new_apply_is_pushed_and_acked_without_a_restart(hass, fake_hub, addon):
    bt = BtHub()
    _entry, c = await start(hass, fake_hub, addon, bt)
    await until(hass, lambda: c.config_state == "accepted" and c.link_state == "ok")
    rev = c.rev
    await apply_yaml(addon, CFG.replace("Homatics", "Second"))
    await until(hass, lambda: c.rev != rev and c.config_state == "accepted")


async def test_nothing_applied_then_the_first_apply(hass, fake_hub, addon):
    bt = BtHub()
    _entry, c = await start(hass, fake_hub, addon, bt, apply_first=False)
    await until(hass, lambda: c.link_state == "nothing_applied")
    await apply_yaml(addon, CFG)
    await until(hass, lambda: c.link_state == "ok" and c.config_state == "accepted")


async def test_hub_without_tcp_keepalive_is_refused_at_setup(hass, fake_hub, addon):
    async def handler(ws, env):
        if env["t"] == "hello":
            await ws.send_json(
                {
                    "v": 1,
                    "id": env["id"],
                    "t": "hello_ack",
                    "d": {"hub_id": HUB_ID, "activities": []},
                }
            )

    hub = await fake_hub(handler)
    entry = make_entry("127.0.0.1", hub.port)
    entry.add_to_hass(hass)
    assert not await hass.config_entries.async_setup(entry.entry_id)


async def test_the_bundled_hub_image_is_learnt_on_connect_and_fetched(
    hass, fake_hub, aiohttp_client, tmp_path
):
    import hashlib

    img = b"HOTA" + bytes(200)
    fw = tmp_path / "fw"
    fw.mkdir()
    (fw / "hub.img").write_bytes(img)
    meta = {
        "version": "0.5.9",
        "build": 99,
        "size": len(img),
        "sha512": hashlib.sha512(img).hexdigest(),
    }
    (fw / "hub.json").write_text(json.dumps(meta))
    app = create_app(
        tmp_path / "addon2",
        auth=NoAuth(),
        bundle_token=ADDON_TOKEN,
        bundle_networks=None,
        firmware_dir=fw,
    )
    addon2 = await aiohttp_client(app)
    _entry, c = await start(hass, fake_hub, addon2, BtHub())
    await until(hass, lambda: c.addon_fw is not None)
    assert (c.addon_fw.version, c.addon_fw.build, c.addon_fw.size) == ("0.5.9", 99, len(img))
    assert await c._link.fetch_firmware() == img
