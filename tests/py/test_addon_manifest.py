"""The add-on manifest must keep its least-privilege shape."""

from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[2]
CFG = yaml.safe_load((ROOT / "addon/config.yaml").read_text())


def test_least_privilege_manifest():
    assert CFG["slug"] == "ha_harmony_editor"
    assert CFG["ingress"] is True and CFG["panel_admin"] is True
    assert CFG["homeassistant_api"] is False and CFG["hassio_api"] is False
    assert set(CFG["arch"]) == {"aarch64", "amd64"}
    for forbidden in ("ports", "map", "host_network", "full_access", "privileged", "devices"):
        assert forbidden not in CFG
    assert CFG["ingress_port"] == 8099
    assert any("ircache" in p for p in CFG["backup_exclude"])


def test_options_schema_matches_options_reader():
    assert set(CFG["schema"]) == {"hub_host", "hub_port", "hub_token", "log_level"}
    assert CFG["schema"]["hub_token"] == "password?" and CFG["schema"]["hub_port"] == "port"
    from harmony_addon.options import LOG_LEVELS

    assert CFG["schema"]["log_level"] == f"list({'|'.join(LOG_LEVELS)})"


def test_repository_and_build_files():
    assert yaml.safe_load((ROOT / "repository.yaml").read_text())["name"]
    build = yaml.safe_load((ROOT / "addon/build.yaml").read_text())["build_from"]
    assert set(build) == {"aarch64", "amd64"}
    assert (ROOT / "addon/apparmor.txt").read_text().count("profile ha_harmony_editor") == 1
    assert "--require-hashes" in (ROOT / "addon/Dockerfile").read_text()
