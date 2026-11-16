"""fetch_stock_config parsing/normalising against canned hub replies (no network)."""

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import fetch_stock_config as fsc


def fn(name, action, label=None):
    return {"name": name, "label": label or name, "action": action}


def ir(dev, cmd):
    return json.dumps({"command": cmd, "type": "IRCommand", "deviceId": dev})


CONFIG = {
    "cmd": fsc.CFG,
    "code": 200,
    "id": "2",
    "msg": "OK",
    "data": {
        "device": [
            {
                "id": "1",
                "label": "TV",
                "type": "Television",
                "manufacturer": "Acme",
                "model": "T1",
                "controlGroup": [
                    {
                        "name": "Power",
                        "function": [
                            fn("PowerOn", ir("1", "PowerOn")),
                            fn("PowerOff", ir("1", "PowerOff")),
                        ],
                    },
                    {"name": "Volume", "function": [fn("Mute", ir("1", "Mute"))]},
                ],
            },
            {
                "id": "2",
                "label": "Odd",
                "controlGroup": [
                    {
                        "name": "X",
                        "function": [
                            fn("Foo", json.dumps({"command": "Foo", "type": "RFCommand"})),
                            fn("Bar", "not json"),
                            fn("Baz", None),
                        ],
                    }
                ],
            },
            {"id": "3", "label": "Empty", "controlGroup": []},
            {"id": "4", "label": "NoGroups"},
        ],
        "activity": [
            {"id": "-1", "label": "PowerOff"},
            {"id": "9", "label": "Watch", "fixit": {"1": {"Power": "On"}}},
        ],
    },
}


def test_normalise_devices_activities():
    devices, acts, codes, warns = fsc.normalise(fsc.reply_data(CONFIG))
    assert [d["id"] for d in devices] == ["1", "2", "3", "4"]
    assert devices[0]["command_count"] == 3
    assert [a["id"] for a in acts] == ["-1", "9"]
    assert acts[1]["fixit"] == {"1": {"Power": "On"}}
    assert set(codes["1"]) == {"PowerOn", "PowerOff", "Mute"}
    assert codes["1"]["Mute"]["payload"] == {
        "command": "Mute",
        "type": "IRCommand",
        "deviceId": "1",
    }
    assert codes["1"]["Mute"]["group"] == "Volume"
    assert not [w for w in warns if w["device"] == "1"]


def test_unknown_encoding_kept_raw_with_warning():
    _, _, codes, warns = fsc.normalise(fsc.reply_data(CONFIG))
    assert set(codes["2"]) == {"Foo", "Bar", "Baz"}  # nothing dropped
    assert codes["2"]["Foo"]["type"] == "RFCommand"
    assert codes["2"]["Bar"]["payload"] == "not json"
    assert codes["2"]["Baz"]["payload"] is None
    assert {w["command"] for w in warns if w["device"] == "2"} == {"Foo", "Bar", "Baz"}


def test_device_without_commands():
    devices, _, codes, warns = fsc.normalise(fsc.reply_data(CONFIG))
    assert codes["3"] == {} and codes["4"] == {}
    assert devices[2]["command_count"] == 0
    assert not [w for w in warns if w["device"] in ("3", "4")]


def test_duplicate_command_kept():
    dev = {
        "id": "5",
        "controlGroup": [
            {"name": "A", "function": [fn("Ok", ir("5", "Ok"))]},
            {"name": "B", "function": [fn("Ok", ir("5", "Ok"))]},
        ],
    }
    codes = fsc.device_commands(dev, [])
    assert set(codes) == {"Ok", "Ok@B"}


def test_normalise_garbage():
    assert fsc.normalise(None) == ([], [], {}, [])


def test_helpers():
    assert fsc.safe_name(fsc.CFG) == "vnd.logitech.harmony_vnd.logitech.harmony.engine_config"
    assert fsc.reply_data({"code": 200}) == {"code": 200}
    m = fsc.build_message("7", "c", {"a": 1}, 3, 60)
    assert m == {"hubId": "7", "timeout": 60, "hbus": {"cmd": "c", "id": "3", "params": {"a": 1}}}


def test_queries_are_read_only():
    for cmd, _ in fsc.QUERIES:
        assert cmd.endswith(("?config", "?getCurrentActivity", "?get", "?getProvisionInfo"))


def test_write_outputs(tmp_path):
    digest = {"data": {"hubSwVersion": "4.15.600"}}
    replies = {fsc.CFG: CONFIG, "vnd.logitech.connect/vnd.logitech.statedigest?get": digest}
    statuses = {fsc.CFG: {"status": "ok", "code": 200}}
    man = fsc.write_outputs(tmp_path, "h", "7", replies, statuses)
    assert man["firmware"] == "4.15.600" and man["warnings"] == 3
    raw = tmp_path / "raw" / (fsc.safe_name(fsc.CFG) + ".json")
    assert json.loads(raw.read_text()) == CONFIG
    assert len(json.loads((tmp_path / "devices.json").read_text())) == 4
    ir_out = json.loads((tmp_path / "ir_codes.json").read_text())
    assert len(ir_out["warnings"]) == 3 and "PowerOn" in ir_out["devices"]["1"]
    assert json.loads((tmp_path / "manifest.json").read_text())["host"] == "h"
