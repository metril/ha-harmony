"""Property: in a big generated config EVERY command, pressed through the real integration against
the real `hub --sim`, emits exactly the frames it was configured with.

The config is far over one frame (it travels as config_chunk), mixes protocol / pronto / raw codes,
and uses long names and names that differ only in case or punctuation -- the places a per-load
table, a name hash index or a pool offset could go wrong for some commands but not others.
"""

from __future__ import annotations

import random

import pytest
from conftest import make_entry
from ir_ref import encode
from test_hub_engine import read_lines

import custom_components.ha_harmony  # noqa: F401  (before the HA plugin shadows it)

pytestmark = pytest.mark.usefixtures("enable_custom_integrations", "fast_client")

DEVICES, PER_DEVICE = 5, 24
# names that differ only in case / punctuation / spacing, plus the longest the hub keeps (31 bytes)
CLASHING = ["VolUp", "volup", "VOLUP", "Vol Up", "Vol-Up", "Vol_Up", "Vol.Up", "vol up"]
LONG = ["A" * 31, "A" * 30 + "B", "a" * 31, "Long command name with spaces 31"[:31]]


def _code(rng: random.Random, kind: int, n: int) -> dict:
    if kind == 0:
        return {"proto": "nec", "addr": rng.randrange(256), "cmd": rng.randrange(256)}
    if kind == 1:
        return {"proto": "samsung32", "addr": rng.randrange(256), "cmd": rng.randrange(256)}
    if kind == 2:
        words = [0, 0x6D, 3, 0]
        for _ in range(3):
            words += [rng.randrange(8, 40), rng.randrange(8, 120)]
        return {"pronto": " ".join(f"{w:04x}" for w in words)}
    us = [rng.randrange(40, 120) for _ in range(480 + (n % 9) * 2)]
    return {"raw": {"hz": rng.choice([36000, 37900, 38000, 40000]), "us": us}}


def generated() -> dict:
    rng = random.Random(1517)
    devices = []
    for d in range(DEVICES):
        names = [*CLASHING, *LONG] + [f"Cmd {d}-{i}" for i in range(PER_DEVICE)]
        names = names[:PER_DEVICE]
        devices.append(
            {
                "id": f"dev{d}",
                "name": f"Generated device {d}",
                "commands": {n: _code(rng, (3, 3, 0, 2, 3, 1)[(i + d) % 6], i) for i, n in enumerate(names)},
            }
        )
    return {"devices": devices}


async def test_every_command_of_a_chunked_mixed_config_emits_its_exact_frames(hass, hub_rf, fake_addon):
    cfg = generated()
    fake_addon.apply(cfg)
    entry = make_entry("127.0.0.1", hub_rf.port, addon=fake_addon)
    entry.add_to_hass(hass)
    assert await hass.config_entries.async_setup(entry.entry_id)
    if entry.runtime_data.first_sync is not None:
        await entry.runtime_data.first_sync
    await hass.async_block_till_done()
    coord = entry.runtime_data
    assert coord.config_pushes == 1 and coord.last_error is None
    assert coord.compiled.usage["json_bytes"] > 64 * 1024, "the config must be big enough to be chunked"
    assert coord.client.config_hash == coord.compiled.config_hash

    wrong = []
    for dev in cfg["devices"]:
        for name, code in dev["commands"].items():
            await coord.async_device_command(dev["id"], name)
            hz, once, _ = encode(code)
            want = f"ir port=255 hz={hz} n={len(once)} us={','.join(str(x) for x in once)}"
            if read_lines(hub_rf, 1) != [want]:
                wrong.append(f"{dev['id']}:{name}")
    assert not wrong, f"{len(wrong)} of {DEVICES * PER_DEVICE} commands did not emit their frames: {wrong[:10]}"
    await hass.config_entries.async_unload(entry.entry_id)


def test_the_generated_config_has_the_names_that_break_lookups():
    cmds = generated()["devices"][0]["commands"]
    assert {n.lower().replace(" ", "").replace("-", "").replace("_", "").replace(".", "") for n in cmds} >= {"volup"}
    assert sum(1 for n in cmds if n.lower().replace(" ", "").replace("-", "").replace("_", "").replace(".", "") == "volup") >= 6
    assert any(len(n) == 31 for n in cmds)
