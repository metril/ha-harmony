import json
from pathlib import Path

import pytest

from proto.rf_codec import Reassembler, split

VECTORS = json.loads((Path(__file__).parent.parent / "vectors" / "rf" / "frames.json").read_text())


@pytest.mark.parametrize("v", VECTORS["vectors"], ids=lambda v: v["name"])
def test_vectors_match_hub_frames(v):
    frames = split(v["type"], v["seq"], bytes.fromhex(v["body"]), v["slot"])
    assert [f.hex() for f in frames] == v["hub"]
    r = Reassembler()
    out = [r.push(f) for f in frames]
    assert out[:-1] == [None] * (len(frames) - 1)
    assert out[-1] == (v["type"], bytes.fromhex(v["body"]))


@pytest.mark.parametrize("n", range(255))
def test_every_length_roundtrips(n):
    body = bytes((i * 7 + 3) & 0xFF for i in range(n))
    r = Reassembler()
    res = [r.push(f) for f in split(5, n, body)]
    assert res[-1] == (5, body) and all(x is None for x in res[:-1])


def test_too_big_and_rejects():
    with pytest.raises(ValueError):
        split(1, 1, bytes(255))
    r = Reassembler()
    for bad in VECTORS["reject"]:
        with pytest.raises(ValueError):
            r.push(bytes.fromhex(bad["frame"]))


def test_dup_continuation_and_retried_object():
    body = bytes(range(60))
    f = split(3, 3, body)
    r = Reassembler()
    r.push(f[0])
    r.push(f[1])
    with pytest.raises(ValueError):
        r.push(f[1])
    assert [r.push(x) for x in f][-1] == (3, body)
    assert [r.push(x) for x in f][-1] is None  # re-acked, not redelivered
