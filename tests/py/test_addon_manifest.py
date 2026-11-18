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


def test_apparmor_profile_covers_s6_start_chain():
    """0.3.0/0.3.1 regressions: exec without read, then no listing of /etc/services.d (the service
    never started). Broad `file,` like the official profile; host mappings stay denied."""
    import re

    prof = (ROOT / "addon/apparmor.txt").read_text()
    assert re.search(rf"^profile {CFG['slug']} ", prof, re.MULTILINE)
    assert re.search(r"^\s*file,", prof, re.MULTILINE)
    for m in ("config", "homeassistant", "share", "ssl", "media", "backup", "addons"):
        assert f"deny /{m}/** rwklx," in prof
    assert "deny network raw," in prof and "deny network packet," in prof
    # a rule on the `/etc/services.d/**` glob alone cannot list the directory s6 scans
    assert not re.search(r"^\s*/etc/services\.d/\*\* ", prof, re.MULTILINE)


def test_service_logs_listening_on_the_ingress_port():
    run = (ROOT / "addon/rootfs/etc/services.d/editor/run").read_text()
    assert f"--port {CFG['ingress_port']}" in run and "listening" in run
    main = (ROOT / "harmony_addon/__main__.py").read_text()
    assert "listening on" in main and '"0.0.0.0"' in main


def test_run_script_uses_with_contenv_so_supervisor_token_reaches_python():
    """0.3.2 regression: s6-overlay v3 services do not inherit the container environment."""
    run = (ROOT / "addon/rootfs/etc/services.d/editor/run").read_text()
    assert run.startswith("#!/command/with-contenv ")
    assert "s6-setuidgid app python" in run  # setuidgid keeps the environment


def test_grants_match_the_endpoints_the_code_calls():
    src = lambda n: (ROOT / f"harmony_addon/{n}.py").read_text()
    sup, ha, irt = src("supervisor"), src("ha_client"), src("ir_tester")
    # Supervisor API: only /addons/self/* and /discovery, both open to every add-on (no hassio_api)
    assert '"/addons/self/info"' in sup and '"/discovery"' in sup
    assert "/hassio" not in sup and CFG["hassio_api"] is False
    assert CFG["discovery"] == ["ha_harmony"]  # SERVICE announced must be a listed discovery domain
    assert 'SERVICE = "ha_harmony"' in sup
    # Core API through the Supervisor proxy needs homeassistant_api
    assert 'CORE_API_URL = "http://supervisor/core/api"' in ha
    assert CFG["homeassistant_api"] is True
    for forbidden in ("auth_api", "hassio_role", "host_network"):
        assert forbidden not in CFG
    assert "SUPERVISOR_TOKEN" in irt
