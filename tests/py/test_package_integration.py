"""tools/package_integration.py: the zip HACS installs must be shaped the way HACS expects."""

import importlib.util
import json
import os
import subprocess
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
        (Path(d) / f).relative_to(pkg.SRC).as_posix()
        for d, ds, fs in os.walk(pkg.SRC, followlinks=True)
        if "__pycache__" not in d
        for f in fs
        if not f.endswith((".pyc", ".pyo"))
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


def test_zip_vendors_real_core_files_byte_identical_and_imports_standalone(tmp_path):
    """The zip carries real harmony_core files (never a symlink), equal to the source, and the
    unpacked integration resolves harmony_core relatively with no top-level copy on sys.path."""
    out = tmp_path / "p.zip"
    pkg.build(pkg.SRC, out, VERSION)
    core = ROOT / "harmony_core"
    src_files = {
        p.relative_to(core).as_posix(): p.read_bytes()
        for p in core.rglob("*.py")
        if "__pycache__" not in p.parts
    }
    assert "compiler.py" in src_files and "ir/pronto.py" in src_files
    with zipfile.ZipFile(out) as zf:
        for name, data in src_files.items():
            info = zf.getinfo(f"harmony_core/{name}")
            assert (info.external_attr >> 16) & 0o170000 != 0o120000  # not a symlink
            assert zf.read(info) == data
        extra = {n for n in zf.namelist() if n.startswith("harmony_core/") and n.endswith(".py")}
        assert extra == {f"harmony_core/{n}" for n in src_files}
        dest = tmp_path / "site" / "custom_components" / "ha_harmony"
        zf.extractall(dest)
    (tmp_path / "site" / "custom_components" / "__init__.py").write_text("")
    code = (
        "import sys\n"
        "import custom_components.ha_harmony.harmony_core.compiler as c\n"
        "import custom_components.ha_harmony.harmony_core.config_model\n"
        "import custom_components.ha_harmony.harmony_core.ir.pronto\n"
        "assert 'harmony_core' not in sys.modules\n"
        f"assert c.__file__.startswith({str(tmp_path / 'site')!r})\n"
    )
    r = subprocess.run(
        [sys.executable, "-c", code],
        check=False,
        cwd=tmp_path / "site",
        env={"PYTHONPATH": str(tmp_path / "site")},
        capture_output=True,
        text=True,
    )
    assert r.returncode == 0, r.stderr
