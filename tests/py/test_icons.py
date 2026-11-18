import importlib.util
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# Ids are append-only forever: this table pins them.
PINNED = [
    "tv",
    "film",
    "music",
    "game",
    "radio",
    "app",
    "power",
    "chev_right",
    "chev_left",
    "back",
    "up",
    "down",
    "home",
    "star",
    "plus",
    "minus",
    "check",
    "menu",
    "guide",
    "info",
    "input",
    "cc",
    "audio",
    "settings",
    "list",
    "play",
    "pause",
    "stop",
    "rewind",
    "ffwd",
    "skip_next",
    "skip_prev",
    "record",
    "vol_up",
    "vol_down",
    "mute",
    "ch",
    "bulb",
    "scene",
    "thermo",
    "blinds",
    "switch",
    "fan",
    "lock",
    "rf",
    "rf_off",
    "sync",
    "warn",
    "moon",
    "sun",
]


def _gen():
    spec = importlib.util.spec_from_file_location("gen_icons", ROOT / "tools" / "gen_icons.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_id_name_table_pinned():
    from harmony_core.icons import ICON_NAMES

    assert [ICON_NAMES[i] for i in range(1, len(PINNED) + 1)] == PINNED
    assert len(ICON_NAMES) >= len(PINNED)  # append-only: may grow, never reorder


def test_generated_files_fresh():
    for path, text in _gen().outputs().items():
        assert path.read_text() == text, (
            f"{path.name} stale: run `uv run python tools/gen_icons.py`"
        )


def test_svgs_are_24px_stroke_icons():
    for svg in (ROOT / "assets" / "icons").glob("*.svg"):
        t = svg.read_text()
        assert 'viewBox="0 0 24 24"' in t and 'stroke-width="2"' in t, svg.name
    names = {p.stem for p in (ROOT / "assets" / "icons").glob("*.svg")}
    assert names == set(PINNED)


def test_header_and_json_agree():
    h = (ROOT / "remote/app/icons.h").read_text()
    j = json.loads((ROOT / "harmony_addon/frontend-shared/icons.json").read_text())
    for ic in j["icons"]:
        assert f"#define RA_ICON_{ic['name'].upper()} {ic['id']}\n" in h
        assert ic["keywords"] and ic["svg"]
    assert re.search(rf"RA_ICON_MAX {len(j['icons'])}", h)
