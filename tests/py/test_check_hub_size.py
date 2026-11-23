"""tools/check_hub_size.py: the hub firmware size budget (stripped, under 600 KiB, OTA fits, build-id tag kept)."""

import importlib.util
import sys
from pathlib import Path

SPEC = importlib.util.spec_from_file_location(
    "check_hub_size", Path(__file__).parents[2] / "tools" / "check_hub_size.py"
)
mod = importlib.util.module_from_spec(SPEC)
sys.modules["check_hub_size"] = mod
SPEC.loader.exec_module(mod)


def blob(tmp_path, size, extra=b"HPBUILDID=7;"):
    p = tmp_path / "hub"
    p.write_bytes(extra + b"\0" * (size - len(extra)))
    return p


def test_slack_is_read_from_the_ota_header():
    assert mod.ota_slack() == 64 * 1024


def test_a_small_stripped_binary_passes(tmp_path):
    ok, lines = mod.check(blob(tmp_path, 500 * 1024))
    assert ok and "OTA needs" in lines[1]


def test_over_budget_fails(tmp_path):
    ok, lines = mod.check(blob(tmp_path, 601 * 1024))
    assert not ok and any("over the 600 KiB budget" in ln for ln in lines)


def test_unstripped_fails(tmp_path):
    ok, lines = mod.check(blob(tmp_path, 1000, extra=b"HPBUILDID=7;.symtab"))
    assert not ok and any("not stripped" in ln for ln in lines)


def test_missing_build_tag_fails(tmp_path):
    ok, lines = mod.check(blob(tmp_path, 1000, extra=b"x"))
    assert not ok and any("HPBUILDID" in ln for ln in lines)


def test_ota_need_beyond_free_space_fails(tmp_path):
    ok, lines = mod.check(blob(tmp_path, 500 * 1024), data_kib=1000)
    assert not ok and any("exceeds" in ln for ln in lines)
