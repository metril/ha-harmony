"""tools/stock_to_ha_config.py: synthetic stock data -> config the real pipeline accepts."""

import importlib.util
import json
import sys
from pathlib import Path

import pytest
import yaml

from custom_components.ha_harmony.compiler import compile_config
from custom_components.ha_harmony.config_model import parse_model

ROOT = Path(__file__).resolve().parents[2]
_spec = importlib.util.spec_from_file_location(
    "stock_to_ha_config", ROOT / "tools/stock_to_ha_config.py"
)
tool = importlib.util.module_from_spec(_spec)
sys.modules["stock_to_ha_config"] = tool
_spec.loader.exec_module(tool)

FRAME = [4500, 4500, 560, 1690, 560, 560]
NEC_ONCE = [8990, 4490, 568, 552, 568, 1662]
NEC_REP = [8990, 2230, 568, 96077]


def ir(once, repeat, finish=(), hz=38000, mr=3):
    return {
        "transport": "ir",
        "protocol": "P",
        "carrier_hz": hz,
        "once": list(once),
        "repeat": list(repeat),
        "finish": list(finish),
        "min_repeats": mr,
    }


def stock(**devices):
    return {"warnings": [], "devices": devices}


@pytest.fixture
def data():
    return stock(
        **{
            "111": {
                "Power": ir([], FRAME, hz=37900),
                "Mute": ir(NEC_ONCE, NEC_REP),
                "Once": ir(NEC_ONCE, []),
            },
            "222": {"Home": {"transport": "hid", "key": "07000027"}},
            "333": {"Home": {"transport": "roku_ecp", "key": "Home"}},
            "444": {},
        }
    )


def run(data, tmp_path, *args, name="stock.json"):
    src = tmp_path / "ir_codes.json"
    src.write_text(json.dumps(data))
    out = tmp_path / name
    rc = tool.main([str(src), "--out", str(out), *args])
    return rc, out


def cmds(conv, dev):
    return next(d for d in conv.config["devices"] if d["id"] == dev)["commands"]


def test_single_mapping_preserves_carrier_and_durations(data):
    conv = tool.convert(data, names={"111": ("tv", "TV")})
    c = cmds(conv, "tv")
    assert c["Power"] == {"raw": {"hz": 37900, "us": FRAME}}  # frame lived in `repeat`
    assert c["Mute"] == {"raw": {"hz": 38000, "us": NEC_ONCE}}  # `once` is the press frame
    assert c["Once"]["raw"]["us"] == NEC_ONCE
    notes = {r.command: r.notes for r in conv.results}
    assert "repeat_frame_not_expressible" in notes["Mute"]
    assert "min_repeats_not_expressed" in notes["Power"]


def test_expand_mapping(data):
    conv = tool.convert(data, mode="expand", names={"111": ("tv", "TV")})
    c = cmds(conv, "tv")
    assert c["Power"]["raw"]["us"] == FRAME * 3
    mute = c["Mute"]["raw"]["us"]
    assert mute == NEC_ONCE + NEC_REP[:-1] + [65535] + (NEC_REP[:-1] + [65535]) * 2
    assert any("clamped" in n for r in conv.results for n in r.notes)
    assert c["Once"]["raw"]["us"] == NEC_ONCE


def test_expand_appends_finish():
    d = stock(**{"1": {"X": ir([10, 20], [30, 40], finish=[50, 60], mr=2)}})
    us = cmds(tool.convert(d, mode="expand"), "dev_1")["X"]["raw"]["us"]
    assert us == [10, 20, 30, 40, 30, 40, 50, 60]
    r = tool.convert(d).results[0]
    assert {"finish_not_expressible", "repeat_frame_not_expressible"} <= set(r.notes)


def test_output_validates_against_real_config_model(data, tmp_path):
    for fmt, name in (("yaml", "o.yaml"), ("json", "o.json")):
        for mode in ("single", "expand"):
            rc, out = run(data, tmp_path, "--repeats", mode, "--format", fmt, name=name)
            assert rc == 0
            model = parse_model(out.read_text(), str(out))
            compile_config(model)
            (dev,) = model["devices"]
            assert dev["id"] == "dev_111"
            assert set(dev["commands"]) == {"Power", "Mute", "Once"}
            raw = dev["commands"]["Power"]["raw"]
            assert raw["hz"] == 37900 and raw["us"][:6] == FRAME
            assert yaml.safe_load(out.read_text()) is not None


def test_skips_warn_and_counts_reconcile(data, tmp_path, capsys):
    rc, out = run(data, tmp_path)
    err = capsys.readouterr().err
    assert rc == 0
    assert "skipped 222/Home: hid" in err and "skipped 333/Home: roku_ecp" in err
    assert "warning: device 444" in err
    assert "total: 5 in = 3 converted + 2 skipped + 0 errors" in err
    assert "reconciliation: converted + skipped + errors == input: OK" in err
    assert "222" not in out.read_text()  # no IR commands -> device omitted, but reported
    assert "devices omitted" in err


def test_only_ir_suppresses_per_command_but_counts(data, tmp_path, capsys):
    rc, _ = run(data, tmp_path, "--only-ir")
    err = capsys.readouterr().err
    assert rc == 0 and "skipped 222/Home" not in err
    assert "2 skipped" in err and "skipped by transport" in err


def test_report_file(data, tmp_path):
    rpt = tmp_path / "r.json"
    rc, _ = run(data, tmp_path, "--report", str(rpt))
    r = json.loads(rpt.read_text())
    assert rc == 0 and r["summary"]["total"]["input"] == 5
    assert {c["status"] for c in r["commands"]} == {"converted", "skipped"}


@pytest.mark.parametrize(
    "entry",
    [
        {
            "transport": "ir",
            "carrier_hz": 38000,
            "once": [1, 2, 3],
            "repeat": [],
            "finish": [],
            "min_repeats": 1,
        },  # odd count
        {
            "transport": "ir",
            "carrier_hz": 38000,
            "once": [],
            "repeat": [],
            "finish": [],
            "min_repeats": 1,
        },  # empty
        {
            "transport": "ir",
            "carrier_hz": 38000,
            "once": [70000, 1],
            "repeat": [],
            "finish": [],
            "min_repeats": 1,
        },  # mark too long
        {
            "transport": "ir",
            "carrier_hz": 5,
            "once": [1, 1],
            "repeat": [],
            "finish": [],
            "min_repeats": 1,
        },
        {
            "transport": "ir",
            "carrier_hz": 38000,
            "once": [0, 1],
            "repeat": [],
            "finish": [],
            "min_repeats": 1,
        },
        {
            "transport": "ir",
            "carrier_hz": 38000,
            "once": [1, 1] * 300,
            "repeat": [],
            "finish": [],
            "min_repeats": 1,
        },  # too long for the hub
        {"transport": "bluetooth", "key": "x"},
        {"garbage": True},
        "just a string",
    ],
)
def test_unconvertible_fails_loudly(entry, tmp_path, capsys):
    d = stock(**{"1": {"Good": ir([], FRAME), "Bad": entry}})
    rc, out = run(d, tmp_path)
    err = capsys.readouterr().err
    assert rc == 1 and not out.exists()
    assert "ERROR: 1/Bad" in err and "1 errors" in err


def test_duplicate_command_names_are_errors(tmp_path, capsys):
    src = tmp_path / "ir_codes.json"
    one = json.dumps(ir([], FRAME))
    src.write_text('{"devices": {"1": {"A": ' + one + ', "A": ' + one + "}}}")
    rc = tool.main([str(src), "--out", str(tmp_path / "o.yaml")])
    assert rc == 1 and "duplicate command name" in capsys.readouterr().err


def test_reserved_and_long_names_are_errors(tmp_path):
    d = stock(**{"1": {"power_on": ir([], FRAME), "x" * 32: ir([], FRAME), "ok": ir([], FRAME)}})
    conv = tool.convert(d)
    errs = {r.command: r.reason for r in conv.results if r.status == "error"}
    assert set(errs) == {"power_on", "x" * 32}
    assert tool.summarise(conv)["total"]["input"] == 3


def test_device_filter_and_names(data, tmp_path):
    _rc, out = run(data, tmp_path, "--device", "111", "--device-name", "111=tv:Living TV")
    model = parse_model(out.read_text())
    assert model["devices"][0]["id"] == "tv" and model["devices"][0]["name"] == "Living TV"
    with pytest.raises(SystemExit):
        run(data, tmp_path, "--device", "999")


def test_stock_names_not_display_names():
    d = stock(**{"1": {"Number0": dict(ir([], FRAME), name="0")}})
    assert list(cmds(tool.convert(d), "dev_1")) == ["Number0"]


def test_accepts_extracted_dir_and_coverage_mismatch(data, tmp_path, capsys):
    (tmp_path / "ir_codes.json").write_text(json.dumps(data))
    (tmp_path / "coverage.json").write_text(
        json.dumps({"devices": {"222": {"commands": 9}, "111": {"commands": 3}}})
    )
    out = tmp_path / "o.yaml"
    assert tool.main([str(tmp_path), "--out", str(out)]) == 0
    assert "coverage.json counts 9 commands for 222" in capsys.readouterr().err
