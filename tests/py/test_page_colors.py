"""Page format v2 colours and theme id: costs, canonical encoding, seeded round-trip property."""

import random
from dataclasses import replace

import pytest

from harmony_core.page_codec import (
    Page,
    Widget,
    decode_page,
    encode,
    widget_cost,
    widget_size,
)
from harmony_core.theme_math import PALETTE_NAMES, snap565


def pg(*ws, theme=0, total=1):
    return Page(0, 4, 6, tuple(ws), version=2, total=total, theme=theme)


def w(kind="button", **kw):
    return Widget(kind, 1, 0, 0, 1, 1, **kw)


@pytest.mark.parametrize(
    ("widget", "color_cost"),
    [
        (w(), 0),
        (w(color="red"), 1),
        (w(color="#F85848"), 3),
        (w("tile", on_color="pink"), 1),
        (w("tile", color="red", on_color="#102030"), 3),
        (w("tile", color="#102030", on_color="#304050"), 5),
    ],
)
def test_widget_cost(widget, color_cost):
    c = widget_cost(widget)
    assert c["color"] == color_cost
    assert sum(c.values()) == widget_size(widget) == len(encode(pg(widget))) - 11


def test_widget_cost_parts():
    c = widget_cost(w("tile", label="héé", role="primary", icon=3, bind=1, color="teal"))
    assert c == {"base": 5, "label": 5, "style": 1, "icon": 1, "bind": 1, "color": 1}


def test_rgb_is_snapped_and_canonical():
    b = encode(pg(w(color="#123456")))
    p = decode_page(b)
    assert p.widgets[0].color == f"#{snap565(0x123456):06X}"
    assert encode(p) == b
    assert encode(pg(w(color="#f85848"))) == encode(pg(w(color="#F85848")))


def test_no_colour_means_no_bytes():
    plain = encode(pg(w(label="a")))
    assert plain[11] & 0x80 == 0 and plain[7] == 0


def test_theme_bits():
    for t in range(8):
        assert encode(pg(theme=t))[7] == t << 3
        assert decode_page(encode(pg(theme=t))).theme == t
    with pytest.raises(ValueError, match="theme"):
        encode(pg(theme=8))


@pytest.mark.parametrize(
    "widget",
    [
        w(on_color="red"),
        w("label", on_color="#000000"),
        w(color="mauve"),
        w(color="#12345"),
        w(color="F85848"),
    ],
)
def test_encode_rejects(widget):
    with pytest.raises(ValueError):
        encode(pg(widget))


def test_v1_drops_colours():
    assert encode(pg(w(color="red"), theme=3), 1) == encode(pg(w()), 1)


def test_old_decoder_rejects_new_pages():
    """Pre-colour decoders reject kind bit 7 and pflags bits 3-5 (reserved), so old remotes drop the page."""
    coloured = encode(pg(w(color="red")))
    assert coloured[11] & 0x8C  # old KIND_RESERVED includes bit 7
    themed = encode(pg(theme=1))
    assert themed[7] & ~0x07  # old pflags mask allowed only bits 0-2


def _rand_color(rng):
    r = rng.random()
    if r < 0.4:
        return None
    if r < 0.7:
        return rng.choice(PALETTE_NAMES)
    return f"#{rng.randrange(1 << 24):06X}"


def _rand_widget(rng, i, x, y):
    kind = rng.choice(("button", "tile", "label"))
    return Widget(
        kind,
        i,
        x,
        y,
        1,
        1,
        rng.choice(("", "a", "Lamp", "héllo")),
        rng.choice(("default", "primary", "secondary", "danger")),
        rng.choice(("card", "row", "pill", "monogram")),
        rng.random() < 0.3,
        rng.choice((0, 0, 5, 159, 160)),
        rng.choice((None, None, 0)),
        _rand_color(rng),
        _rand_color(rng) if kind == "tile" else None,
    )


@pytest.mark.parametrize("seed", range(8))
def test_roundtrip_property(seed):
    rng = random.Random(seed)
    for _ in range(300):
        n = rng.randint(0, 16)
        ws = tuple(_rand_widget(rng, i, i % 4, i // 4) for i in range(n))
        page = Page(0, 4, 6, ws, version=2, total=1, theme=rng.randrange(8))
        try:
            b = encode(page)
        except ValueError as e:
            assert "bytes" in str(e)  # only the size budget may refuse a generated page
            continue
        got = decode_page(b)
        assert encode(got) == b  # canonical: decode then encode is the identity
        assert [(x.color is None, x.on_color is None) for x in got.widgets] == [
            (x.color is None, x.on_color is None) for x in ws
        ]
        assert got.theme == page.theme
        for a, c in zip(ws, got.widgets, strict=True):
            for f in ("color", "on_color"):
                want, have = getattr(a, f), getattr(c, f)
                assert have == (
                    f"#{snap565(int(want[1:], 16)):06X}" if want and want[0] == "#" else want
                )
            assert replace(c, color=a.color, on_color=a.on_color) == a
