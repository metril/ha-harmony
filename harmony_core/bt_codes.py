"""Bluetooth HID codes: built-in command names and the explicit `HID:<page>:<usage>` form.

Only the keyboard page (7) and the consumer page (12) exist. `resolve` turns a command value
(a built-in name or an explicit code) into `(page, usage)`; the compiler sends it to the hub as
`{"type":"bt","page":p,"usage":u,"host":slot}`. No Home Assistant imports.
"""

from __future__ import annotations

import re

PAGE_KEYBOARD = 7
PAGE_CONSUMER = 12
PAGES = (PAGE_KEYBOARD, PAGE_CONSUMER)
USAGE_MAX = {PAGE_KEYBOARD: 0xFF, PAGE_CONSUMER: 0x3FF}  # what the hub sends as a code
BT_MAX_HOSTS = 255  # bt devices per hub: slot 0..254 = index in config order (hub: HH_BT_MAX_HOSTS)
TRANSPORTS = ("auto", "classic", "le")

K, C = PAGE_KEYBOARD, PAGE_CONSUMER

# canonical name -> (page, usage); `_ALIASES` below adds the other spellings
_CORE: dict[str, tuple[int, int]] = {
    "DirectionUp": (K, 0x52),
    "DirectionDown": (K, 0x51),
    "DirectionLeft": (K, 0x50),
    "DirectionRight": (K, 0x4F),
    "Select": (K, 0x28),
    "Back": (C, 0x224),
    "Home": (C, 0x223),
    "Menu": (C, 0x40),
    "Guide": (C, 0x8D),
    "Info": (C, 0x1BD),
    "Play": (C, 0xB0),
    "Pause": (C, 0xB1),
    "PlayPause": (C, 0xCD),
    "Stop": (C, 0xB7),
    "SkipForward": (C, 0xB5),
    "SkipBack": (C, 0xB6),
    "FastForward": (C, 0xB3),
    "Rewind": (C, 0xB4),
    "VolumeUp": (C, 0xE9),
    "VolumeDown": (C, 0xEA),
    "Mute": (C, 0xE2),
    "PowerToggle": (C, 0x30),
    "Search": (C, 0x221),
    "ChannelUp": (C, 0x9C),
    "ChannelDown": (C, 0x9D),
    "Red": (C, 0x69),
    "Green": (C, 0x6A),
    "Yellow": (C, 0x6C),
    "Blue": (C, 0x6B),
    "Escape": (K, 0x29),
    "Backspace": (K, 0x2A),
    "Tab": (K, 0x2B),
    "Space": (K, 0x2C),
    "ContextMenu": (K, 0x65),
    "PageUp": (K, 0x4B),
    "PageDown": (K, 0x4E),
    "Insert": (K, 0x49),
    "Delete": (K, 0x4C),
    "End": (K, 0x4D),
    "CapsLock": (K, 0x39),
}
_DIGITS = (0x27, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26)
_CORE.update({f"Number{n}": (K, u) for n, u in enumerate(_DIGITS)})
# Rarely used keys: built-in names, but left out of the default command set.
_EXTRA: dict[str, tuple[int, int]] = {
    **{f"F{n}": (K, 0x39 + n) for n in range(1, 13)},
    "Minus": (K, 0x2D),
    "Equal": (K, 0x2E),
    "LeftBracket": (K, 0x2F),
    "RightBracket": (K, 0x30),
    "Backslash": (K, 0x31),
    "Semicolon": (K, 0x33),
    "Apostrophe": (K, 0x34),
    "Grave": (K, 0x35),
    "Comma": (K, 0x36),
    "Period": (K, 0x37),
    "Slash": (K, 0x38),
}
_ALIASES: dict[str, str] = {
    "arrowup": "DirectionUp", "up": "DirectionUp",
    "arrowdown": "DirectionDown", "down": "DirectionDown",
    "arrowleft": "DirectionLeft", "left": "DirectionLeft",
    "arrowright": "DirectionRight", "right": "DirectionRight",
    "ok": "Select", "enter": "Select",
    "return": "Back", "exit": "Back",
    "play_pause": "PlayPause",
    "nexttrack": "SkipForward", "next": "SkipForward",
    "previoustrack": "SkipBack", "prev": "SkipBack",
    "vol_up": "VolumeUp", "vol_down": "VolumeDown",
    "power": "PowerToggle",
    "ch_up": "ChannelUp", "ch_down": "ChannelDown",
    "esc": "Escape", "pageup": "PageUp", "pagedown": "PageDown", "capslock": "CapsLock",
}  # fmt: skip

BUILTINS: dict[str, tuple[int, int]] = {**_CORE, **_EXTRA}
DEFAULT_COMMANDS: tuple[str, ...] = tuple(_CORE)  # the command set of a device without `commands`

_FOLDED: dict[str, tuple[int, int]] = {n.lower(): v for n, v in BUILTINS.items()}
_FOLDED.update({a: BUILTINS[c] for a, c in _ALIASES.items()})
_FOLDED.update({str(n): (K, u) for n, u in enumerate(_DIGITS)})
_FOLDED.update({chr(97 + i): (K, 4 + i) for i in range(26)})  # a..z (lower case only)

_HID = re.compile(r"^HID:([0-9A-Fa-f]{2}):([0-9A-Fa-f]{4})$")
_HEX = re.compile(r"^0[xX]([0-9A-Fa-f]{1,4})$")
_ADDR = re.compile(r"^[0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5}$")


class BtCodeError(ValueError):
    pass


def norm_address(v: object) -> str:
    """`aa:bb:cc:dd:ee:ff` -> `AA:BB:CC:DD:EE:FF`."""
    if not isinstance(v, str) or not _ADDR.match(v.strip()):
        raise BtCodeError("a Bluetooth address looks like AA:BB:CC:DD:EE:FF")
    return v.strip().upper()


def parse_hid(text: str) -> tuple[int, int]:
    """`HID:0C:0223` -> (12, 0x223). Page and usage are hex; the page must be 07 or 0C."""
    m = _HID.match(text.strip()) if isinstance(text, str) else None
    if not m:
        raise BtCodeError("not a HID code: use HID:<page hex>:<usage hex>, e.g. HID:0C:0223")
    page, usage = int(m[1], 16), int(m[2], 16)
    if page not in PAGES:
        raise BtCodeError(
            f"HID page {m[1].upper()} is not supported: use 07 (keyboard) or 0C (consumer)"
        )
    if not 1 <= usage <= USAGE_MAX[page]:
        raise BtCodeError(
            f"usage {m[2].upper()} is out of range for page {page:02X}: 0001..{USAGE_MAX[page]:04X}"
        )
    return page, usage


def resolve(value: str) -> tuple[int, int]:
    """A command value (built-in name or `HID:pp:uuuu`) -> (page, usage)."""
    if not isinstance(value, str) or not value.strip():
        raise BtCodeError("a bt command is a built-in name or HID:<page>:<usage>")
    v = value.strip()
    if len(v) == 1 and v.isupper():
        raise BtCodeError(f"{v!r}: an upper-case letter needs Shift; use the lower-case letter")
    if v[:4].upper() == "HID:":
        return parse_hid(v)
    if m := _HEX.match(v):  # `0x0221`: a consumer usage
        usage = int(m[1], 16)
        if not 1 <= usage <= USAGE_MAX[PAGE_CONSUMER]:
            raise BtCodeError(
                f"usage out of range for the consumer page: 0001..{USAGE_MAX[PAGE_CONSUMER]:04X}"
            )
        return PAGE_CONSUMER, usage
    got = _FOLDED.get(v.lower())
    if got is None:
        raise BtCodeError(
            f"unknown bt command {v!r}: use a built-in name (e.g. DirectionUp, VolumeUp)"
            " or HID:<page>:<usage>"
        )
    return got


BT_MAX_REMOTES = (
    32  # physical Bluetooth remotes (input) per hub, slot 0..31 (hub: HH_BT_MAX_REMOTES)
)
REMOTE_MAX_KEYS = 255  # usage -> key entries of one remote, defaults included

# Default usage -> key map of a Bluetooth remote with no `keys` (the same key names as the
# Elite's hard keys, so per-activity key maps apply unchanged). Digits are the free logical
# keys key_100 .. key_109.
DEFAULT_REMOTE_KEYS: tuple[tuple[str, str], ...] = (
    ("DirectionUp", "up"),
    ("DirectionDown", "down"),
    ("DirectionLeft", "left"),
    ("DirectionRight", "right"),
    ("Select", "select"),
    ("Back", "exit"),
    ("Escape", "exit"),
    ("Home", "home"),
    ("Menu", "menu"),
    ("Guide", "guide"),
    ("Info", "info"),
    ("Play", "play"),
    ("PlayPause", "play"),
    ("Pause", "pause"),
    ("Stop", "stop"),
    ("FastForward", "fast_forward"),
    ("Rewind", "rewind"),
    ("VolumeUp", "volume_up"),
    ("VolumeDown", "volume_down"),
    ("Mute", "volume_mute"),
    ("ChannelUp", "channel_up"),
    ("ChannelDown", "channel_down"),
    ("PowerToggle", "power_off_activity"),
    ("Red", "red"),
    ("Green", "green"),
    ("Yellow", "yellow"),
    ("Blue", "blue"),
    *((f"Number{n}", f"key_{100 + n}") for n in range(10)),
)
