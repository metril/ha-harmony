import json
import random
from dataclasses import replace
from pathlib import Path

import pytest

from harmony_core.page_codec import (
    ROLES,
    SHAPES,
    Page,
    Widget,
    decode_page,
    encode,
    page_to_dict,
)

VECTORS = json.loads((Path(__file__).parent.parent / "vectors" / "pages.json").read_text())


def model_from_dict(d):
    seq = d["seq"] or {"idx": 0, "cnt": 0}
    return Page(
        d["id"],
        d["cols"],
        d["rows"],
        tuple(Widget(**w) for w in d["widgets"]),
        d["version"],
        d["gen"],
        d["total"],
        d["kind"],
        d["back"],
        seq["idx"],
        seq["cnt"],
        d["act"],
        d["title"],
    )


@pytest.mark.parametrize("v", VECTORS["valid"], ids=lambda v: v["name"])
def test_v1_vectors_page_to_dict(v):
    assert page_to_dict(decode_page(bytes.fromhex(v["hex"]))) == v["dict"]
    assert v["dict"]["version"] == 1 and v["dict"]["total"] == 0


@pytest.mark.parametrize("v", VECTORS["valid_v2"], ids=lambda v: v["name"])
def test_v2_vectors(v):
    data = bytes.fromhex(v["hex"])
    page = decode_page(data)
    assert page_to_dict(page) == v["dict"]
    assert encode(model_from_dict(v["dict"]), 2) == data
    assert encode(page, 2) == data


@pytest.mark.parametrize("v", VECTORS["invalid_v2"], ids=lambda v: v["name"])
def test_v2_invalid_vectors(v):
    with pytest.raises(ValueError):
        decode_page(bytes.fromhex(v["hex"]))


def test_v2_minimal_hand_assembled():
    assert encode(Page(0, 4, 6, (), total=1), 2).hex() == "0200040600000100000000"


def test_v2_251_boundary_and_overage():
    v = next(v for v in VECTORS["valid_v2"] if v["name"].endswith("251_bytes"))
    assert len(bytes.fromhex(v["hex"])) == 251
    page = model_from_dict(v["dict"])
    ws = list(page.widgets)
    ws[31] = replace(ws[31], label="abc")
    with pytest.raises(ValueError, match="252 bytes"):
        encode(replace(page, widgets=tuple(ws)), 2)


def test_v1_view_of_v2_page():
    page = Page(
        1,
        4,
        6,
        (
            Widget("button", 1, 0, 0, 1, 1, "A", role="primary", icon=3, bind=0),
            Widget("button", 2, 1, 0, 1, 1, "N", role="nav", bind=0),
        ),
        2,
        total=2,
        title="Hi",
    )
    p1 = decode_page(encode(page, 1))
    assert p1.version == 1 and p1.rows == 7
    assert [(w.kind, w.y, w.label) for w in p1.widgets] == [("label", 0, "Hi"), ("button", 1, "A")]
    assert p1.widgets[0].w == 4 and p1.widgets[0].h == 1


def _rand_page(rng: random.Random) -> Page:
    cols, rows = rng.randint(1, 16), rng.randint(1, 16)
    total = rng.randint(1, 255)
    pid = rng.randrange(total)
    ws, occ = [], set()
    ids = rng.sample(range(128), 32)
    for i in range(rng.randint(0, 32)):
        w, h = rng.randint(1, min(3, cols)), rng.randint(1, min(3, rows))
        x, y = rng.randint(0, cols - w), rng.randint(0, rows - h)
        cells = {(x + a, y + b) for a in range(w) for b in range(h)}
        if cells & occ:
            continue
        occ |= cells
        role = rng.choice(ROLES)
        ws.append(
            Widget(
                rng.choice(["button", "tile", "label"]),
                ids[i],
                x,
                y,
                w,
                h,
                "".join(rng.choice("abé日 ") for _ in range(rng.randint(0, 3))),
                role,
                rng.choice(SHAPES),
                rng.random() < 0.3,
                rng.choice([0, 0, rng.randint(1, 254)]),
                rng.randrange(total) if role == "nav" else rng.choice([None, rng.randint(0, 255)]),
            )
        )
    seq = (0, 0)
    cnt = rng.randint(2, 15)
    idx = rng.randint(1, cnt)
    if pid - idx + 1 >= 0 and pid + cnt - idx < total and rng.random() < 0.5:
        seq = (idx, cnt)
    return Page(
        pid,
        cols,
        rows,
        tuple(ws),
        2,
        rng.randint(0, 255),
        total,
        rng.choice(["normal", "home", "favorites"]),
        rng.random() < 0.5,
        seq[0],
        seq[1],
        rng.randint(0, 255),
        "".join(rng.choice("abé日") for _ in range(rng.randint(0, 6))),
    )


def test_property_roundtrip_identity():
    rng = random.Random(1234)
    n = 0
    for _ in range(1500):
        page = _rand_page(rng)
        try:
            data = encode(page, 2)
        except ValueError:
            continue  # over the byte budget
        n += 1
        assert decode_page(data) == page
        assert encode(decode_page(data), 2) == data
    assert n > 1000


def test_property_single_byte_corruption_never_crashes():
    rng = random.Random(99)
    corpus = [bytes.fromhex(v["hex"]) for v in VECTORS["valid_v2"]]
    while len(corpus) < len(VECTORS["valid_v2"]) + 20:
        try:
            corpus.append(encode(_rand_page(rng), 2))
        except ValueError:
            pass
    for data in corpus:
        for i in range(len(data)):
            for val in {*rng.sample(range(256), 12), 0, 1, 2, 3, 0x7F, 0x80, 0xFF, data[i] ^ 1}:
                bad = data[:i] + bytes([val]) + data[i + 1 :]
                try:
                    page = decode_page(bad)
                except ValueError:
                    continue
                assert encode(page, 2)  # a decoded page is always a valid, re-encodable model
        for cut in range(len(data)):  # truncation is never accepted
            with pytest.raises(ValueError):
                decode_page(data[:cut])
