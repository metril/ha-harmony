"""tools/stock_to_ha_config.py: synthetic stock data -> config the real pipeline accepts."""

import importlib.util
import json
import sys
from pathlib import Path

import pytest
import yaml

from custom_components.ha_harmony.harmony_core.compiler import compile_config
from custom_components.ha_harmony.harmony_core.config_model import parse_model

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
    assert c["Power"] == {
        "raw": {"hz": 37900, "us": FRAME},
        "repeats": 3,
    }  # frame lived in `repeat`
    assert c["Mute"] == {"raw": {"hz": 38000, "us": NEC_ONCE}}  # `once` is the press frame
    assert c["Once"]["raw"]["us"] == NEC_ONCE
    notes = {r.command: r.notes for r in conv.results}
    assert "repeat_frame_not_expressible" in notes["Mute"]
    assert "min_repeats_as_repeats" in notes["Power"]
    assert c["Power"]["repeats"] == 3 and "repeats" not in c["Mute"]  # min_repeats 3 -> repeats: 3


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


# ----------------------------------------------------------------------------- power, inputs, activities
# Synthetic stock DeviceList.json / engine config, shaped like the real ones (the real files are
# gitignored device data; tests/py/test_real_config.py runs against them where they exist).
TV_ON, TV_OFF, TV_H1, TV_H3, TV_TV = (
    [9000, 4500, 560, 560], [9000, 4500, 560, 1690], [9000, 4500, 1690, 560],
    [9000, 4500, 1690, 1690], [9000, 4500, 560, 300],
)  # fmt: skip


def press(cmd):
    return [{"__type": "IRPressAction", "IRCommandName": cmd, "Order": 0, "Duration": None}]


def power_feature(dev_id, *, on=None, off=None, toggle=None, delay=0):
    return {
        "__type": "PowerFeature", "DeviceId-": int(dev_id), "PowerOnDelay": delay,
        "PowerOnActions": press(on) if on else None,
        "PowerOffActions": press(off) if off else None,
        "PowerToggleActions": press(toggle) if toggle else None,
    }  # fmt: skip


def input_feature(dev_id, *pairs):
    return {
        "__type": "InputFeature", "DeviceId-": int(dev_id),
        "Inputs": [{"InputName": lbl, "Actions": act} for lbl, act in pairs],
    }  # fmt: skip


@pytest.fixture
def ext():
    """IR database + DeviceList + engine config for a TV (discrete power, inputs), a toggle
    switch, a manually powered amp, and a Bluetooth-only console."""
    ir_db = stock(
        **{
            "51886472": {
                "PowerOn": ir([], TV_ON), "PowerOff": ir([], TV_OFF),
                "InputHdmi1": ir([], TV_H1), "InputHdmi3": ir([], TV_H3),
                "InputTV": ir([], TV_TV),  # stock's IR database spells it InputTV ...
            },
            "64694265": {"PowerToggle": ir([], FRAME)},
            "51887071": {"Mute": ir([], FRAME)},
            "67368870": {"Home": {"transport": "hid", "key": "07000027"}},
        }
    )  # fmt: skip
    device_list = {
        "DevicesWithFeatures": [
            {"Device": {"Name": "Samsung TV", "Transport": 1}, "DeviceFeatures": [
                power_feature("51886472", on="PowerOn", off="PowerOff", delay=5000),
                input_feature(
                    "51886472",
                    ("HDMI 1", press("InputHdmi1")), ("HDMI 3", press("InputHdmi3")),
                    ("TV", press("InputTv")),  # ... while DeviceList spells it InputTv
                    ("HDMI 4(with HDMI 1,HDMI 2,HDMI 3,HDMI 4 Active)", press("InputHdmi3")),
                    ("SmartHub", press("Exit") + press("Exit")),  # a two-press sequence
                ),
            ]},
            {"Device": {"Name": "J-Tech", "Transport": 1}, "DeviceFeatures": [
                power_feature("64694265", toggle="PowerToggle", delay=1500),
            ]},
            {"Device": {"Name": "Amp", "Transport": 1}, "DeviceFeatures": [
                power_feature("51887071", delay=500),  # manual power: no actions at all
            ]},
            {"Device": {"Name": "Console", "Transport": 32}, "DeviceFeatures": [
                power_feature("67368870", on="Home", off="Sleep", delay=1500),
            ]},
        ]
    }  # fmt: skip
    engine = {
        "data": {
            "activity": [
                {"id": "1", "label": "SHIELD TV", "type": "VirtualDvd", "fixit": {
                    "51886472": {"Power": "On", "Input": "HDMI 1"},
                    "67368870": {"Power": "On"},
                    "64694265": {"Power": "Off", "isRelativePower": True},
                }},
                {"id": "2", "label": "Watch Roku", "type": "VirtualGeneric", "fixit": {
                    "51886472": {"Power": "On", "Input": "HDMI 3"},
                }},
                {"id": "-1", "label": "PowerOff", "type": "PowerOff", "fixit": {
                    "51886472": {"Power": "Off"},
                }},
            ]
        }
    }  # fmt: skip
    return ir_db, device_list, engine


def run_ext(ext, tmp_path, *, activities=True):
    ir_db, device_list, engine = ext
    src, dl, ec = tmp_path / "ir_codes.json", tmp_path / "DeviceList.json", tmp_path / "ec.json"
    src.write_text(json.dumps(ir_db))
    dl.write_text(json.dumps(device_list))
    ec.write_text(json.dumps(engine))
    out = tmp_path / "out.yaml"
    args = [str(src), "--out", str(out), "--device-list", str(dl)]
    if activities:
        args += ["--engine-config", str(ec)]
    rc = tool.main(args)
    return rc, (yaml.safe_load(out.read_text()) if out.exists() else None)


def dev(cfg, slug):
    return next(d for d in cfg["devices"] if d["id"] == slug)


def test_without_the_new_flags_output_is_unchanged(data, tmp_path):
    """Back-compat: no --device-list means devices + commands only, exactly as before."""
    rc, out = run(data, tmp_path)
    cfg = yaml.safe_load(out.read_text())
    assert rc == 0 and "activities" not in cfg
    assert all(set(d) == {"id", "name", "commands"} for d in cfg["devices"])


def test_discrete_power_and_settle_come_from_stock(ext, tmp_path):
    rc, cfg = run_ext(ext, tmp_path)
    tv = dev(cfg, "samsung_tv")
    assert rc == 0
    assert tv["power"] == {"on": tv["commands"]["PowerOn"], "off": tv["commands"]["PowerOff"]}
    assert tv["settle_ms"] == 5000


def test_toggle_power_is_kept_as_a_toggle(ext, tmp_path):
    _, cfg = run_ext(ext, tmp_path)
    jt = dev(cfg, "jtech_switch")
    assert jt["power"] == {"toggle": jt["commands"]["PowerToggle"]} and jt["settle_ms"] == 1500


def test_manual_power_gets_no_power_block_and_no_settle(ext, tmp_path):
    _, cfg = run_ext(ext, tmp_path)
    amp = dev(cfg, "audioengine_a5")
    assert "power" not in amp and "settle_ms" not in amp


def test_inputs_match_stock_labels_and_resolve_case_clashes(ext, tmp_path):
    _, cfg = run_ext(ext, tmp_path)
    tv = dev(cfg, "samsung_tv")
    assert list(tv["inputs"]) == ["HDMI 1", "HDMI 3", "TV"]
    assert tv["inputs"]["TV"] == tv["commands"]["InputTV"], "InputTv must resolve to InputTV"


def test_case_insensitive_match_refuses_ambiguity():
    assert tool._resolve_cmd("InputTv", {"InputTV": 1}) == "InputTV"
    assert tool._resolve_cmd("InputTv", {"InputTV": 1, "inputtv": 2}) is None
    assert tool._resolve_cmd("InputTv", {"InputTv": 1, "InputTV": 2}) == "InputTv"  # exact wins


def test_unexpressible_inputs_are_reported_not_dropped(ext, tmp_path, capsys):
    _, cfg = run_ext(ext, tmp_path)
    err = capsys.readouterr().err
    assert "HDMI 4(with HDMI 1,HDMI 2,HDMI 3,HDMI 4 Active)" in err and "31 bytes" in err
    assert "'SmartHub': not a single IR press" in err
    assert "SmartHub" not in dev(cfg, "samsung_tv")["inputs"]


def test_activities_list_only_devices_stock_wants_on(ext, tmp_path):
    _, cfg = run_ext(ext, tmp_path)
    acts = {a["name"]: a for a in cfg["activities"]}
    assert list(acts) == ["SHIELD TV", "Watch Roku"], "PowerOff maps to the built-in off"
    assert acts["SHIELD TV"] == {
        "id": "shield_tv",
        "name": "SHIELD TV",
        "devices": [{"device": "samsung_tv", "input": "HDMI 1"}],
    }
    assert acts["Watch Roku"]["devices"] == [{"device": "samsung_tv", "input": "HDMI 3"}]


def test_a_device_the_hub_cannot_drive_is_reported_for_ha(ext, tmp_path, capsys):
    """The console is Bluetooth-only: stock powers it, the hub cannot -- say so, loudly."""
    _, cfg = run_ext(ext, tmp_path)
    err = capsys.readouterr().err
    assert "wants Console ON" in err and "(transport 32)" in err  # DeviceList's own name
    assert "Home Assistant automation" in err
    shield = cfg["activities"][0]["devices"]
    assert all(d["device"] != "nvidia_shield" for d in shield)


def test_engine_config_requires_device_list(data, tmp_path):
    src = tmp_path / "ir_codes.json"
    src.write_text(json.dumps(data))
    with pytest.raises(SystemExit):
        tool.main([str(src), "--out", str(tmp_path / "o.yaml"), "--engine-config", "x.json"])


def test_extended_output_compiles_and_counts_power_and_input_words(ext, tmp_path, capsys):
    rc, cfg = run_ext(ext, tmp_path)
    assert rc == 0
    compile_config(parse_model(yaml.safe_dump(cfg)))  # the real pipeline accepts it
    # pool words now include power + inputs, which each carry their own copy of a raw code
    cmd_words = sum(len(c["raw"]["us"]) for d in cfg["devices"] for c in d["commands"].values())
    reported = int(capsys.readouterr().err.split("bytes, ")[1].split(" pool")[0])
    assert reported > cmd_words, "pool count must include power and input codes"


def test_slugify():
    assert tool._slugify("SHIELD TV") == "shield_tv"
    assert tool._slugify("Watch  Roku!") == "watch_roku"
    assert tool._slugify("***") == "activity"
    assert len(tool._slugify("x" * 50)) == 31


def test_stock_id_and_stock_name_become_aliases(ext, tmp_path):
    """So `remote.send_command device: "51886472"` from a stock-era automation keeps working."""
    _, cfg = run_ext(ext, tmp_path)
    assert dev(cfg, "samsung_tv")["aliases"] == ["51886472", "Samsung TV"]
    assert dev(cfg, "jtech_switch")["aliases"] == ["64694265", "J-Tech"]
