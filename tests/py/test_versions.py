"""One release number: the integration, the add-on, the shared core and the frontend package agree, and both
changelogs have a heading for it."""

import json
import re
from pathlib import Path

import harmony_addon
import harmony_core

ROOT = Path(__file__).resolve().parents[2]


def _headings(path: Path) -> list[str]:
    return re.findall(r"^## (\d+\.\d+\.\d+)\b", path.read_text(), re.M)


def test_versions_agree():
    manifest = json.loads((ROOT / "custom_components/ha_harmony/manifest.json").read_text())["version"]
    addon = re.search(r'^version:\s*"?([\d.]+)"?\s*$', (ROOT / "addon/config.yaml").read_text(), re.M)
    pkg = json.loads((ROOT / "addon/frontend/package.json").read_text())["version"]
    lock = json.loads((ROOT / "addon/frontend/package-lock.json").read_text())
    assert addon is not None
    versions = {
        "manifest": manifest,
        "addon config": addon.group(1),
        "harmony_addon": harmony_addon.__version__,
        "harmony_core": harmony_core.__version__,
        "package.json": pkg,
        "package-lock": lock["version"],
        "package-lock root": lock["packages"][""]["version"],
    }
    assert len(set(versions.values())) == 1, versions


def test_changelogs_newest_heading_is_the_release():
    version = harmony_core.__version__
    for name in ("CHANGELOG.md", "addon/CHANGELOG.md"):
        path = ROOT / name
        assert _headings(path)[0] == version, name
