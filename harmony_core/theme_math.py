"""Theme maths (hand-written reference; the TS and C twins are pinned to it by tests/vectors/themes.json).

Colours are 0xRRGGBB ints. Everything that decides a colour on the remote is integer maths:
  luminance  Y = 2126*L[r] + 7152*L[g] + 722*L[b]   (L = 12-bit sRGB->linear table, full scale 40950000)
  contrast   (Yhi + K) / (Ylo + K), K = 2047500 (0.05); compared by cross-multiplication, never divided
  snap565    R,B: min(31,(v+4)>>3)<<3 ; G: min(63,(v+2)>>2)<<2  (round to nearest step, ties up)
  ink        TX or BG, whichever contrasts more with the fill (a tie gives TX)
  pressed    per channel (3*fill + ink + 2)>>2, then snap565
The float parts (OKLab nearest palette, "nearest passing" fixes) are sender-side/editor only.
"""

from __future__ import annotations

import math
import re
from collections.abc import Callable, Mapping
from typing import Any

TOKEN_NAMES = (
    "bg",
    "s1",
    "s2",
    "s3",
    "tx",
    "tx2",
    "tx3",
    "acc",
    "onacc",
    "warm",
    "onwarm",
    "bad",
    "onbad",
)
PALETTE_NAMES = (
    "red",
    "orange",
    "amber",
    "yellow",
    "lime",
    "green",
    "teal",
    "cyan",
    "blue",
    "indigo",
    "purple",
    "pink",
)
# (text token, surface token, minimum contrast as num/den) every theme must satisfy
PAIRS = (
    ("tx", "bg", 9, 2),
    ("tx", "s1", 9, 2),
    ("tx", "s2", 9, 2),
    ("tx", "s3", 9, 2),
    ("onacc", "acc", 9, 2),
    ("onwarm", "warm", 9, 2),
    ("onbad", "bad", 9, 2),
    ("tx2", "bg", 9, 2),
    ("tx2", "s1", 9, 2),
    ("tx3", "bg", 3, 1),
    ("tx3", "s1", 3, 1),
)
INK_MIN = (9, 2)  # palette ink on fill
OUTLINE_BELOW = (3, 2)  # a fill under 1.5:1 against BG gets a 1 px TX3 outline
WIRE_VERSION = 1
WIRE_LEN = 1 + 2 * len(TOKEN_NAMES) + 6 * len(PALETTE_NAMES)  # 99
MAX_DEPTH = 4
_K = 2047500


def _lin(v: int) -> int:
    x = v / 255
    x = x / 12.92 if x <= 0.03928 else ((x + 0.055) / 1.055) ** 2.4
    return int(x * 4095 + 0.5)


LIN = tuple(_lin(v) for v in range(256))


def _y(c: int) -> int:
    return 2126 * LIN[(c >> 16) & 255] + 7152 * LIN[(c >> 8) & 255] + 722 * LIN[c & 255]


def _hilo(a: int, b: int) -> tuple[int, int]:
    ya, yb = _y(a) + _K, _y(b) + _K
    return (ya, yb) if ya >= yb else (yb, ya)


def contrast_x100(a: int, b: int) -> int:
    hi, lo = _hilo(a, b)
    return 100 * hi // lo


def meets(a: int, b: int, num: int, den: int) -> bool:
    """contrast(a, b) >= num/den, exactly."""
    hi, lo = _hilo(a, b)
    return den * hi >= num * lo


def snap565(c: int) -> int:
    r = min(31, (((c >> 16) & 255) + 4) >> 3)
    g = min(63, (((c >> 8) & 255) + 2) >> 2)
    b = min(31, ((c & 255) + 4) >> 3)
    return (r << 19) | (g << 10) | (b << 3)


def is565(c: int) -> bool:
    return snap565(c) == c


def ink(fill: int, tx: int, bg: int) -> int:
    nt, dt = _hilo(fill, tx)
    nb, db = _hilo(fill, bg)
    return tx if nt * db >= nb * dt else bg


def pressed(fill: int, ink_: int) -> int:
    c = 0
    for s in (16, 8, 0):
        c |= ((3 * ((fill >> s) & 255) + ((ink_ >> s) & 255) + 2) >> 2) << s
    return snap565(c)


def needs_outline(fill: int, bg: int) -> bool:
    return not meets(fill, bg, *OUTLINE_BELOW)


def to565(c: int) -> int:
    s = snap565(c)
    return ((s >> 19) << 11) | (((s >> 10) & 63) << 5) | ((s >> 3) & 31)


def from565(v: int) -> int:
    return (((v >> 11) & 31) << 19) | (((v >> 5) & 63) << 10) | ((v & 31) << 3)


# ---- wire theme: version, 13 tokens, 12 x (fill, ink, pressed); RGB565 big-endian; fully derived ----


def serialise_theme(tokens: Mapping[str, int], fills: list[int]) -> bytes:
    if len(fills) != len(PALETTE_NAMES):
        raise ValueError("need 12 palette fills")
    t = {k: snap565(tokens[k]) for k in TOKEN_NAMES}
    out = [WIRE_VERSION.to_bytes(1, "big")]
    out += [to565(t[k]).to_bytes(2, "big") for k in TOKEN_NAMES]
    for f in fills:
        f = snap565(f)
        i = ink(f, t["tx"], t["bg"])
        out += [to565(c).to_bytes(2, "big") for c in (f, i, pressed(f, i))]
    return b"".join(out)


def parse_theme(data: bytes) -> tuple[dict[str, int], list[tuple[int, int, int]]]:
    """Decode a wire theme: (tokens, [(fill, ink, pressed)] x 12). Raises ValueError on length/version."""
    if len(data) != WIRE_LEN:
        raise ValueError(f"theme payload must be {WIRE_LEN} bytes, got {len(data)}")
    if data[0] != WIRE_VERSION:
        raise ValueError(f"theme payload version {data[0]} unsupported")
    v = [from565(int.from_bytes(data[i : i + 2], "big")) for i in range(1, WIRE_LEN, 2)]
    n = len(TOKEN_NAMES)
    pal = v[n:]
    return dict(zip(TOKEN_NAMES, v[:n], strict=True)), [
        tuple(pal[i : i + 3]) for i in range(0, len(pal), 3)
    ]  # type: ignore[misc]


# ---- validation and "fix for me" ----


def _srgb_to_oklab(c: int) -> tuple[float, float, float]:
    def f(v: int) -> float:
        x = v / 255
        return x / 12.92 if x <= 0.04045 else ((x + 0.055) / 1.055) ** 2.4

    r, g, b = f((c >> 16) & 255), f((c >> 8) & 255), f(c & 255)
    l_ = math.cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b)
    m_ = math.cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b)
    s_ = math.cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b)
    return (
        0.2104542553 * l_ + 0.7936177850 * m_ - 0.0040720468 * s_,
        1.9779984951 * l_ - 2.4285922050 * m_ + 0.4505937099 * s_,
        0.0259040371 * l_ + 0.7827717662 * m_ - 0.8086757660 * s_,
    )


def _oklab_to_srgb(L: float, a: float, b: float) -> tuple[int, bool]:
    """(0xRRGGBB, in gamut): channels clipped to 0..1; in gamut means none needed more than 0.0005 of clipping."""
    l_ = (L + 0.3963377774 * a + 0.2158037573 * b) ** 3
    m_ = (L - 0.1055613458 * a - 0.0638541728 * b) ** 3
    s_ = (L - 0.0894841775 * a - 1.2914855480 * b) ** 3
    lin = (
        4.0767416621 * l_ - 3.3077115913 * m_ + 0.2309699292 * s_,
        -1.2684380046 * l_ + 2.6097574011 * m_ - 0.3413193965 * s_,
        -0.0041960863 * l_ - 0.7034186147 * m_ + 1.7076147010 * s_,
    )
    out = 0
    ok = all(-0.0005 <= x <= 1.0005 for x in lin)
    for x in lin:
        x = min(1.0, max(0.0, x))
        v = 12.92 * x if x <= 0.0031308 else 1.055 * x ** (1 / 2.4) - 0.055
        out = (out << 8) | int(v * 255 + 0.5)
    return out, ok


def nearest_passing(color: int, ok: Callable[[int], bool]) -> int | None:
    """Closest RGB565 colour to `color` (OKLab lightness moved in 1/200 steps, hue kept (chroma shrunk to fit the gamut), going darker first on a tie) for which ok(c) holds; None when none does."""
    L, a, b = _srgb_to_oklab(color)
    for k in range(1, 201):
        for sign in (-1, 1):
            nl = L + sign * k / 200
            if 0 <= nl <= 1:
                for k_ in (
                    1.0,
                    0.8,
                    0.6,
                    0.4,
                    0.2,
                    0.0,
                ):  # keep the hue: shrink chroma until in gamut
                    rgb, inside = _oklab_to_srgb(nl, a * k_, b * k_)
                    if inside:
                        break
                c = snap565(rgb)
                if ok(c):
                    return c
    return None


def suggest_text(text: int, surface: int, num: int, den: int) -> int:
    c = nearest_passing(text, lambda c: meets(c, surface, num, den))
    if c is not None:
        return c
    return 0xF8FCF8 if meets(0xF8FCF8, surface, num, den) else 0


def suggest_fill(fill: int, tx: int, bg: int) -> int:
    c = nearest_passing(fill, lambda c: meets(ink(c, tx, bg), c, *INK_MIN))
    return fill if c is None else c


def validate_theme(
    tokens: Mapping[str, int], fills: list[int] | None = None
) -> list[dict[str, Any]]:
    """Contrast problems of a token set (+ optional palette fills, checked against their auto ink).

    [{text, surface, ratio_x100, min_x100, fix}] - warnings for a custom theme; fix = nearest RGB565 colour passing."""
    out: list[dict[str, Any]] = []
    for a, s, num, den in PAIRS:
        if not meets(tokens[a], tokens[s], num, den):
            out.append(
                {
                    "text": a,
                    "surface": s,
                    "ratio_x100": contrast_x100(tokens[a], tokens[s]),
                    "min_x100": 100 * num // den,
                    "fix": suggest_text(tokens[a], tokens[s], num, den),
                }
            )
    for name, f in zip(PALETTE_NAMES, fills or [], strict=False):
        i = ink(f, tokens["tx"], tokens["bg"])
        if not meets(i, f, *INK_MIN):
            out.append(
                {
                    "text": "ink",
                    "surface": name,
                    "ratio_x100": contrast_x100(i, f),
                    "min_x100": 100 * INK_MIN[0] // INK_MIN[1],
                    "fix": suggest_fill(f, tokens["tx"], tokens["bg"]),
                }
            )
    return out


def nearest_palette(rgb: int, fills: list[int]) -> int:
    """Wire palette index 1..12 of the fill closest to rgb in OKLab (squared distance; ties to the lower index)."""
    p = _srgb_to_oklab(rgb)
    best, best_d = 1, math.inf
    for i, f in enumerate(fills, 1):
        q = _srgb_to_oklab(f)
        d = (p[0] - q[0]) ** 2 + (p[1] - q[1]) ** 2 + (p[2] - q[2]) ** 2
        if d < best_d:
            best, best_d = i, d
    return best


# ---- user themes ----


class ThemeError(ValueError):
    pass


_HEX = re.compile(r"^#[0-9a-fA-F]{6}$")


def _hex(v: Any, where: str) -> int:
    if not isinstance(v, str) or not _HEX.match(v):
        raise ThemeError(f"{where}: bad colour {v!r} (want #RRGGBB)")
    return snap565(int(v[1:], 16))


def resolve_theme(
    name: str, themes: Mapping[str, Any], presets: Mapping[str, Any]
) -> tuple[dict[str, int], list[int]]:
    """Resolve a theme name to (tokens, 12 fills), all snapped to RGB565.

    `presets` maps preset key to an object with .tokens (dict of ints) and .palette (items with .fill);
    `themes` is {name: {extends, tokens: {name: "#RRGGBB"}, palette: {1..12: {fill: "#RRGGBB"}}}}."""
    chain: list[str] = []
    cur = name
    while cur not in presets or cur in themes:
        if cur in presets:
            raise ThemeError(f"{cur}: a user theme cannot reuse a preset name")
        if cur not in themes:
            raise ThemeError(f"unknown theme {cur!r}")
        if cur in chain:
            raise ThemeError(f"{cur}: circular extends")
        chain.append(cur)
        if len(chain) > MAX_DEPTH:
            raise ThemeError(f"{name}: extends chain deeper than {MAX_DEPTH}")
        cur = themes[cur].get("extends") if isinstance(themes[cur], Mapping) else None
        if not isinstance(cur, str):
            raise ThemeError(f"{chain[-1]}: extends must name a preset or another theme")
    base = presets[cur]
    tokens = {k: base.tokens[k] for k in TOKEN_NAMES}
    fills = [s.fill for s in base.palette]
    for n in reversed(chain):
        t = themes[n]
        for k, v in (t.get("tokens") or {}).items():
            if k not in TOKEN_NAMES:
                raise ThemeError(f"{n}: unknown token {k!r}")
            tokens[k] = _hex(v, f"{n}.tokens.{k}")
        for k, v in (t.get("palette") or {}).items():
            try:
                idx = int(k)
            except TypeError, ValueError:
                idx = 0
            if not 1 <= idx <= len(PALETTE_NAMES):
                raise ThemeError(f"{n}: palette index {k!r} must be 1..{len(PALETTE_NAMES)}")
            if not isinstance(v, Mapping) or set(v) != {"fill"}:
                raise ThemeError(f"{n}.palette.{k}: want {{fill: '#RRGGBB'}}")
            fills[idx - 1] = _hex(v["fill"], f"{n}.palette.{k}.fill")
    return tokens, fills
