"""Device blocks, power candidates and the CLI, all against the fixture slice (no network)."""

import importlib.util
import json
import re
import sys
from pathlib import Path

import pytest
import yaml
from irdb_fake import FakeSession

from custom_components.ha_harmony.harmony_core import compiler
from custom_components.ha_harmony.harmony_core.config_model import parse_model
from harmony_irdb import device as dev_mod
from harmony_irdb.archive import ArchiveError, IrdbArchive, OfflineError
from harmony_irdb.candidates import power_candidates
from harmony_irdb.device import build_device, command_name, device_type_id, to_yaml

ROOT = Path(__file__).resolve().parents[2]
FIX = ROOT / "tests" / "py" / "fixtures" / "irdb"
SLUG = re.compile(r"^[a-z0-9_]{1,31}$")


def load(slug, file):
    d = json.loads((FIX / "devices" / slug / file).read_text())
    return d, json.loads((FIX / d["codeset"]).read_text()), d["codeset"]


def build(slug, file, **kw):
    d, cs, path = load(slug, file)
    return build_device(
        d,
        cs,
        mfr=slug,
        model_file=file,
        codeset_path=path,
        imported_at="2026-01-02T03:04:05Z",
        **kw,
    )


def test_limits_match_the_compiler():
    assert dev_mod.MAX_COMMANDS == compiler.LIMITS["MAX_COMMANDS"]
    assert dev_mod.MAX_INPUTS == compiler.LIMITS["MAX_INPUTS"]
    assert dev_mod.MAX_JSON == compiler.LIMITS["MAX_JSON"]


def test_sony_tv_block_validates_and_roundtrips():
    b = build("Sony", "KV-32V42.json")
    d = b.device
    assert d["id"] == "sony_kv_32v42" and d["name"] == "Sony KV-32V42"
    assert d["settle_ms"] == 1000
    assert d["power"]["on"]["proto"] == "sirc12" and d["power"]["off"]["proto"] == "sirc12"
    assert set(d["inputs"]) == {"Tuner", "Line In 1", "Line In 2"}
    assert d["source"] == {
        "kind": "harmony_archive",
        "mfr": "Sony",
        "model_file": "KV-32V42.json",
        "codeset": "codesets/13/13f9d15a6b4cb70c.json",
        "imported_at": "2026-01-02T03:04:05Z",
    }
    assert all(SLUG.match(n) for n in d["commands"])
    model = parse_model(to_yaml([d]), "<t>")  # the real config schema accepts it
    assert model["devices"][0]["commands"] == d["commands"]
    assert model["devices"][0]["source"]["kind"] == "harmony_archive"
    assert compiler.compile_config(model)  # and the compiler ignores `source`
    assert set(b.forms.values()) == {"proto"}


def test_yaml_style_matches_the_docs():
    text = to_yaml([build("Sony", "KV-32V42.json").device])
    assert "power: {on: {proto: sirc12" in text  # plain on/off keys, flow actions
    assert "'on'" not in text and "\n  - id: sony_kv_32v42" in text
    assert yaml.safe_load(text)["devices"][0]["id"] == "sony_kv_32v42"


def test_toshiba_mixed_forms_and_skip_report():
    b = build("Toshiba", "43D8UXA.json")
    assert b.device["power"] == {"toggle": b.device["commands"]["power_toggle"]}
    forms = set(b.forms.values())
    assert forms == {"proto", "pronto"}  # Toshiba NEC frames exact; Samsung/RC5 as pronto
    assert [s[0] for s in b.skipped] == ["Back"]
    assert "no pronto" in b.skipped[0][1]
    assert any("clamped" in n for n in b.notes)
    parse_model(to_yaml([b.device]), "<t>")


def test_dvd_toggle_with_sony20():
    b = build("Sony", "BDP-S590-231542.json", dev_id="dvd", name="Bluray")
    assert b.device["id"] == "dvd" and b.device["name"] == "Bluray"
    assert b.device["power"]["toggle"]["proto"] == "sirc20"
    assert "inputs" not in b.device


@pytest.mark.parametrize(
    "raw,expected",
    [
        ("PowerOn", "ir_power_on"),  # reserved built-in name
        ("PowerToggle", "power_toggle"),
        ("InputHdmi1", "ir_input_hdmi1"),  # reserved prefix
        ("VolumeUp", "volume_up"),
        ("Volume+", "volume_plus"),
        ("10+", "10_plus"),
        (".", "dot"),
        ("-", "minus"),
        ("Ä Ö", "cmd"),
        ("x" * 80, "x" * 31),
    ],
)
def test_command_names(raw, expected):
    assert command_name(raw, set()) == expected
    assert SLUG.match(command_name(raw, set()))


def test_command_names_reserved_and_collisions_are_deterministic():
    names = [
        "power_on",
        "power_off",
        "Input_X",
        "VolumeUp",
        "volume up",
        "Volume-Up",
        "A" * 40,
        "a" * 40,
    ]
    used: set[str] = set()
    out = [command_name(n, used) for n in names]
    again: set[str] = set()
    assert out == [command_name(n, again) for n in names]  # same input, same names
    assert len(set(out)) == len(out) and all(SLUG.match(n) for n in out)
    assert out[:2] == ["ir_power_on", "ir_power_off"] and out[2] == "ir_input_x"
    assert out[3:6] == ["volume_up", "volume_up_2", "volume_up_3"]
    assert out[6] == "a" * 31 and out[7] == "a" * 29 + "_2"


def test_command_cap_and_size_cap(monkeypatch):
    d, cs, path = load("Sony", "KV-32V42.json")
    monkeypatch.setattr(dev_mod, "MAX_COMMANDS", 3)
    b = build_device(d, cs, mfr="S", model_file="f.json", codeset_path=path, imported_at="x")
    assert len(b.device["commands"]) == 3
    assert any("commands limit" in why for _, why in b.skipped)
    monkeypatch.setattr(dev_mod, "MAX_COMMANDS", 256)
    monkeypatch.setattr(dev_mod, "MAX_JSON", 300)
    b = build_device(d, cs, mfr="S", model_file="f.json", codeset_path=path, imported_at="x")
    assert any("size limit" in why for _, why in b.skipped)
    assert len(json.dumps(b.device["commands"], separators=(",", ":"))) <= 300


def test_hostile_archive_fields_do_not_break_the_builder():
    d = {
        "model": 5,
        "power": {"on": "PowerOn", "off": [1, None], "toggle": {"x": 1}},
        "inputs": {"list": "x"},
    }
    cs = {
        "commands": [
            None,
            {"name": ""},
            {"name": "A", "protocol": "Sony 12 Bit", "keycode": "k"},
            5,
        ]
    }
    b = build_device(d, cs, mfr="M", model_file="m.json", codeset_path="p", imported_at="x")
    assert b.device["commands"] == {} and len(b.skipped) == 4 and "power" not in b.device
    parse_model(to_yaml([b.device]), "<t>")


def test_device_type_names():
    assert [device_type_id(x) for x in (1, "1", "TV", "dvd player", "Television", "nope", True)] == [
        1, 1, 1, 4, 1, None, None,
    ]  # fmt: skip


# ---- candidates
def archive(tmp_path, session=None):
    return IrdbArchive(session or FakeSession(), tmp_path / "c")


async def test_candidates_grouped_and_ranked(tmp_path):
    s = FakeSession()
    res = await power_candidates(archive(tmp_path, s), "Sony", "TV")
    assert (res.examined, res.matched) == (4 + 1, 3)  # 4 files + index entry that does not exist
    assert len(res.errors) == 1 and "device-234951" in res.errors[0]
    counts = [(c.kind, c.count) for c in res.candidates]
    assert counts[0][1] >= counts[-1][1] and counts == sorted(counts, key=lambda kc: -kc[1]), counts
    on = next(c for c in res.candidates if c.kind == "on")
    off = next(c for c in res.candidates if c.kind == "off")
    assert on.count == 3 and off.count == 3  # all three TVs share Sony's PowerOn/PowerOff
    assert on.codesets == 2  # two distinct codesets (13f9..., 739b...)
    assert on.action["proto"] == "sirc12" and on.form == "proto"
    assert on.models[:2] == ["KV-32V42", "KV-29FV15B"] and len(set(on.models)) == len(on.models)
    # the shared codeset is fetched once, not once per model
    cs_urls = [u for u in s.urls() if "codesets/13/" in u]
    assert len(cs_urls) == 1
    assert not any(c.kind == "toggle" for c in res.candidates)  # no TV here uses a toggle


async def test_candidates_by_type_and_distinct_codes(tmp_path):
    res = await power_candidates(archive(tmp_path), "Sony", "DVD Player")
    assert (res.matched, [(c.kind, c.count) for c in res.candidates]) == (1, [("toggle", 1)])
    res = await power_candidates(archive(tmp_path), "Toshiba", 1)
    assert [(c.kind, c.count, c.form) for c in res.candidates] == [("toggle", 1, "proto")]
    with pytest.raises(ArchiveError, match="device type"):
        await power_candidates(archive(tmp_path), "Sony", "toaster")


async def test_candidates_limit_and_offline(tmp_path):
    res = await power_candidates(archive(tmp_path), "Sony", "TV", limit=1)
    assert res.examined == 1 and res.candidates[0].count == 1
    offline = IrdbArchive(FakeSession(), tmp_path / "empty", offline=True)
    with pytest.raises(OfflineError):
        await power_candidates(offline, "Sony", "TV")


# ---- CLI
@pytest.fixture
def cli(monkeypatch, tmp_path):
    spec = importlib.util.spec_from_file_location(
        "irdb_import_tool", ROOT / "tools" / "irdb_import.py"
    )
    mod = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = mod
    spec.loader.exec_module(mod)
    session = FakeSession()

    class Ctx:
        async def __aenter__(self):
            return session

        async def __aexit__(self, *a):
            return False

    import aiohttp

    monkeypatch.setattr(aiohttp, "ClientSession", lambda *a, **k: Ctx())
    return lambda *argv: mod.main(["--cache-dir", str(tmp_path / "cache"), *argv]), session


def test_cli_import_output_validates(cli, capsys):
    run, _ = cli
    assert run("import", "sony", "KV-32V42") == 0
    out, err = capsys.readouterr()
    model = parse_model(out, "<cli>")
    d = model["devices"][0]
    assert d["id"] == "sony_kv_32v42" and d["source"]["codeset"].startswith("codesets/13/")
    assert re.fullmatch(r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ", d["source"]["imported_at"])
    assert "commands imported" in err and ", 0 skipped" in err


def test_cli_import_to_file_with_skip_report(cli, capsys, tmp_path):
    run, _ = cli
    out_file = tmp_path / "t.yaml"
    assert run("import", "Toshiba", "43D8UXA.json", "--out", str(out_file), "--id", "tv") == 0
    out, err = capsys.readouterr()
    assert out == ""
    assert parse_model(out_file.read_text(), "<f>")["devices"][0]["id"] == "tv"
    assert "skipped Back: no pronto" in err and "clamped" in err


def test_cli_search_commands(cli, capsys):
    run, _ = cli
    assert run("brands", "sony") == 0
    assert capsys.readouterr().out.startswith("Sony\t")
    assert run("models", "Sony", "bdp") == 0
    assert "BDP-S590\tBDP-S590-231542.json" in capsys.readouterr().out
    assert run("candidates", "Sony", "--type", "TV") == 0
    assert "sirc12" in capsys.readouterr().out


def test_cli_errors_are_clean(cli, capsys):
    run, _ = cli
    assert run("import", "Sony", "no such model") == 1
    assert capsys.readouterr().err.startswith("error:")
    assert run("import", "zzzzqqq", "x") == 1
    assert "error:" in capsys.readouterr().err


def test_cli_offline_without_cache(cli, capsys):
    run, session = cli
    assert run("--offline", "models", "Sony") == 1
    assert "offline" in capsys.readouterr().err and session.calls == []
