"""The editor's frontend tests use a keymap response fixture (addon/frontend/test/fixtures/keymap.json).
It must equal what the backend produces, so the remote drawing's key list cannot drift from the key table.
Regenerate with: HH_UPDATE_FIXTURES=1 uv run pytest tests/py/test_addon_frontend_fixture.py -n0"""

import json
import os
from pathlib import Path

from harmony_core.config_model import parse_model
from harmony_core.keymap import resolve_keymaps

FIXTURE = (
    Path(__file__).resolve().parents[2] / "addon" / "frontend" / "test" / "fixtures" / "keymap.json"
)
CONFIG = """
devices:
  - id: tv
    name: TV
    commands: {menu: {proto: nec, addr: 4, cmd: 20}}
  - id: box
    name: Box
    commands: {ok: {proto: nec, addr: 4, cmd: 21}}
activities:
  - id: stream
    name: Stream
    devices: [{device: box}]
    keys: {select: {device: box, command: ok}, stop: {none: true}}
keys:
  menu: {device: tv, command: menu}
  home: {activity: stream}
"""


def test_keymap_fixture_matches_backend():
    got = {"ok": True, **resolve_keymaps(parse_model(CONFIG))}
    text = json.dumps(got, indent=1) + "\n"
    if os.environ.get("HH_UPDATE_FIXTURES"):
        FIXTURE.parent.mkdir(parents=True, exist_ok=True)
        FIXTURE.write_text(text)
    assert json.loads(FIXTURE.read_text()) == got, "run with HH_UPDATE_FIXTURES=1 to regenerate"
    assert len(got["default"]) == 35
