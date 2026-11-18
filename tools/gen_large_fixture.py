"""Writes tests/py/fixtures/large_config.json: an invented, realistically large config (5 devices incl.
one Home Assistant device, ~90 IR commands, 3 activities, 14 pages incl. list pages) for editor tests.
    uv run python tools/gen_large_fixture.py
"""

import json
from pathlib import Path

OUT = Path(__file__).resolve().parents[1] / "tests" / "py" / "fixtures" / "large_config.json"


def nec(addr: int, cmd: int) -> dict:
    return {"proto": "nec", "addr": addr, "cmd": cmd}


def ha(service: str, entity: str, **data: object) -> dict:
    return {
        "ha": {
            "service": service,
            "target": {"entity_id": entity},
            **({"data": data} if data else {}),
        }
    }


def ir_device(did: str, name: str, addr: int, names: list[str], inputs: list[str]) -> dict:
    return {
        "id": did,
        "name": name,
        "settle_ms": 1500,
        "power": {"on": nec(addr, 2), "off": nec(addr, 3)},
        "inputs": {i: nec(addr, 100 + n) for n, i in enumerate(inputs)},
        "commands": {c: nec(addr, 10 + n) for n, c in enumerate(names)},
        "source": {"kind": "harmony_archive", "mfr": "Acme", "model_file": f"{did}.ir"},
    }


NAV = [
    "up",
    "down",
    "left",
    "right",
    "ok",
    "back",
    "menu",
    "home",
    "info",
    "guide",
    "exit",
    "options",
]
TRANSPORT = ["play", "pause", "stop", "rewind", "fast_forward", "record", "next", "previous"]
NUMS = [f"num_{n}" for n in range(10)]
VOL = ["vol_up", "vol_down", "mute"]
CH = ["ch_up", "ch_down", "last"]
TV = (
    VOL
    + CH
    + NAV
    + NUMS
    + [
        "sleep",
        "aspect",
        "picture_mode",
        "sound_mode",
        "subtitles",
        "teletext",
        "red",
        "green",
        "yellow",
        "blue",
        "source_toggle",
        "pip",
        "swap",
        "freeze",
    ]
)
AVR = (
    VOL
    + NAV[:6]
    + [
        "surround",
        "stereo",
        "direct",
        "bass_up",
        "bass_down",
        "treble_up",
        "treble_down",
        "dim",
        "tuner_up",
        "tuner_down",
        "preset_1",
        "preset_2",
        "preset_3",
        "night",
        "zone2",
        "eco",
    ]
)
STREAM = NAV[:8] + TRANSPORT + ["settings", "search", "voice", "app_a", "app_b"]
CONSOLE = ["up", "down", "left", "right", "select", "ps", "options", "share"]

widgets_home = [
    {
        "type": "button",
        "id": f"go_{n}",
        "x": n % 3,
        "y": n // 3,
        "label": lbl,
        "action": {"activity": a},
    }
    for n, (lbl, a) in enumerate([("Watch TV", "watch_tv"), ("Stream", "stream"), ("Game", "game")])
]
lights = [f"light.zone_{n}" for n in range(1, 9)]
list_buttons = [
    {
        "type": "button",
        "id": f"ch_{n}",
        "label": f"Channel {n}",
        "action": {"device": "tv", "command": f"num_{n % 10}"},
    }
    for n in range(1, 31)
]
list_tiles = [
    {"type": "tile", "id": f"lt_{n}", "label": f"Light {n}", "entity": e, "tap": "toggle"}
    for n, e in enumerate(lights, 1)
]
CONFIG = {
    "devices": [
        ir_device("tv", "Living room TV", 4, TV, ["hdmi1", "hdmi2", "hdmi3", "tuner", "usb"]),
        ir_device("avr", "AV receiver", 7, AVR, ["tv", "bd", "game", "aux", "phono", "tuner"]),
        ir_device("streamer", "Streaming box", 9, STREAM, []),
        ir_device("console", "Game console", 12, CONSOLE, []),
        {
            "id": "shield",
            "kind": "ha",
            "name": "Media shield",
            "entity": "media_player.shield",
            "power": {
                "on": ha("remote.turn_on", "remote.shield"),
                "off": ha("remote.turn_off", "remote.shield"),
            },
            "inputs": {
                "home": ha("media_player.select_source", "media_player.shield", source="Home")
            },
            "commands": {
                c: ha("remote.send_command", "remote.shield", command=c.upper())
                for c in [
                    "dpad_up",
                    "dpad_down",
                    "dpad_left",
                    "dpad_right",
                    "dpad_center",
                    "back",
                    "home",
                    "play_pause",
                    "rewind",
                    "fast_forward",
                ]
            },
        },
    ],
    "activities": [
        {
            "id": "watch_tv",
            "name": "Watch TV",
            "devices": [{"device": "tv", "input": "hdmi1"}, {"device": "avr", "input": "tv"}],
            "steps": [{"device": "avr", "command": "surround"}],
            "keys": {
                "volume_up": {"device": "avr", "command": "vol_up"},
                "volume_down": {"device": "avr", "command": "vol_down"},
                "volume_mute": {"device": "avr", "command": "mute"},
                "up": {"device": "tv", "command": "up"},
                "down": {"device": "tv", "command": "down"},
                "select": {"device": "tv", "command": "ok"},
            },
        },
        {
            "id": "stream",
            "name": "Stream",
            "devices": [
                {"device": "tv", "input": "hdmi2"},
                {"device": "shield"},
                {"device": "avr", "input": "bd"},
            ],
            "keys": {
                "up": ha("remote.send_command", "remote.shield", command="DPAD_UP") | {},
                "play": {"device": "shield", "command": "play_pause"},
                "volume_up": {"device": "avr", "command": "vol_up"},
                "volume_down": {"device": "avr", "command": "vol_down"},
            },
        },
        {
            "id": "game",
            "name": "Game",
            "devices": [
                {"device": "tv", "input": "hdmi3"},
                {"device": "console"},
                {"device": "avr", "input": "game"},
            ],
            "keys": {"volume_up": {"device": "avr", "command": "vol_up"}},
        },
    ],
    "keys": {
        "power_off_activity": {"activity": "off"},
        "volume_up": {"device": "avr", "command": "vol_up"},
        "volume_down": {"device": "avr", "command": "vol_down"},
        "volume_mute": {"device": "avr", "command": "mute"},
        "channel_up": {"device": "tv", "command": "ch_up"},
        "channel_down": {"device": "tv", "command": "ch_down"},
    },
    "pages": [
        {
            "id": "home",
            "kind": "home",
            "title": "Home",
            "cols": 3,
            "rows": 3,
            "widgets": widgets_home,
        },
        {
            "id": "favs",
            "kind": "favorites",
            "title": "Favorites",
            "list": {"rows": 6},
            "widgets": [{**w, "id": "f" + w["id"]} for w in list_buttons[:8]],
        },
        {
            "id": "channels",
            "title": "Channels",
            "activity": "watch_tv",
            "list": {"rows": 8},
            "widgets": list_buttons,
        },
        {"id": "lights", "title": "Lights", "list": {"rows": 4}, "widgets": list_tiles},
        {
            "id": "watch",
            "title": "Watch TV",
            "cols": 3,
            "rows": 4,
            "widgets": [
                {
                    "type": "button",
                    "id": "w_sleep",
                    "x": 0,
                    "y": 0,
                    "label": "Sleep",
                    "action": {"device": "tv", "command": "sleep"},
                },
                {
                    "type": "button",
                    "id": "w_pic",
                    "x": 1,
                    "y": 0,
                    "label": "Picture",
                    "action": {"device": "tv", "command": "picture_mode"},
                },
                {
                    "type": "tile",
                    "id": "w_temp",
                    "x": 0,
                    "y": 1,
                    "w": 2,
                    "label": "Room",
                    "entity": "sensor.living_room_temperature",
                },
                {
                    "type": "tile",
                    "id": "w_lamp",
                    "x": 2,
                    "y": 1,
                    "label": "Lamp",
                    "entity": "light.lamp",
                    "tap": ha("light.turn_on", "light.lamp", brightness=120),
                },
                {"type": "button", "id": "w_next", "x": 2, "y": 3, "label": "More", "nav": "next"},
            ],
        },
        {
            "id": "stream_p",
            "title": "Stream",
            "activity": "stream",
            "cols": 3,
            "rows": 3,
            "widgets": [
                {
                    "type": "button",
                    "id": "s_app_a",
                    "x": 0,
                    "y": 0,
                    "label": "App A",
                    "action": {"device": "streamer", "command": "app_a"},
                },
                {
                    "type": "button",
                    "id": "s_shield",
                    "x": 1,
                    "y": 0,
                    "label": "Shield home",
                    "action": {"device": "shield", "command": "home"},
                },
                {"type": "button", "id": "s_back", "x": 2, "y": 0, "label": "Back", "nav": "home"},
            ],
        },
    ]
    + [
        {
            "id": f"misc_{n}",
            "title": f"Page {n}",
            "cols": 4,
            "rows": 4,
            "widgets": [
                {
                    "type": "button",
                    "id": f"m{n}_{k}",
                    "x": k % 4,
                    "y": k // 4,
                    "label": f"B{k}",
                    "action": {"device": "avr", "command": AVR[k % len(AVR)]},
                }
                for k in range(6)
            ]
            + [{"type": "label", "x": 0, "y": 3, "w": 4, "label": f"Section {n}"}],
        }
        for n in range(1, 9)
    ],
    "editor": {"handlers": {"watch_tv": {"volume": "avr", "nav": "tv"}}},
}

if __name__ == "__main__":
    OUT.write_text(json.dumps(CONFIG, indent=1) + "\n")
    print(OUT)
