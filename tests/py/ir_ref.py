"""Test-only reference IR encoder, written independently of hub/ir/encode.c.

Works on (level, duration) runs, then run-length merges; returns
(carrier_hz, once, repeat) with alternating mark/space durations, mark first.
"""

from __future__ import annotations


def _flat(runs):
    out: list[list[int]] = []
    for lvl, d in runs:
        if out and out[-1][0] == lvl:
            out[-1][1] += d
        else:
            out.append([lvl, d])
    while out and out[0][0] == 0:  # a frame is listed from its first mark
        out.pop(0)
    return [d for _, d in out]


def _pad(runs, total):
    # trailing space is clamped to the uint16 duration range; period is reported separately
    tail = 0
    for lvl, d in reversed(runs):
        if lvl:
            break
        tail += d
    return [*runs, (0, min(65535 - tail, total - sum(d for _, d in runs)))]


PERIODS = {"nec": 108000, "necext": 108000, "samsung32": 108000, "rc5": 113792, "rc6": 107000,
           "sirc12": 45000, "sirc15": 45000, "sirc20": 45000}


def period_us(code):
    return PERIODS.get(code.get("proto"), 0)


def _bits(value, n, lsb_first):
    r = range(n) if lsb_first else range(n - 1, -1, -1)
    return [(value >> i) & 1 for i in r]


def _check(v, hi):
    if not 0 <= v <= hi:
        raise ValueError(v)


def _pulse_distance(bits, mark, z, o):
    runs = []
    for b in bits:
        runs += [(1, mark), (0, o if b else z)]
    return runs


def nec_like(proto, addr, cmd):
    if proto == "nec":
        _check(addr, 255)
        abits = _bits(addr, 8, True) + _bits(255 - addr, 8, True)
    else:
        _check(addr, 65535)
        abits = _bits(addr, 16, True)
    _check(cmd, 255)
    cbits = _bits(cmd, 8, True) + _bits(255 - cmd, 8, True)
    frame = [(1, 9000), (0, 4500), *_pulse_distance(abits + cbits, 562, 562, 1687), (1, 562)]
    rep = [(1, 9000), (0, 2250), (1, 562)]
    return 38000, _flat(_pad(frame, 108000)), _flat(_pad(rep, 108000))


def samsung32(addr, cmd):
    _check(addr, 255)
    _check(cmd, 255)
    bits = _bits(addr, 8, True) * 2 + _bits(cmd, 8, True) + _bits(255 ^ cmd, 8, True)
    frame = [(1, 4500), (0, 4500), *_pulse_distance(bits, 560, 560, 1690), (1, 560)]
    f = _flat(_pad(frame, 108000))
    return 38000, f, f


def sirc(n, addr, cmd):
    abits = {12: 5, 15: 8, 20: 13}[n]
    _check(addr, (1 << abits) - 1)
    _check(cmd, 127)
    bits = _bits(cmd, 7, True) + _bits(addr, abits, True)
    runs = [(1, 2400), (0, 600)]
    for b in bits:
        runs += [(1, 1200 if b else 600), (0, 600)]
    f = _flat(_pad(runs, 45000))
    return 40000, f, f


def rc5(addr, cmd, toggle):
    _check(addr, 31)
    _check(cmd, 127)
    bits = [1, 0 if cmd >= 64 else 1, 1 if toggle else 0, *_bits(addr, 5, False), *_bits(cmd & 63, 6, False)]
    half = 889
    runs = []
    for b in bits:  # 1 = space,mark ; 0 = mark,space
        runs += [(0, half), (1, half)] if b else [(1, half), (0, half)]
    # period = 64 bit times, counted from the (unlisted) leading idle half
    f = _flat(_pad(runs, 128 * half))
    return 36000, f, f


def rc6(addr, cmd, toggle):
    _check(addr, 255)
    _check(cmd, 255)
    t = 444

    def bit(b, w=t):
        return [(1, w), (0, w)] if b else [(0, w), (1, w)]

    runs = [(1, 6 * t), (0, 2 * t), *bit(1)]
    for _ in range(3):
        runs += bit(0)
    runs += bit(1 if toggle else 0, 2 * t)
    for b in _bits(addr, 8, False) + _bits(cmd, 8, False):
        runs += bit(b)
    f = _flat(_pad(runs, 107000))
    return 36000, f, f


def pronto(words):
    if len(words) < 4 or words[0] != 0 or words[1] == 0:
        raise ValueError("header")
    once, rep = words[2], words[3]
    if once + rep == 0 or len(words) != 4 + 2 * (once + rep):
        raise ValueError("count")
    unit = words[1] * 0.241246
    durs = [int(w * unit + 0.5) for w in words[4:]]
    if any(d == 0 or d > 65535 for d in durs):
        raise ValueError("dur")
    return int(1e6 / unit + 0.5), durs[: 2 * once], durs[2 * once :]


def encode(code: dict, toggle: int = 0):
    """code: {"proto","addr","cmd"} | {"raw":{"hz","us"}} | {"pronto": "hex string"}"""
    if "raw" in code:
        hz, us = code["raw"]["hz"], code["raw"]["us"]
        if len(us) == 0 or len(us) % 2 or any(not 0 < d <= 65535 for d in us):
            raise ValueError("raw")
        return hz, list(us), []
    if "pronto" in code:
        return pronto([int(w, 16) for w in code["pronto"].split()])
    p, a, c = code["proto"], code["addr"], code["cmd"]
    if p in ("nec", "necext"):
        return nec_like(p, a, c)
    if p == "samsung32":
        return samsung32(a, c)
    if p.startswith("sirc"):
        return sirc(int(p[4:]), a, c)
    if p == "rc5":
        return rc5(a, c, toggle)
    if p == "rc6":
        return rc6(a, c, toggle)
    raise ValueError(p)
