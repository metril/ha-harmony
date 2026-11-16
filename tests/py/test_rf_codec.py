import json
from pathlib import Path

import pytest

from proto.rf_codec import Reassembler, split

VECTORS = Path(__file__).parent.parent / "vectors" / "rf_frames.json"


def pat(n: int) -> bytes:
    return bytes((i * 7 + 3) & 0xFF for i in range(n))


def roundtrip(payload: bytes, cap: int) -> list[bytes]:
    frames = split(9, 5, payload, cap)
    r = Reassembler()
    out = None
    for i, f in enumerate(frames):
        assert len(f) <= cap
        out = r.push(f)
        assert (out is None) == (i + 1 < len(frames))
    assert out == (9, payload)
    return frames


@pytest.mark.parametrize(
    ("n", "cap", "frags"),
    [(0, 30, 1), (5, 30, 1), (26, 30, 1), (27, 30, 2), (28, 32, 1), (29, 32, 2),
     (390, 30, 15), (420, 32, 15)],
)
def test_roundtrip(n, cap, frags):
    assert len(roundtrip(pat(n), cap)) == frags


@pytest.mark.parametrize(("n", "cap"), [(391, 30), (421, 32)])
def test_too_big(n, cap):
    with pytest.raises(ValueError, match="fragments"):
        split(9, 1, pat(n), cap)


def test_bad_cap():
    for cap in (4, 33):
        with pytest.raises(ValueError):
            split(9, 1, b"x", cap)


def test_duplicate_and_out_of_order():
    p = pat(80)
    f = split(3, 3, p, 30)
    r = Reassembler()
    for i in (2, 2, 0, 0, 3):
        assert r.push(f[i]) is None
    assert r.push(f[1]) == (3, p)


def test_mismatched_total_keeps_state():
    p = pat(80)
    f = split(3, 3, p, 30)
    r = Reassembler()
    assert r.push(f[0]) is None
    bad = bytes([f[1][0], f[1][1], (1 << 4) | 5]) + f[1][3:]
    with pytest.raises(ValueError):
        r.push(bad)
    assert r.push(f[1]) is None
    assert r.push(f[2]) is None
    assert r.push(f[3]) == (3, p)


def test_new_seq_discards_partial():
    f = split(3, 3, pat(80), 30)
    g = split(3, 4, b"\xab" * 10, 30)
    r = Reassembler()
    r.push(f[0])
    r.push(f[1])
    assert r.push(g[0]) == (3, b"\xab" * 10)
    assert r.push(f[2]) is None
    assert r.push(f[3]) is None


@pytest.mark.parametrize(
    "frame",
    [b"", b"\x01\x01\x01", bytes([1, 1, 0x00, 0]), bytes([1, 1, 0x22, 0]),
     bytes([1, 1, 0xF3, 0]), bytes([1, 1, 0x01, 10, 0, 0, 0, 0]),
     bytes([1, 1, 0x01, 33]) + bytes(28), bytes([1, 1, 0x01, 255]) + bytes(28)],
)
def test_malformed(frame):
    with pytest.raises(ValueError):
        Reassembler().push(frame)


def test_python_matches_c_vectors():
    cases = json.loads(VECTORS.read_text())
    assert cases
    for c in cases:
        payload = bytes.fromhex(c["payload"])
        got = split(c["type"], c["seq"], payload, c["frame_cap"])
        assert [f.hex() for f in got] == c["frames"]
