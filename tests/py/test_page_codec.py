import json
from pathlib import Path

import pytest

from custom_components.ha_harmony.page_codec import (
    MAX_LABEL,
    MAX_WIDGETS,
    Widget,
    decode_page,
    encode_page,
)

VECTORS = json.loads((Path(__file__).parent.parent / "vectors" / "pages.json").read_text())


def widgets(spec):
    return [Widget(**w) for w in spec]


@pytest.mark.parametrize("v", VECTORS["valid"], ids=lambda v: v["name"])
def test_encode_matches_vector_and_roundtrips(v):
    ws = widgets(v["widgets"])
    data = encode_page(v["page_id"], v["cols"], v["rows"], ws)
    assert data.hex() == v["hex"]
    page = decode_page(data)
    assert (page.page_id, page.cols, page.rows) == (v["page_id"], v["cols"], v["rows"])
    assert list(page.widgets) == ws


@pytest.mark.parametrize("v", VECTORS["invalid"], ids=lambda v: v["name"])
def test_decode_rejects_invalid_vectors(v):
    with pytest.raises(ValueError):
        decode_page(bytes.fromhex(v["hex"]))


def enc(*ws, cols=4, rows=6):
    return encode_page(1, cols, rows, list(ws))


def b(i, x=0, y=0, w=1, h=1, label="", kind="button"):
    return Widget(kind, i, x, y, w, h, label)


@pytest.mark.parametrize(
    ("ws", "kw", "match"),
    [
        ([b(1, x=4)], {}, "outside"),
        ([b(1, y=5, h=2)], {}, "outside"),
        ([b(1, w=0)], {}, "size"),
        ([b(1, h=0)], {}, "size"),
        ([b(1, w=16)], {"cols": 16, "rows": 16}, "size"),
        ([b(1, h=16)], {"cols": 16, "rows": 16}, "size"),
        ([b(1, w=2, h=2), b(2, x=1, y=1, w=2, h=2)], {}, "overlap"),
        ([b(1), b(1, x=1)], {}, "duplicate"),
        ([b(128)], {}, "id"),
        ([b(1, label="a" * (MAX_LABEL + 1))], {}, "label"),
        ([b(1, label="é" * 13)], {}, "label"),  # 26 bytes, 13 chars
        ([b(1, label="a\x00")], {}, "NUL"),
        ([Widget("slider", 1, 0, 0, 1, 1)], {}, "kind"),
        ([b(i, x=i % 4, y=i // 4) for i in range(MAX_WIDGETS + 1)], {"cols": 16, "rows": 16}, "too many"),
        ([b(1)], {"cols": 0}, "grid"),
        ([b(1)], {"rows": 17}, "grid"),
    ],
)
def test_encode_limits(ws, kw, match):
    with pytest.raises(ValueError, match=match):
        enc(*ws, **kw)


def test_encode_page_id_range():
    with pytest.raises(ValueError, match="page id"):
        encode_page(256, 4, 6, [])


def test_total_size_limit():
    ws = [Widget("label", i, i % 16, i // 16, 1, 1, "x" * MAX_LABEL) for i in range(9)]
    with pytest.raises(ValueError, match="255"):  # 5 + 9 * 29 = 266
        encode_page(1, 16, 2, ws)
    assert len(encode_page(1, 16, 2, ws[:8])) == 5 + 8 * 29
    with pytest.raises(ValueError, match="255"):
        decode_page(bytes([1, 0, 16, 16, 0]) + bytes(251))


def test_decode_utf8_label_roundtrip():
    page = decode_page(encode_page(1, 4, 6, [b(1, label="日本語")]))
    assert page.widgets[0].label == "日本語"
