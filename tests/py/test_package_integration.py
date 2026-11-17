"""tools/package_integration.py: the zip HACS installs must be shaped the way HACS expects."""

import importlib.util
import json
import sys
import zipfile
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
_spec = importlib.util.spec_from_file_location("pkg", ROOT / "tools/package_integration.py")
pkg = importlib.util.module_from_spec(_spec)
sys.modules["pkg"] = pkg
_spec.loader.exec_module(pkg)

VERSION = json.loads((pkg.SRC / "manifest.json").read_text())["version"]


def test_files_sit_at_the_zip_root_with_manifest_on_top(tmp_path):
    """HACS unpacks into custom_components/ha_harmony/, so no leading directory."""
    out = tmp_path / "ha_harmony.zip"
    names = pkg.build(pkg.SRC, out, VERSION)
    with zipfile.ZipFile(out) as zf:
        listed = zf.namelist()
        assert json.loads(zf.read("manifest.json"))["domain"] == "ha_harmony"
    assert listed == names
    assert "__init__.py" in listed and "translations/en.json" in listed
    assert not any(n.startswith("ha_harmony/") for n in listed)


def test_every_source_file_is_included_and_no_bytecode(tmp_path):
    out = tmp_path / "p.zip"
    pkg.build(pkg.SRC, out, VERSION)
    with zipfile.ZipFile(out) as zf:
        listed = set(zf.namelist())
    expected = {
        p.relative_to(pkg.SRC).as_posix()
        for p in pkg.SRC.rglob("*")
        if p.is_file() and "__pycache__" not in p.parts and p.suffix not in (".pyc", ".pyo")
    }
    assert listed == expected
    assert not any("__pycache__" in n or n.endswith(".pyc") for n in listed)


def test_version_must_match_the_manifest(tmp_path):
    with pytest.raises(SystemExit, match="manifest.json version"):
        pkg.build(pkg.SRC, tmp_path / "p.zip", "9.9.9")


def test_package_is_reproducible(tmp_path):
    a, b = tmp_path / "a.zip", tmp_path / "b.zip"
    pkg.build(pkg.SRC, a, VERSION)
    pkg.build(pkg.SRC, b, VERSION)
    assert a.read_bytes() == b.read_bytes()


def test_hacs_json_points_at_the_package():
    hacs = json.loads((ROOT / "hacs.json").read_text())
    assert hacs["zip_release"] is True
    assert hacs["filename"] == "ha_harmony.zip"
