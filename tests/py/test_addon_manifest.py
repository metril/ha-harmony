"""The add-on manifest must keep its least-privilege shape."""

from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[2]
CFG = yaml.safe_load((ROOT / "addon/config.yaml").read_text())


def test_least_privilege_manifest():
    assert CFG["slug"] == "ha_harmony_editor"
    assert CFG["ingress"] is True and CFG["panel_admin"] is True
    assert CFG["homeassistant_api"] is True and CFG["hassio_api"] is False
    assert set(CFG["arch"]) == {"aarch64", "amd64"}
    for forbidden in ("ports", "map", "host_network", "full_access", "privileged", "devices"):
        assert forbidden not in CFG
    # discovery needs `discovery:` only; /discovery and /addons/self need no hassio_api (Supervisor docs)
    assert CFG["discovery"] == ["ha_harmony"]
    assert CFG["ingress_port"] == 8099
    assert any("irdb" in p for p in CFG["backup_exclude"])


def test_options_schema_matches_options_reader():
    assert set(CFG["schema"]) == {"log_level"} and set(CFG["options"]) == {"log_level"}
    from harmony_addon.options import LOG_LEVELS

    assert CFG["schema"]["log_level"] == f"list({'|'.join(LOG_LEVELS)})"


def test_base_images_pinned_by_digest():
    build = yaml.safe_load((ROOT / "addon/build.yaml").read_text())["build_from"]
    assert all("@sha256:" in v for v in build.values())
    assert build["amd64"] in (ROOT / "addon/Dockerfile").read_text()


def test_publish_workflow_is_tag_or_manual_only():
    wf = yaml.safe_load((ROOT / ".github/workflows/addon-publish.yml").read_text())
    on = wf.get("on", wf.get(True))
    assert set(on) == {"push", "workflow_dispatch"} and on["push"] == {"tags": ["addon-v*"]}
    assert wf["jobs"]["publish"]["permissions"]["packages"] == "write"


def test_repository_and_build_files():
    assert yaml.safe_load((ROOT / "repository.yaml").read_text())["name"]
    build = yaml.safe_load((ROOT / "addon/build.yaml").read_text())["build_from"]
    assert set(build) == {"aarch64", "amd64"}
    assert (ROOT / "addon/apparmor.txt").read_text().count("profile ha_harmony_editor") == 1
    assert "--require-hashes" in (ROOT / "addon/Dockerfile").read_text()
