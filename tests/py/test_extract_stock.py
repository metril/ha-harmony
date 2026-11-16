"""extract_stock against synthetic DeviceList/ProtocolList fixtures (no hardware, no real data)."""

import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import extract_stock as ex
import ir_ref


def atom(v, mark):
    return {"Value": v, "Type": 1 if mark else 0, "MinValue": None, "MaxValue": None}


def pcm_protocol(name, hdr, zero, one, trailer, total, carrier, **kw):
    seg = {
        "Name": name,
        "Header": [atom(hdr[0], 1), atom(hdr[1], 0)],
        "Payload": {
            "NumberOfBits": 32,
            "EncodingType": 0,
            "ToggleBit": None,
            "Encodings": [
                {"BitType": 0, "Atoms": [atom(zero[0], 1), atom(zero[1], 0)]},
                {"BitType": 1, "Atoms": [atom(one[0], 1), atom(one[1], 0)]},
            ],
        },
        "Trailer": [atom(trailer, 1)],
        "TotalLength": total,
    }
    return {
        "__type": "IrProtocol",
        "Name": name,
        "Id-": kw.get("pid", 1),
        "CarrierFrequency": carrier,
        "IRSegments": [seg],
        "CodeSegments": kw.get("code_segments", []),
        "KeyCode": kw["keycode"],
    }


SAMSUNGISH = pcm_protocol(
    "Synth A",
    (4500, 4500),
    (560, 560),
    (560, 1690),
    560,
    108000,
    38000,
    keycode={
        "Start": None,
        "Repeat": [{"SegmentType": 1, "SegmentName": "Synth A"}],
        "Finish": None,
    },
)
REPEATING = pcm_protocol(
    "Synth B",
    (8990, 4490),
    (568, 552),
    (568, 1662),
    568,
    107870,
    38000,
    pid=2,
    code_segments=[
        {
            "Name": "Synth B Rep",
            "Payload": None,
            "Atoms": [atom(8990, 1), atom(2230, 0), atom(568, 1), atom(96077, 0)],
        }
    ],
    keycode={
        "Start": [{"SegmentType": 1, "SegmentName": "Synth B"}],
        "Repeat": [{"SegmentType": 0, "SegmentName": "Synth B Rep"}],
        "Finish": None,
    },
)
HID = {"__type": "HidProtocol", "Name": "HID 16 Bit", "Id-": 9}
PROTOCOLS = {p["Name"]: p for p in (SAMSUNGISH, REPEATING, HID)}


def cmd(name, keycode, ctype=None, learned=False, raw=None):
    return {
        "Name": name,
        "CommandTypeId": ctype or name,
        "KeyCode": keycode,
        "IsLearned": learned,
        "Raw": raw,
        "Id-": hash(name) % 1000,
    }


def write_tree(root, commands_by_dev):
    res = root / "data" / "resources"
    res.mkdir(parents=True)
    (res / "ProtocolList.json").write_text(json.dumps({"Protocols": list(PROTOCOLS.values())}))
    devs = [{"Device": {"Id-": int(d)}, "Commands": c} for d, c in commands_by_dev.items()]
    (res / "DeviceList.json").write_text(json.dumps({"DevicesWithFeatures": devs}))
    return res


def test_decode_matches_reference_encoder():
    got = ex.decode_keycode("G:Synth A:()(0xE0E040BF)():3", PROTOCOLS)
    hz, once, _rep = ir_ref.samsung32(0x07, 0x02)  # 0xE0E040BF MSB first == addr 7 cmd 2
    assert got["carrier_hz"] == hz
    assert got["once"] == []
    assert got["repeat"] == once
    assert got["min_repeats"] == 3


def test_start_and_repeat_sections():
    got = ex.decode_keycode("G:Synth B:(0x00BFC03F)(Repeat)():1", PROTOCOLS)
    assert got["once"][:2] == [8990, 4490]
    assert sum(got["once"]) == 107870  # padded to TotalLength
    assert got["repeat"] == [8990, 2230, 568, 96077]
    assert got["finish"] == []


@pytest.mark.parametrize(
    "keycode",
    [
        "G:Nope 32 Bit:()(0x1)():3",  # protocol not in the list
        "G:Synth A:()(0xE0E040BF):3",  # two groups, not three
        "G:Synth A:()(zzz)():3",  # payload not hex
        "G:Synth A:()(0x1FFFFFFFF)():3",  # wider than 32 bits
        "garbage",
    ],
)
def test_bad_keycodes_raise(keycode):
    with pytest.raises(ex.DecodeError):
        ex.decode_keycode(keycode, PROTOCOLS)


def test_unknown_encoding_kept_raw_with_warning(tmp_path):
    res = write_tree(
        tmp_path,
        {
            "1": [
                cmd("Good", "G:Synth A:()(0xE0E040BF)():3"),
                cmd("Odd", "G:Mystery Proto:()(0x12)():3"),
                cmd("Learned", "", raw="AAAA"),
            ]
        },
    )
    db = ex.extract(res, tmp_path)
    assert set(db["devices"]["1"]) == {"Good", "Odd", "Learned"}  # nothing dropped
    odd = db["devices"]["1"]["Odd"]
    assert odd["transport"] == "unknown"
    assert odd["key_code"] == "G:Mystery Proto:()(0x12)():3"
    assert bytes.fromhex(odd["source_hex"]).decode() == odd["key_code"]
    assert db["devices"]["1"]["Learned"]["raw"] == "AAAA"
    assert {w["command"] for w in db["warnings"]} == {"Odd", "Learned"}
    good = db["devices"]["1"]["Good"]
    assert good["transport"] == "ir" and good["source_file"].endswith("DeviceList.json")


def test_non_ir_transports_recorded_with_key(tmp_path):
    res = write_tree(
        tmp_path,
        {
            "2": [
                cmd("Number1", "G:HID 16 Bit:()(0700001E)():1", learned=True),
                cmd("Home", "G:Roku IP:(Key)(Home):3", learned=True),
            ]
        },
    )
    db = ex.extract(res, tmp_path)
    assert db["warnings"] == []
    assert db["devices"]["2"]["Number1"]["transport"] == "hid"
    assert db["devices"]["2"]["Number1"]["key"] == "0700001E"
    assert db["devices"]["2"]["Home"]["transport"] == "roku_ecp"
    assert db["devices"]["2"]["Home"]["key"] == "Home"


def test_unknown_command_type_falls_back_to_name(tmp_path):
    res = write_tree(
        tmp_path, {"3": [cmd("InputA1", "G:Synth A:()(0xE0E040BF)():3", ctype="Unknown")]}
    )
    assert "InputA1" in ex.extract(res, tmp_path)["devices"]["3"]


def test_coverage_report(tmp_path):
    res = write_tree(
        tmp_path / "t",
        {
            "1": [
                cmd("PowerOn", "G:Synth A:()(0xE0E040BF)():3"),
                cmd("Mute", "G:Mystery:()(0x1)():3"),
            ],
            "2": [cmd("Home", "G:Roku IP:(Key)(Home):3")],
        },
    )
    db = ex.extract(res, tmp_path / "t")
    cfg = tmp_path / "cfg"
    cfg.mkdir()
    names = {"1": ["PowerOn", "Mute", "VolumeUp"], "2": ["Home"], "9": ["Gone"]}
    (cfg / "ir_codes.json").write_text(
        json.dumps({"devices": {d: {n: {} for n in c} for d, c in names.items()}})
    )
    cov = ex.coverage(db, cfg)
    assert cov["total"] == {"commands": 5, "ir": 1, "other_transport": 1, "missing": 3}
    assert cov["devices"]["1"]["missing"] == ["Mute", "VolumeUp"]


def test_main_writes_outputs(tmp_path, capsys):
    write_tree(tmp_path / "t", {"1": [cmd("PowerOn", "G:Synth A:()(0xE0E040BF)():3")]})
    cfg = tmp_path / "cfg"
    cfg.mkdir()
    (cfg / "ir_codes.json").write_text(json.dumps({"devices": {"1": {"PowerOn": {}}}}))
    out = tmp_path / "out"
    assert ex.main([str(tmp_path / "t"), "--out-dir", str(out), "--config-dir", str(cfg)]) == 0
    assert (
        json.loads((out / "ir_codes.json").read_text())["devices"]["1"]["PowerOn"]["carrier_hz"]
        == 38000
    )
    assert "1/1 commands have IR timings" in capsys.readouterr().out


def test_find_resources_missing(tmp_path):
    with pytest.raises(FileNotFoundError):
        ex.find_resources(tmp_path)
