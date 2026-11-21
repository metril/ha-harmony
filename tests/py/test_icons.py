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
    "search",
    "grid_apps",
    "numpad",
    "replay10",
    "skip10",
    "shuffle",
    "repeat",
    "aspect",
    "picture",
    "sleep_timer",
    "prev_ch",
    "enter",
    "keyboard",
    "audio_track",
    "plex",
    "youtube",
    "netflix",
    "prime_video",
    "disney_plus",
    "hbo_max",
    "hulu",
    "apple_tv",
    "paramount_plus",
    "peacock",
    "crunchyroll",
    "twitch",
    "jellyfin",
    "emby",
    "kodi",
    "plexamp",
    "spotify",
    "apple_music",
    "tidal",
    "soundcloud",
    "steam",
    "xbox",
    "playstation",
    "nintendo_switch",
    "nvidia",
    "sonos",
    "chromecast",
    "speaker",
    "soundbar",
    "receiver",
    "streamer",
    "projector",
    "kvm",
    "plug",
    "pc",
    "laptop",
    "hdmi1",
    "hdmi2",
    "hdmi3",
    "hdmi4",
    "av",
    "usb",
    "cctv",
    "cctv_off",
    "garage",
]

from harmony_core.icons import BRAND_COLORS, ICON_NAMES

BRAND = {ICON_NAMES[i] for i in BRAND_COLORS}


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
        assert 'viewBox="0 0 24 24"' in t, svg.name
        # brand logos (Simple Icons, CC0) are filled marks; everything else is a 2 px stroke icon
        assert 'stroke-width="2"' in t or svg.stem in BRAND, svg.name
    names = {p.stem for p in (ROOT / "assets" / "icons").glob("*.svg")}
    assert names == set(PINNED)


def test_header_and_json_agree():
    h = (ROOT / "remote/app/icons.h").read_text()
    j = json.loads((ROOT / "harmony_addon/frontend-shared/icons.json").read_text())
    for ic in j["icons"]:
        assert f"#define RA_ICON_{ic['name'].upper()} {ic['id']}\n" in h
        assert ic["keywords"] and (ic["svg"] or ic.get("brand"))  # brand markup is in brand-icons.json
    assert re.search(rf"RA_ICON_MAX {len(j['icons'])}", h)


def test_no_surface_keeps_its_own_icon_cap():
    """The cap lives in proto/icons.yaml only: the integration (validator), the remote and the editor all take it from
    the generated tables, and no C source on the hub or remote hard-codes a number or a name list of its own."""
    from harmony_core.icons import ICON_IDS, MAX_STOCK_ID

    cap = len(ICON_IDS)
    assert cap == MAX_STOCK_ID == len(json.loads((ROOT / "harmony_addon/frontend-shared/icons.json").read_text())["icons"])
    assert f"RA_ICON_MAX {cap}\n" in (ROOT / "remote/app/icons.h").read_text()
    for src in [*(ROOT / "hub").rglob("*.[ch]"), *(ROOT / "common").rglob("*.[ch]")]:
        assert not re.search(r"icon\w*\s*(>|>=|<|<=)\s*\d{2,}", src.read_text()), src
    for src in (ROOT / "remote" / "app").glob("*.[ch]"):
        if src.name != "icons.h":
            assert not re.search(r"icon\w*\s*(>|>=)\s*\d{2,}", src.read_text()), src


def test_package_ships_the_generated_icons(tmp_path):
    import zipfile

    from tools.package_integration import build

    out = tmp_path / "p.zip"
    build(
        ROOT / "custom_components" / "ha_harmony",
        out,
        json.loads((ROOT / "custom_components/ha_harmony/manifest.json").read_text())["version"],
    )
    with zipfile.ZipFile(out) as z:
        assert z.read("harmony_core/icons.py").decode() == (ROOT / "harmony_core/icons.py").read_text()


def test_brand_icons_are_the_filled_logos_with_hex_cards():
    assert set(BRAND_COLORS) == set(range(65, 65 + len(BRAND)))
    assert all(re.fullmatch(r"#[0-9A-F]{6}", c) for c in BRAND_COLORS.values())
    for n in BRAND:
        assert 'fill="#000"' in (ROOT / "assets" / "icons" / f"{n}.svg").read_text()
