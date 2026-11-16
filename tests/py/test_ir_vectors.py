"""The independent reference encoder must reproduce the shared IR vectors; the
C encoder is checked against the same files by tests/c/test_ir_encode.c."""

import json
from pathlib import Path

import ir_ref
import pytest

DIR = Path(__file__).parent.parent / "vectors" / "ir"
CASES = [(f.stem, i, v) for f in sorted(DIR.glob("*.json")) for i, v in enumerate(json.loads(f.read_text()))]


def test_vectors_present():
    assert {n for n, _, _ in CASES} >= {
        "nec", "necext", "samsung32", "rc5", "rc6", "sirc12", "sirc15", "sirc20", "pronto_raw",
    }


@pytest.mark.parametrize(("name", "idx", "v"), CASES, ids=[f"{n}-{i}" for n, i, _ in CASES])
def test_reference_matches_vector(name, idx, v):
    hz, once, rep = ir_ref.encode(v["code"], v["toggle"])
    assert (hz, once, rep) == (v["carrier_hz"], v["once"], v["rep"])
    assert ir_ref.period_us(v["code"]) == v["period_us"]
    assert len(once) % 2 == 0 and len(rep) % 2 == 0
    assert all(0 < d <= 65535 for d in once + rep)


def test_hand_checked_nec():
    # NEC addr 0x04 cmd 0x08: bytes 04 FB 08 F7 sent LSB first; 108 ms period.
    _, once, rep = ir_ref.encode({"proto": "nec", "addr": 4, "cmd": 8})
    assert once[:2] == [9000, 4500]
    bits = [1 if once[3 + 2 * i] > 1000 else 0 for i in range(32)]
    val = sum(b << i for i, b in enumerate(bits))
    assert val == 0xF7_08_FB_04
    assert sum(once) == 108000 - 39970 + 39970 and once[-1] == 39970 and rep == [9000, 2250, 562, 65535]


def test_hand_checked_necext():
    # Flipper NECext "addr 87 7C 00 00, cmd 15 EA 00 00" -> addr 0x7C87, cmd 0xEA15.
    # Wire bytes (each LSB first): 87 7C 15 EA, no computed inverse.
    _, once, _ = ir_ref.encode({"proto": "necext", "addr": 0x7C87, "cmd": 0xEA15})
    bits = [1 if once[3 + 2 * i] > 1000 else 0 for i in range(32)]
    assert sum(b << i for i, b in enumerate(bits)) == 0xEA15_7C87
    with pytest.raises(ValueError):
        ir_ref.encode({"proto": "necext", "addr": 0, "cmd": 0x10000})


def test_hand_checked_rc5_toggle_differs():
    a = ir_ref.encode({"proto": "rc5", "addr": 5, "cmd": 35}, 0)[1]
    b = ir_ref.encode({"proto": "rc5", "addr": 5, "cmd": 35}, 1)[1]
    assert a != b


@pytest.mark.parametrize(
    "code",
    [
        {"proto": "nec", "addr": 256, "cmd": 0},
        {"proto": "rc5", "addr": 0, "cmd": 128},
        {"proto": "sirc12", "addr": 32, "cmd": 0},
        {"pronto": "0100 006D 0001 0000 0010 0010"},
        {"pronto": "0000 006D 0002 0000 0010 0010"},
        {"raw": {"hz": 38000, "us": [1, 2, 3]}},
    ],
)
def test_reference_rejects(code):
    with pytest.raises(ValueError):
        ir_ref.encode(code)
