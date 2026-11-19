"""Config limits end to end: the integration learns the hub's limits from hello_ack, sends big
configs as config_chunk, and compiles against the old fixed limits for a hub that reports none."""

import asyncio
import json
import zlib

import pytest
from conftest import HUB_ID
from homeassistant.config_entries import ConfigEntryState
from test_ha_config import BASE, hub_hash, setup

from custom_components.ha_harmony.harmony_core.hub_limits import CURRENT

pytestmark = pytest.mark.usefixtures("enable_custom_integrations", "fast_client")


@pytest.fixture
def write_yaml(hass, fake_addon):
    """write_yaml(text | dict): apply a config in the fake add-on, served to the integration as a bundle."""
    hass.data["fake_addon"] = fake_addon
    return lambda cfg: fake_addon.apply(cfg) if isinstance(cfg, dict) else fake_addon.apply_yaml(cfg)


def many(n: int, per: int = 250, name: str = "D") -> dict:
    """A config of `n` NEC commands spread over devices of `per` each (a dict: YAML parsing 800 kB is slow)."""
    return {
        "devices": [
            {
                "id": f"d{d}",
                "name": f"{name} {d}",
                "commands": {
                    f"c{j}": {"proto": "nec", "addr": d, "cmd": j % 256} for j in range(min(per, n - d * per))
                },
            }
            for d in range((n + per - 1) // per)
        ]
    }


async def test_a_config_far_over_256_commands_reaches_the_real_hub_in_chunks(hass, hub_rf, write_yaml):
    write_yaml(BASE)
    entry = await setup(hass, hub_rf)
    coord = entry.runtime_data
    assert coord.client.limits is not None and not coord.client.limits.legacy
    assert coord.client.limits == CURRENT
    sent = []
    real = coord.client._request

    async def spy(msg_type, data, timeout=None):
        sent.append((msg_type.name, data.get("seq")))
        return await real(msg_type, data, timeout)

    coord.client._request = spy
    write_yaml(many(1500))  # ~70 KB of config text: 6 chunks of 12000 bytes
    await hass.async_block_till_done()
    for _ in range(100):
        if coord.compiled.usage["commands"] == 1500 and coord.client.config_hash == coord.compiled.config_hash:
            break
        await hass.async_block_till_done()

        await asyncio.sleep(0.2)
    assert coord.compiled.usage["commands"] == 1500
    assert coord.last_error is None and entry.state is ConfigEntryState.LOADED
    chunks = [s for n, s in sent if n == "CONFIG_CHUNK"]
    assert len(chunks) >= 5 and chunks == list(range(len(chunks))) and not [n for n, _ in sent if n == "SET_CONFIG"]
    # the hub runs exactly the compiled text
    assert coord.client.config_hash == coord.compiled.config_hash == await hub_hash(hub_rf.url)
    await hass.config_entries.async_unload(entry.entry_id)


def _fake_hub_handler(limits, log):
    async def handler(ws, env):
        t = env["t"]
        reply = {"v": 1, "id": env["id"]}
        if t == "hello":
            d = {"hub_id": HUB_ID, "activities": []}
            if limits is not None:
                d["limits"] = limits
            await ws.send_json({**reply, "t": "hello_ack", "d": d})
        elif t in ("set_config", "config_chunk"):
            log.append(env)
            d = env["d"]
            if t == "config_chunk":
                chunks = [e["d"] for e in log if e["t"] == "config_chunk"]
                if sum(len(c["data"].encode()) for c in chunks) < d["total"]:
                    await ws.send_json({**reply, "t": "config_ack", "d": {"ok": True}})
                    return
                text = "".join(c["data"] for c in chunks)
                h = f"{zlib.crc32(text.encode()):08x}"
            else:
                h = "00000000"
            await ws.send_json({**reply, "t": "config_ack", "d": {"ok": True, "config_hash": h}})

    return handler


async def test_old_firmware_without_limits_gets_the_old_fixed_limits(hass, fake_hub, write_yaml):
    log = []
    hub = await fake_hub(_fake_hub_handler(None, log))
    write_yaml(many(300))
    entry = await setup(hass, hub)
    coord = entry.runtime_data
    assert coord.client.limits is not None and coord.client.limits.legacy
    assert "too many commands: 300 (the hub allows at most 256)" in coord.last_error
    assert not log  # refused before anything was sent
    await hass.config_entries.async_unload(entry.entry_id)


async def test_old_firmware_still_takes_what_fits_in_one_message(hass, fake_hub, write_yaml):
    log = []
    hub = await fake_hub(_fake_hub_handler(None, log))
    write_yaml(many(200))
    entry = await setup(hass, hub)
    assert entry.runtime_data.last_error is None
    assert {e["t"] for e in log} == {"set_config"}  # a hub without `limits` has no config_chunk
    await hass.config_entries.async_unload(entry.entry_id)


async def test_a_tighter_hub_budget_is_reported_before_pushing(hass, fake_hub, write_yaml):
    log = []
    tight = {**CURRENT.to_wire(), "budget": 128 * 1024}
    hub = await fake_hub(_fake_hub_handler(tight, log))
    write_yaml(many(1500))
    entry = await setup(hass, hub)
    err = entry.runtime_data.last_error
    assert err and "KiB of hub memory, the hub allows 128 KiB" in err
    assert not log
    await hass.config_entries.async_unload(entry.entry_id)


async def test_chunks_are_in_order_sized_and_reassemble_to_the_compiled_text(hass, fake_hub, write_yaml):
    log = []
    hub = await fake_hub(_fake_hub_handler(CURRENT.to_wire(), log))
    write_yaml(many(1200))
    entry = await setup(hass, hub)
    coord = entry.runtime_data
    chunks = [e["d"] for e in log if e["t"] == "config_chunk"]
    assert [c["seq"] for c in chunks] == list(range(len(chunks))) and len(chunks) > 3
    assert len({c["total"] for c in chunks}) == 1
    assert all(len(c["data"].encode()) <= CURRENT.chunk for c in chunks)
    text = "".join(c["data"] for c in chunks)
    assert len(text.encode()) == chunks[0]["total"]
    assert json.loads(text) == coord.compiled.hub_json
    assert f"{zlib.crc32(text.encode()):08x}" == coord.compiled.config_hash == coord.client.config_hash
    await hass.config_entries.async_unload(entry.entry_id)


async def test_non_ascii_names_split_on_character_boundaries(hass, fake_hub, write_yaml):
    log = []
    hub = await fake_hub(_fake_hub_handler(CURRENT.to_wire(), log))
    write_yaml(many(1000, per=100, name="Télé ✓"))
    entry = await setup(hass, hub)
    chunks = [e["d"] for e in log if e["t"] == "config_chunk"]
    text = "".join(c["data"] for c in chunks)
    assert chunks and "Télé ✓ 7" in text and len(text.encode()) == chunks[0]["total"]
    await hass.config_entries.async_unload(entry.entry_id)
