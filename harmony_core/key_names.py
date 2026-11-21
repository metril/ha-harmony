"""Display names for device commands (twin of addon/frontend/src/lib/key-names.ts `displayName` / `abbreviate`).

"DirectionUp" -> "Up", "InputHdmi1" -> "HDMI 1", "vol_up" -> "Volume Up". Both sides run
tests/vectors/key_names.json. Stored names never change; this is only what a label says.
"""

from __future__ import annotations

import re

_NAMES = {
    "select": "OK",
    "ok": "OK",
    "dvr": "DVR",
    "tv": "TV",
    "fast_forward": "Fast Forward",
    "play_pause": "Play/Pause",
    "prev_channel": "Previous Channel",
    "arrowup": "Arrow Up",
    "arrowdown": "Arrow Down",
    "arrowleft": "Arrow Left",
    "arrowright": "Arrow Right",
    "direction_up": "Up",
    "direction_down": "Down",
    "direction_left": "Left",
    "direction_right": "Right",
    "directionup": "Up",
    "directiondown": "Down",
    "directionleft": "Left",
    "directionright": "Right",
}
_ACRONYMS = frozenset(
    ["ok", "dvr", "tv", "hdmi", "usb", "ir", "av", "dvd", "cd", "hd", "uhd", "sd", "epg", "ui", "hdr", "arc", "pip", "bt", "ha", "am", "fm", "cec", "rf", "led", "lcd", "osd", "pc", "vcr", "pvr", "nec", "sirc", "dts", "cc", "vod", "kvm"]
)
_BRAND = {
    "earc": "eARC",
    "dpad": "D-Pad",
    "necext": "NECext",
    "youtube": "YouTube",
    "iphone": "iPhone",
    "ipad": "iPad",
    "ipod": "iPod",
    "playstation": "PlayStation",
    "airplay": "AirPlay",
    "directv": "DirecTV",
    "tivo": "TiVo",
    "wifi": "WiFi",
    "hbo": "HBO",
    "hulu": "Hulu",
}
_SPLIT_WORDS = frozenset(["ch", "vol", "num", "key", "input", "digit", "page", "mode", "scene"])
_UNITS = frozenset(["s", "ms", "min", "sec", "h", "hz", "khz", "mhz", "db", "st", "nd", "rd", "th", "m", "cm", "mm"])
_CMD_WORDS = {
    "vol": "volume",
    "ch": "channel",
    "chan": "channel",
    "pwr": "power",
    "prev": "previous",
    "pct": "percent",
}
_SMALL = frozenset(
    ["a", "an", "and", "of", "the", "to", "for", "at"]
)  # "on" / "in" stay capitalised in a command name
_DIGIT = re.compile(r"^(?:key_10|num_?|digit_?|number_?)(\d)$")
_HDMI_INPUT = re.compile(r"^input_hdmi_?(\d+)$")


def _lookup(k: str) -> str:
    k = re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", k)
    k = re.sub(r"([A-Za-z])(\d)", r"\1_\2", k)
    return re.sub(r"[\s-]+", "_", k).lower()


def _tokens(name: str) -> list[str]:
    out: list[str] = []
    for raw in filter(None, re.split(r"_+|\s+|(?<=[A-Za-z0-9])-(?=[A-Za-z0-9])", name)):
        m = re.search(r"[+-]+$", raw)
        sym = m.group(0) if m else ""
        part = raw[: -len(sym)] if sym else raw
        if not part:
            out.append(raw)
            continue
        if re.match(r"^[a-z][A-Z]", part) or part.lower() in _BRAND:
            pieces = [part]
        else:
            p = re.sub(r"([a-z])([A-Z])", r"\1 \2", part)
            p = re.sub(r"([A-Z]+)([A-Z][a-z])", r"\1 \2", p)
            p = re.sub(r"([A-Za-z]\d+)(?=[A-Z][a-z])", r"\1 ", p)
            pieces = p.split(" ")
        for piece in pieces:
            low = piece.lower()
            ld = re.match(r"^([A-Za-z]+)(\d+)$", piece)
            dl = re.match(r"^(\d+)([A-Za-z]+)$", piece)
            if low in _BRAND:
                out.append(_BRAND[low])
            elif re.match(r"^[a-z][A-Z]", piece):
                out.append(piece)
            elif ld:
                lo = ld.group(1).lower()
                if lo in _ACRONYMS or lo in _SPLIT_WORDS or len(lo) >= 4:
                    out += [lo.upper() if lo in _ACRONYMS else lo, ld.group(2)]
                else:
                    out.append(piece.upper())
            elif dl:
                lo = dl.group(2).lower()
                if lo in _UNITS:
                    out.append(piece.lower())
                elif len(lo) <= 2:
                    out.append(piece.upper())
                else:
                    out += [dl.group(1), lo]
            elif low in _ACRONYMS:
                out.append(low.upper())
            else:
                out.append(low)
        if sym:
            out[-1] += sym
    return [_CMD_WORDS.get(t, t) for t in out]


def _bare(t: str) -> bool:
    return re.fullmatch(r"[a-z]+[+-]*", t) is not None


def _title(words: list[str]) -> str:
    def one(i: int, w: str) -> str:
        if not _bare(w):
            return w
        if len(w) == 1:
            return w.upper()
        return w if i > 0 and w in _SMALL else w[0].upper() + w[1:]

    return " ".join(one(i, w) for i, w in enumerate(words))


def _title_human(name: str) -> str:
    out = []
    for i, w in enumerate(name.split()):
        m = re.fullmatch(r"([^A-Za-z]*)([a-z]+)([^A-Za-z]*)", w)
        if not m:
            out.append(w)
        elif len(m.group(2)) == 1:
            out.append(m.group(1) + m.group(2).upper() + m.group(3))
        elif i > 0 and m.group(2) in _SMALL:
            out.append(w)
        else:
            out.append(m.group(1) + m.group(2).capitalize() + m.group(3))
    return " ".join(out)


def display_name(name: str) -> str:
    """The short name a page button says for a command (no device prefix)."""
    if re.search(r"\S\s+\S", name.strip()):  # already written for people: only tidied
        return _title_human(name)
    c = _lookup(name)
    h = _HDMI_INPUT.match(c)
    if h:
        return f"HDMI {h.group(1)}"
    if c in _NAMES:
        return _NAMES[c]
    d = _DIGIT.match(c)
    if d:
        return d.group(1)
    n = re.fullmatch(r"key_(\d+)", c)
    if n:
        return f"Key {n.group(1)}"
    words = _tokens(name)
    return _title(words) if words else name


_PHRASES = (
    ("Picture Mode", "Pic Mode"),
    ("Aspect Ratio", "Aspect"),
    ("Input HDMI", "HDMI"),
    ("Fast Forward", "FF"),
)
SHORT = {
    "Volume": "Vol",
    "Channel": "Ch",
    "Previous": "Prev",
    "Settings": "Setup",
    "Favourite": "Fav",
    "Favorite": "Fav",
    "Favourites": "Favs",
    "Favorites": "Favs",
    "Subtitles": "CC",
}


def abbreviate(label: str) -> str:
    """Short forms for a label that is over its pixel budget; never applied otherwise."""
    for a, b in _PHRASES:
        label = re.sub(rf"\b{a}\b", b, label)
    return " ".join(SHORT.get(w, w) for w in label.split(" "))


_INPUT_HDMI = re.compile(r"^(?:input_)?hdmi_?(\d+)(?:_?mhl|_?arc|_?earc)?$")
_INPUT_PORT = re.compile(r"^input_(av|usb|tv)(?:_?\d+)?$")


def normalize_input(name: str) -> str | None:
    """"InputHdmi1", "HDMI1", "Input HDMI 2" -> "HDMI 1" / "HDMI 2"; "InputAv" -> "AV", "InputUsb" -> "USB",
    "InputTv" -> "TV". None when the command is not an input selector (twin of studio-model.ts
    `normalizeInput`; both run tests/vectors/inputs.json)."""
    c = _lookup(name.strip())
    h = _INPUT_HDMI.match(c)
    if h:
        return f"HDMI {int(h.group(1))}"
    p = _INPUT_PORT.match(c)
    return p.group(1).upper() if p else None
