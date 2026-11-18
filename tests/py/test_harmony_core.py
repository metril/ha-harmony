"""harmony_core must stay importable without Home Assistant."""

import pkgutil
import subprocess
import sys
from pathlib import Path

import harmony_core

ROOT = Path(__file__).resolve().parents[2]


def test_no_homeassistant_needed():
    mods = [m.name for m in pkgutil.walk_packages(harmony_core.__path__, "harmony_core.")]
    assert "harmony_core.ir.pronto" in mods and "harmony_core.compiler" in mods
    code = (
        "import sys, importlib\n"
        "class Block:\n"
        "    def find_spec(self, name, path=None, target=None):\n"
        "        if name == 'homeassistant' or name.startswith('homeassistant.'):\n"
        "            raise ImportError('blocked ' + name)\n"
        "sys.meta_path.insert(0, Block())\n"
        f"for m in {['harmony_core', *mods]!r}: importlib.import_module(m)\n"
        "assert not any(n.startswith('homeassistant') for n in sys.modules)\n"
    )
    r = subprocess.run(
        [sys.executable, "-c", code], cwd=ROOT, capture_output=True, text=True, check=False
    )
    assert r.returncode == 0, r.stderr


def test_versions():
    assert harmony_core.BUNDLE_SCHEMA == 1
    assert harmony_core.__version__
