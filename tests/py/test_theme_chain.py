"""A compiled config with a user theme, the vector the C chain test renders (tests/c/test_remote_rf.c, "theme chain"), and the
real stack: fake add-on -> integration -> `hub --sim` -> `remote_sim` (which applies the remote's own completeness rule).

The vector is the contract between the two halves of the golden render: this test proves the compiler still produces exactly
these bytes, the C test proves hub -> remote draws them the same as a direct render with that theme. Regenerate deliberately:
HH_REGEN_THEME_CHAIN=1 uv run pytest tests/py/test_theme_chain.py"""

import asyncio
import json
import os
from pathlib import Path

import pytest
from test_compiler_pages import BASE, btn, page, parse_model
from test_ha_e2e import remote, setup
from test_ha_runtime import YAML

from harmony_core.compiler import compile_config

VECTOR = Path(__file__).resolve().parents[1] / "vectors" / "theme_chain.json"
DUSK = "themes:\n  dusk: {extends: midnight, tokens: {ACC: '#FF8A3D'}, palette: {3: {fill: '#2E7D32'}}}\n"
CFG = (
    BASE
    + "theme: dusk\n"
    + DUSK
    + page(
        ", ".join(
            [
                btn("a", 0, 0, "Mute", ", color: red"),
                btn("b", 1, 0, "Custom", ", color: '#3355FF'"),
                btn("c", 2, 0, "Plain"),
            ]
        )
    )
)
ALL = 15


def _vector():
    c = compile_config(parse_model(CFG, "t.yaml"))
    rp = c.for_caps(ALL)
    return {
        "note": "tests/py/test_theme_chain.py regenerates this (HH_REGEN_THEME_CHAIN=1); tests/c/test_remote_rf.c renders it",
        "gen": rp.gen,
        "theme": rp.theme_wire.hex(),
        "pages": [p.hex() for p in rp.pages_v2],
    }


def test_the_chain_vector_is_what_the_compiler_produces():
    v = _vector()
    if os.environ.get("HH_REGEN_THEME_CHAIN"):
        VECTOR.write_text(json.dumps(v, indent=1) + "\n")
    assert json.loads(VECTOR.read_text()) == v
    assert len(bytes.fromhex(v["theme"])) == 99 and len(v["pages"]) >= 1


pytestmark_e2e = pytest.mark.usefixtures("enable_custom_integrations", "fast_client")
USER = YAML + DUSK + "theme: dusk\n"


@pytest.fixture
def info_wait(monkeypatch):
    from custom_components.ha_harmony import const

    monkeypatch.setattr(const, "INFO_WAIT", 5.0)


@pytestmark_e2e
async def test_the_remote_sim_gets_theme_then_pages_and_the_set_is_complete(
    hass, hub_rf, fake_addon, built_binaries, info_wait
):
    entry = await setup(hass, hub_rf, fake_addon, yaml=USER)
    c = entry.runtime_data
    r = remote(built_binaries, hub_rf.rf, "--info", str(ALL), "--dump-layout", "--settle-ms", "1500")
    code, out = await asyncio.to_thread(r.result)
    lay = json.loads(out)
    assert code == 0 and lay["version"] == 2 and lay["theme_id"] == 7
    assert lay["theme"] == {"gen": lay["gen"], "hex": c.compiled.theme_wire.hex()}
    assert lay["theme_first"] is True and lay["themes_heard"] == 1
    assert lay["complete"] is True  # the remote's rule: its pages use theme 7 and the slot has that gen
    await hass.config_entries.async_unload(entry.entry_id)


@pytestmark_e2e
async def test_an_old_remote_without_bit_3_is_sent_pages_of_the_base_preset(
    hass, hub_rf, fake_addon, built_binaries, info_wait
):
    entry = await setup(hass, hub_rf, fake_addon, yaml=USER)
    r = remote(built_binaries, hub_rf.rf, "--info", "7", "--dump-layout", "--settle-ms", "1500")
    code, out = await asyncio.to_thread(r.result)
    lay = json.loads(out)
    assert code == 0 and lay["theme_id"] == 1 and "theme" not in lay and lay["themes_heard"] == 0
    assert lay["complete"] is True  # a preset set needs no theme
    await hass.config_entries.async_unload(entry.entry_id)


@pytestmark_e2e
async def test_a_restarted_remote_is_sent_the_theme_again(
    hass, hub_rf, fake_addon, built_binaries, info_wait
):
    entry = await setup(hass, hub_rf, fake_addon, yaml=USER)
    for _ in range(2):  # the second remote_sim is a remote that rebooted and lost its slot and store
        r = remote(built_binaries, hub_rf.rf, "--info", str(ALL), "--dump-layout", "--settle-ms", "1500")
        code, out = await asyncio.to_thread(r.result)
        lay = json.loads(out)
        assert code == 0 and lay["complete"] is True and lay["theme_first"] is True
        assert lay["theme"]["hex"] == entry.runtime_data.compiled.theme_wire.hex()
    assert entry.runtime_data.theme_pushes >= 2
    await hass.config_entries.async_unload(entry.entry_id)


