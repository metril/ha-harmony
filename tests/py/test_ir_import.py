"""IR database import: Flipper / SmartIR / Broadlink / Pronto -> config-format-2 IR actions."""

import base64
from pathlib import Path

import pytest

from custom_components.ha_harmony.ir.flipper import parse_flipper, parse_flipper_file
from custom_components.ha_harmony.ir.model import (
    PROTOCOLS,
    IrCode,
    IrImportError,
    UnsupportedProtocolError,
)
from custom_components.ha_harmony.ir.pronto import normalise_pronto, pronto_to_raw
from custom_components.ha_harmony.ir.smartir import decode_broadlink, parse_smartir_file

FIX = Path(__file__).parent / "fixtures" / "ir"


# ---- model ----
def test_protocol_list():
    assert PROTOCOLS == (
        "nec", "necext", "samsung32", "rc5", "rc6", "sirc12", "sirc15", "sirc20"
    )


def test_hub_json_shapes():
    assert IrCode(proto="nec", addr=4, cmd=8).to_hub_json() == {
        "proto": "nec", "addr": 4, "cmd": 8}
    assert IrCode(hz=38000, us=(100, 200)).to_hub_json() == {
        "raw": {"hz": 38000, "us": [100, 200]}}
    assert IrCode(pronto="0000 006D 0000 0001 0010 0020").to_hub_json() == {
        "pronto": "0000 006D 0000 0001 0010 0020"}


@pytest.mark.parametrize("kw", [
    {},
    {"proto": "nec", "addr": 1},
    {"proto": "nec", "addr": 1, "cmd": 1, "pronto": "0000 006D 0000 0001 0010 0020"},
    {"proto": "bogus", "addr": 1, "cmd": 1},
    {"proto": "nec", "addr": 256, "cmd": 1},
    {"proto": "nec", "addr": 1, "cmd": -1},
    {"proto": "rc5", "addr": 32, "cmd": 0},
    {"proto": "sirc12", "addr": 0, "cmd": 128},
    {"hz": 38000, "us": ()},
    {"hz": 0, "us": (1, 2)},
    {"hz": 38000, "us": (1, 0)},
    {"hz": 38000},
    {"pronto": "zzzz"},
])
def test_invalid_codes(kw):
    with pytest.raises(IrImportError):
        IrCode(**kw)


def test_raw_size_limit():
    code = IrCode(hz=38000, us=(100,) * 10)
    assert code.to_hub_json(max_raw_us=10)["raw"]["us"] == [100] * 10
    with pytest.raises(IrImportError, match="too long"):
        code.to_hub_json(max_raw_us=9)
    with pytest.raises(IrImportError, match="too long"):
        IrCode(pronto="0000 006D 0000 0005" + " 0010 0020" * 5).to_hub_json(max_raw_us=9)
    # protocol codes are never size-limited
    assert IrCode(proto="nec", addr=1, cmd=1).to_hub_json(max_raw_us=1)


# ---- pronto ----
def test_normalise_pronto():
    assert normalise_pronto("0000 006d 0000 0001\n0010  0020") == "0000 006D 0000 0001 0010 0020"


@pytest.mark.parametrize("s", [
    "", "0000 006D 0000", "0000 006D 0001 0001 0010 0020",  # count mismatch (len 6 vs needs 8)
    "0000 006D 0000 0000", "0100 006D 0000 0001 0010 0020", "0000 0000 0000 0001 0010 0020",
    "0000 006D 0000 0001 0010 00G0", "0000 006D 0000 0001 0010 12345",
])
def test_bad_pronto(s):
    with pytest.raises(IrImportError):
        normalise_pronto(s)


def test_pronto_to_raw_hand_computed():
    # code 0x6D=109 -> period 109*0.241246 = 26.2958us ; 0x10 -> 420.73 ; 0x20 -> 841.47
    hz, us = pronto_to_raw("0000 006D 0001 0001 0010 0020 0010 0020")
    assert hz == 38029
    assert us == [421, 841, 421, 841]


def test_pronto_to_raw_once_only():
    hz, us = pronto_to_raw("0000 006D 0001 0000 0010 0020")
    assert (hz, us) == (38029, [421, 841])


# ---- flipper ----
def test_flipper_sample():
    sigs = parse_flipper_file(FIX / "sample.ir")
    assert list(sigs) == ["Power", "Vol_up", "Raw_key", "Mute"]
    assert sigs["Power"] == IrCode(proto="nec", addr=4, cmd=8)
    assert sigs["Vol_up"] == IrCode(proto="necext", addr=0x7C87, cmd=0xCA)
    assert sigs["Raw_key"] == IrCode(hz=38000, us=(9000, 4500, 560, 560, 560, 1690, 560))
    assert sigs["Mute"] == IrCode(proto="sirc15", addr=1, cmd=0x14)


def test_flipper_library_duplicates_and_protocols():
    sigs = parse_flipper_file(FIX / "library.ir")
    assert list(sigs) == ["Power", "Power_2", "Power_3"]
    assert sigs["Power"] == IrCode(proto="samsung32", addr=7, cmd=2)
    assert sigs["Power_2"] == IrCode(proto="rc5", addr=5, cmd=0x41)
    assert sigs["Power_3"] == IrCode(proto="rc6", addr=10, cmd=12)


@pytest.mark.parametrize("name,proto,addr,cmd", [
    ("NEC", "nec", 1, 2), ("NECext", "necext", 0x1234, 2), ("Samsung32", "samsung32", 1, 2),
    ("RC5", "rc5", 1, 2), ("RC5X", "rc5", 1, 0x42), ("RC6", "rc6", 1, 2),
    ("SIRC", "sirc12", 1, 2), ("SIRC15", "sirc15", 1, 2), ("SIRC20", "sirc20", 0x123, 2),
])
def test_flipper_protocol_mapping(name, proto, addr, cmd):
    a = addr.to_bytes(4, "little").hex(" ")
    c = cmd.to_bytes(4, "little").hex(" ")
    text = (f"Filetype: IR signals file\nVersion: 1\n#\nname: K\ntype: parsed\n"
            f"protocol: {name}\naddress: {a}\ncommand: {c}\n")
    assert parse_flipper(text)["K"] == IrCode(proto=proto, addr=addr, cmd=cmd)


def test_flipper_unsupported_protocol_raises():
    with pytest.raises(UnsupportedProtocolError, match="Kaseikyo"):
        parse_flipper_file(FIX / "unsupported.ir")


def test_flipper_malformed():
    with pytest.raises(IrImportError, match="Filetype"):
        parse_flipper_file(FIX / "bad_header.ir")
    with pytest.raises(IrImportError, match="command"):
        parse_flipper_file(FIX / "bad_signal.ir")
    with pytest.raises(IrImportError, match="Version"):
        parse_flipper("Filetype: IR signals file\nVersion: 2\n")
    with pytest.raises(IrImportError):
        parse_flipper("")
    base = "Filetype: IR signals file\nVersion: 1\n#\nname: K\n"
    with pytest.raises(IrImportError, match="hex"):
        parse_flipper(base + "type: parsed\nprotocol: NEC\naddress: 0G 00 00 00\n"
                      "command: 01 00 00 00\n")
    with pytest.raises(IrImportError, match="type"):
        parse_flipper(base + "type: weird\n")
    with pytest.raises(IrImportError):
        parse_flipper(base + "type: raw\nfrequency: 38000\nduty_cycle: 0.3\ndata: 1 x\n")


# ---- broadlink / smartir ----
def test_broadlink_decode_hand_computed():
    # data 10 20 00 01 00: 16 ticks, 32 ticks, 0x0100=256 ticks; tick = 8192/269 us
    pkt = bytes.fromhex("2600 0500 10 20 00 0100 0d05".replace(" ", ""))
    assert decode_broadlink(pkt) == [487, 975, 7796]


def test_broadlink_errors():
    with pytest.raises(IrImportError, match="0x26"):
        decode_broadlink(bytes.fromhex("b2000100100d05"))
    with pytest.raises(IrImportError, match="length"):
        decode_broadlink(bytes.fromhex("2600 0900 10 20".replace(" ", "")))
    with pytest.raises(IrImportError, match="truncated"):
        decode_broadlink(bytes.fromhex("2600 0200 10 00".replace(" ", "")))
    with pytest.raises(IrImportError):
        decode_broadlink(b"\x26")


def test_smartir_base64():
    codes = parse_smartir_file(FIX / "smartir_base64.json")
    assert list(codes) == ["off", "cool.low.22"]
    assert codes["off"] == IrCode(hz=38000, us=(487, 975, 7796))
    assert codes["cool.low.22"] == codes["off"]


def test_smartir_pronto_and_raw():
    codes = parse_smartir_file(FIX / "smartir_pronto.json")
    assert codes["power"] == IrCode(pronto="0000 006D 0000 0002 0010 0020 0010 0020")
    codes = parse_smartir_file(FIX / "smartir_raw.json")
    assert codes["a"] == IrCode(hz=38000, us=(500, 600, 500, 700))
    assert codes["b"] == IrCode(hz=38000, us=(400, 300, 400))


def test_smartir_errors(tmp_path):
    with pytest.raises(IrImportError, match="commandsEncoding"):
        parse_smartir_file(FIX / "smartir_badenc.json")
    with pytest.raises(IrImportError, match="JSON"):
        parse_smartir_file(FIX / "smartir_malformed.json")
    bad = tmp_path / "x.json"
    bad.write_text('{"commandsEncoding":"Base64","commands":{"a":{"b":"!!!"}}}')
    with pytest.raises(IrImportError, match="a.b"):
        parse_smartir_file(bad)
    bad.write_text('{"commandsEncoding":"Base64"}')
    with pytest.raises(IrImportError, match="commands"):
        parse_smartir_file(bad)
    bad.write_text('{"commandsEncoding":"Base64","commands":{"a":5}}')
    with pytest.raises(IrImportError, match="a"):
        parse_smartir_file(bad)


def test_broadlink_roundtrip_b64():
    pkt = base64.b64decode("JgAFABAgAAEADQU=")
    assert decode_broadlink(pkt) == [487, 975, 7796]
