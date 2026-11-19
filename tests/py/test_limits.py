"""Hub config limits: the C header, the Python mirror and the frontend constants cannot drift; the
compiler checks against the limits a hub reports (or the old fixed ones); the estimate the hub
checks its budget against is the one clients compute."""

import json
import re
from pathlib import Path

import pytest

from harmony_core.compiler import (
    CompileError,
    compact_json,
    compile_config,
    config_stats,
    fit_error,
)
from harmony_core.config_model import parse_model
from harmony_core.hub_limits import CURRENT, LEGACY, HubLimits, estimate_bytes

ROOT = Path(__file__).resolve().parents[2]
NEC = "{proto: nec, addr: 4, cmd: 2}"

# header define -> HubLimits field
HEADER_FIELDS = {
    "MAX_DEVICES": "max_devices",
    "MAX_ACTIVITIES": "max_activities",
    "MAX_KEYS": "max_keys",
    "MAX_STEPS": "max_steps",
    "MAX_ACT_DEVS": "max_act_devs",
    "MAX_PRONTO": "max_pronto",
    "NAME_MAX": "name_max",
    "BUDGET_BYTES": "budget",
    "SLACK_BYTES": "slack_b",
    "DEV_BYTES": "dev_b",
    "CMD_BYTES": "cmd_b",
    "KEY_BYTES": "key_b",
    "WORD_BYTES": "word_b",
}


def _num(expr: str) -> int:
    expr = re.sub(r"(\d+)[uU]", r"\1", expr)
    assert re.fullmatch(r"[\d\s*]+", expr), expr
    out = 1
    for part in expr.split("*"):
        out *= int(part)
    return out


def test_python_limits_match_mapping_h():
    src = (ROOT / "hub/engine/mapping.h").read_text()
    defs = {
        m[1]: _num(m[2])
        for m in re.finditer(r"^#define HP_MAP_(\w+)\s+\(?([\d\s*uU]+?)\)?\s*(?:/\*.*)?$", src, re.MULTILINE)
    }
    defs.pop("ERR_MAX")
    assert set(defs) == set(HEADER_FIELDS), "a header limit has no Python mirror (or the reverse)"
    for name, field in HEADER_FIELDS.items():
        assert getattr(CURRENT, field) == defs[name], f"python {field} drifted from HP_MAP_{name}"
    chunk = re.search(r"#define HP_CFG_CHUNK_BYTES (\d+)", (ROOT / "hub/hub_handlers.h").read_text())
    assert chunk and CURRENT.chunk == int(chunk[1])


def test_hello_limits_keys_match_what_the_hub_sends():
    src = (ROOT / "hub/hub_handlers.c").read_text()
    body = src[src.index("static cJSON *on_limits") :]
    sent = set(re.findall(r'cJSON_AddNumberToObject\(o, "(\w+)"', body[: body.index("return o;")]))
    assert sent == set(CURRENT.to_wire())  # incl. flash_free (a runtime value: 0 in the model)


def test_frontend_legacy_constants_match():
    src = (ROOT / "addon/frontend/src/lib/model.ts").read_text()
    m = re.search(r"export const LEGACY_LIMITS = \{([^}]*)\}", src)
    assert m
    ts = {k: _num(v) for k, v in re.findall(r"(\w+): ([\d\s*]+)", m[1])}
    assert ts["devices"] == LEGACY.max_devices and ts["activities"] == LEGACY.max_activities
    assert ts["commands"] == LEGACY.max_commands and ts["inputs"] == LEGACY.max_inputs
    assert ts["jsonBytes"] == LEGACY.max_json


def test_legacy_numbers_are_the_old_firmwares():
    assert (LEGACY.max_commands, LEGACY.max_inputs, LEGACY.max_devices, LEGACY.pool_words) == (256, 64, 16, 16384)
    assert LEGACY.legacy and not CURRENT.legacy and CURRENT.chunk > 0 and LEGACY.chunk == 0


# ------------------------------------------------------------------ the estimate
def _c_golden() -> str:
    src = (ROOT / "tests/c/test_budget.c").read_text()
    lit = src[src.index("static const char *SMALL") :].split(";", 1)[0]
    return "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', lit)).encode().decode("unicode_escape")


def test_estimate_equals_the_c_golden():
    cfg = json.loads(_c_golden())
    stats = config_stats(cfg)
    assert stats == {"text": 135, "devices": 1, "commands": 2, "inputs": 0, "keys": 0, "words": 4}
    assert compact_json(cfg) == _c_golden()
    assert estimate_bytes(CURRENT, **stats) == 1573336  # tests/c/test_budget.c asserts the same number


def test_pool_words_and_keys_are_counted_like_the_hub():
    cfg = {
        "devices": [
            {"id": "a", "commands": {"x": {"pronto": "0000 006D 0000 0000"}, "y": {"raw": {"hz": 1, "us": [1, 2, 3]}}}}
        ],
        "keys": {"1": {"type": "none"}},
        "activities": [{"id": "z", "keys": {"2": {"type": "none"}, "3": {"type": "none"}}}],
    }
    s = config_stats(cfg)
    assert (s["words"], s["keys"], s["commands"]) == (7, 3, 2)


# ------------------------------------------------------------------ what a hub reports
def test_from_hello_roundtrips_and_distrusts_junk():
    assert HubLimits.from_hello(CURRENT.to_wire()) == CURRENT
    assert HubLimits.from_hello(None) is LEGACY and HubLimits.from_hello({}) is LEGACY
    assert HubLimits.from_hello("x") is LEGACY
    bad = CURRENT.to_wire() | {"budget": 5}  # below any real hub
    assert HubLimits.from_hello(bad) is LEGACY
    bad = CURRENT.to_wire() | {"dev_b": True}
    assert HubLimits.from_hello(bad) is LEGACY
    missing = {k: v for k, v in CURRENT.to_wire().items() if k != "chunk"}
    assert HubLimits.from_hello(missing) is LEGACY
    assert LEGACY.to_wire() == {}


# ------------------------------------------------------------------ the compiler
def _cmds(n, per=250):
    devs = []
    for d in range((n + per - 1) // per):
        count = min(per, n - d * per)
        cmds = ", ".join(f"c{j}: {NEC}" for j in range(count))
        devs.append(f"  - {{id: d{d}, commands: {{{cmds}}}}}")
    return parse_model("devices:\n" + "\n".join(devs) + "\n")


def test_legacy_hub_rejects_257_commands_current_accepts_thousands():
    m = _cmds(257)
    with pytest.raises(CompileError, match="too many commands: 257 .the hub allows at most 256"):
        compile_config(m)
    c = compile_config(_cmds(5000), limits=CURRENT)
    u = c.usage
    assert u["commands"] == 5000 and u["commands_max"] is None and u["json_max"] is None
    assert u["config_budget"] == CURRENT.budget and 0 < u["config_bytes"] < CURRENT.budget


def test_the_one_budget_and_its_message():
    lim = CURRENT.with_budget(2 * 1024 * 1024)
    with pytest.raises(CompileError, match=r"config needs ~\d+ KiB of hub memory, the hub allows 2048 KiB"):
        compile_config(_cmds(5000), limits=lim)
    c = compile_config(_cmds(20), limits=lim)
    assert c.usage["config_bytes"] <= 2 * 1024 * 1024


def test_per_item_bounds_remain_with_hub_values():
    with pytest.raises(CompileError, match="too many devices: 256"):
        compile_config(parse_model("devices:\n" + "".join(f"  - {{id: d{i}}}\n" for i in range(256))), limits=CURRENT)
    compile_config(parse_model("devices:\n" + "".join(f"  - {{id: d{i}}}\n" for i in range(255))), limits=CURRENT)
    tight = HubLimits.from_hello({**CURRENT.to_wire(), "max_devices": 3})
    with pytest.raises(CompileError, match="too many devices: 4 .the hub allows at most 3"):
        compile_config(parse_model("devices:\n" + "".join(f"  - {{id: d{i}}}\n" for i in range(4))), limits=tight)


def test_flash_is_checked_with_a_precise_message():
    c = compile_config(_cmds(1000), limits=CURRENT)
    stats = config_stats(c.hub_json)
    tight = HubLimits.from_hello({**CURRENT.to_wire(), "flash_free": 20 * 1024})
    assert tight.flash_free == 20 * 1024
    assert "KiB of flash, the hub has 20 KiB free" in fit_error(stats, tight)
    assert fit_error(stats, HubLimits.from_hello({**CURRENT.to_wire(), "flash_free": 4 * 1024 * 1024})) is None
    # none left is not "unknown": flash_free 0 + flash_low says "full", 0 alone still means unknown
    full = HubLimits.from_hello({**CURRENT.to_wire(), "flash_free": 0, "flash_low": True})
    assert full.flash_low and "flash is full" in fit_error(stats, full)
    assert full.to_wire()["flash_low"] is True and "flash_low" not in CURRENT.to_wire()
    unknown = HubLimits.from_hello({**CURRENT.to_wire(), "flash_free": 0})
    assert not unknown.flash_low and fit_error(stats, unknown) is None
    assert not HubLimits.from_hello({**CURRENT.to_wire(), "flash_free": 5, "flash_low": True}).flash_low
    # optional on the wire: an add-on stores the limits without it
    assert HubLimits.from_hello({k: v for k, v in CURRENT.to_wire().items() if k != "flash_free"}) == CURRENT


def test_fit_error_checks_a_compiled_config_against_another_hub():
    c = compile_config(_cmds(1000), limits=CURRENT)  # compiled before the hub reported itself
    stats = config_stats(c.hub_json)
    assert fit_error(stats, CURRENT) is None
    assert "too many commands: 1000" in fit_error(stats, LEGACY)
    assert "KiB of hub memory" in fit_error(stats, CURRENT.with_budget(100 * 1024))


def test_estimate_tracks_the_text_it_is_made_of():
    a = config_stats(compile_config(_cmds(100), limits=CURRENT).hub_json)
    b = config_stats(compile_config(_cmds(200), limits=CURRENT).hub_json)
    assert b["commands"] == 2 * a["commands"] and b["text"] > a["text"]
    assert estimate_bytes(CURRENT, **b) > estimate_bytes(CURRENT, **a)
