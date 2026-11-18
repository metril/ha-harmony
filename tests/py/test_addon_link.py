"""Integration <-> add-on link: hostile bundles, cache, outages, discovery. No hub binary needed."""

import asyncio
import json

import aiohttp
import pytest
from aiohttp import web
from conftest import ADDON_TOKEN, HUB_ID, free_port, make_entry
from homeassistant import config_entries
from homeassistant.config_entries import ConfigEntryState
from homeassistant.data_entry_flow import FlowResultType
from homeassistant.exceptions import HomeAssistantError, ServiceValidationError
from homeassistant.helpers import issue_registry as ir
from homeassistant.helpers.service_info.hassio import HassioServiceInfo

import custom_components.ha_harmony  # noqa: F401  (before the HA plugin shadows it)
from custom_components.ha_harmony import const
from custom_components.ha_harmony.addon_link import (
    AddonAuthError,
    AddonClient,
    AddonUnreachable,
    BundleRejected,
    parse_bundle,
)
from harmony_core import BUNDLE_SCHEMA

pytestmark = pytest.mark.usefixtures("enable_custom_integrations", "fast_client")

SOURCE = {
    "devices": [
        {
            "id": "tv",
            "name": "TV",
            "power": {
                "on": {"proto": "nec", "addr": 4, "cmd": 1},
                "off": {"proto": "nec", "addr": 4, "cmd": 2},
            },
            "commands": {"vol_up": {"proto": "nec", "addr": 4, "cmd": 16}},
        }
    ],
    "activities": [{"id": "watch", "name": "Watch", "devices": [{"device": "tv"}]}],
}
SOURCE2 = {
    **SOURCE,
    "activities": [{"id": "movie", "name": "Movie", "devices": [{"device": "tv"}]}],
}
STORE_KEY = f"ha_harmony.bundle.{HUB_ID}"


def envelope(**over):
    base = {
        "schema": BUNDLE_SCHEMA,
        "core_version": "0.1.0",
        "hub_id": HUB_ID,
        "applied_at": 5,
        "rev": "r1",
        "config": SOURCE,
    }
    return json.dumps({**base, **over}).encode()


# ------------------------------------------------------------------ pure validation
GOOD = envelope()
HOSTILE = {
    "non_json": b"<html>nope</html>",
    "not_object": b"[1, 2]",
    "deep_json": b"[" * 100_000,
    "oversized": b'{"x":"' + b"a" * (2 * 1024 * 1024) + b'"}',
    "wrong_schema": envelope(schema=BUNDLE_SCHEMA + 1),
    "schema_string": envelope(schema="1"),
    "schema_bool": envelope(schema=True),
    "wrong_hub": envelope(hub_id="other-hub"),
    "unknown_envelope_key": envelope(extra=1),
    "missing_key": json.dumps({"schema": BUNDLE_SCHEMA}).encode(),
    "bad_rev": envelope(rev="../../x"),
    "config_not_object": envelope(config=[1]),
    "unknown_config_key": envelope(config={**SOURCE, "evil": {"x": 1}}),
    "bad_device": envelope(config={"devices": [{"id": "tv", "power": {"on": {"proto": "nec"}}}]}),
    "flipper_file_ref": envelope(
        config={
            "devices": [
                {
                    "id": "tv",
                    "name": "TV",
                    "commands": {"p": {"flipper": {"file": "/etc/passwd", "name": "Power"}}},
                }
            ]
        }
    ),
    "smartir_file_ref": envelope(
        config={
            "devices": [
                {"id": "tv", "commands": {"p": {"smartir": {"file": "../x.json", "command": "on"}}}}
            ]
        }
    ),
    "too_many_devices": envelope(config={"devices": [{"id": f"d{i}"} for i in range(40)]}),
}


def test_good_bundle_parses():
    b = parse_bundle(GOOD, HUB_ID)
    assert b.rev == "r1" and b.applied_at == 5 and b.source == SOURCE
    assert list(b.compiled.devices) == ["tv"]


@pytest.mark.parametrize("name", HOSTILE)
def test_hostile_bundles_are_rejected(name):
    with pytest.raises(BundleRejected) as exc:
        parse_bundle(HOSTILE[name], HUB_ID)
    assert len(str(exc.value)) < 400


def test_schema_mismatch_is_flagged_and_keeps_rev():
    with pytest.raises(BundleRejected) as exc:
        parse_bundle(HOSTILE["wrong_schema"], HUB_ID)
    assert exc.value.schema and exc.value.rev == "r1"


# ------------------------------------------------------------------ helpers
def _hashes():
    from custom_components.ha_harmony.addon_link import compile_source

    out = {}
    for src in (SOURCE, SOURCE2):
        c = compile_source(src, HUB_ID)
        out[json.dumps(c.hub_json, sort_keys=True)] = c.config_hash
    return out


HASHES = _hashes()


def hub_handler(sets):
    async def handler(ws, env):
        reply = {"v": 1, "id": env["id"]}
        if env["t"] == "hello":
            await ws.send_json(
                {**reply, "t": "hello_ack", "d": {"hub_id": HUB_ID, "activities": []}}
            )
        elif env["t"] == "set_config":
            sets.append(env["d"]["config"])
            await ws.send_json(
                {
                    **reply,
                    "t": "config_ack",
                    "d": {
                        "ok": True,
                        "config_hash": HASHES[json.dumps(env["d"]["config"], sort_keys=True)],
                    },
                }
            )

    return handler


async def setup(hass, hub, addon_data):
    entry = make_entry("127.0.0.1", hub.port)
    entry.add_to_hass(hass)
    hass.config_entries.async_update_entry(entry, data={**entry.data, **addon_data})
    assert await hass.config_entries.async_setup(entry.entry_id)
    await hass.async_block_till_done()
    return entry


async def until(predicate, timeout=5.0):
    async with asyncio.timeout(timeout):
        while not predicate():
            await asyncio.sleep(0.02)


def preseed(hass_storage, source=SOURCE, rev="cached"):
    hass_storage[STORE_KEY] = {
        "version": const.STORE_VERSION,
        "minor_version": 1,
        "key": STORE_KEY,
        "data": {"rev": rev, "applied_at": 1, "config": source},
    }


@pytest.fixture
async def raw_addon(aiohttp_server):
    """An add-on that answers whatever the test says (status, body, or a slow dribble)."""
    state = {"status": 200, "body": b"", "dribble": False}

    async def handler(request):
        if state["dribble"]:
            resp = web.StreamResponse()
            await resp.prepare(request)
            while True:
                await resp.write(b"{")
                await asyncio.sleep(0.05)
        return web.Response(status=state["status"], body=state["body"])

    app = web.Application()
    app.router.add_get("/api/v1/bundle", handler)
    app.router.add_get("/api/v1/bundle/wait", handler)
    server = await aiohttp_server(app)
    state["data"] = {
        "addon_host": "127.0.0.1",
        "addon_port": server.port,
        "addon_token": ADDON_TOKEN,
    }
    return state


# ------------------------------------------------------------------ hostile bundles end to end
async def test_hostile_bundles_never_touch_cache_hub_or_entities(
    hass, fake_hub, fake_addon, hass_storage, monkeypatch
):
    sets = []
    hub = await fake_hub(hub_handler(sets))
    fake_addon.apply(SOURCE)
    entry = await setup(hass, hub, fake_addon.link_data())
    coord = entry.runtime_data
    assert len(sets) == 1 and hass_storage[STORE_KEY]["data"]["config"] == SOURCE
    before = (coord.compiled, dict(hass_storage[STORE_KEY]["data"]), len(sets), coord.rev)
    monkeypatch.setattr(const, "BUNDLE_MAX_BYTES", 100_000)  # keep the oversize case small

    for name in (
        "non_json",
        "wrong_schema",
        "wrong_hub",
        "unknown_envelope_key",
        "flipper_file_ref",
    ):
        with pytest.raises(BundleRejected):
            await coord.async_process_raw(HOSTILE[name])
    with pytest.raises(BundleRejected):
        await coord.async_process_raw(b"x" * 200_000)
    await hass.async_block_till_done()

    assert (coord.compiled, hass_storage[STORE_KEY]["data"], len(sets), coord.rev) == before
    assert coord.link_state == "rejected" and coord.managed
    await hass.config_entries.async_unload(entry.entry_id)


async def test_oversized_body_is_cut_off_while_streaming(hass, fake_hub, raw_addon, monkeypatch):
    sets = []
    hub = await fake_hub(hub_handler(sets))
    raw_addon["status"], raw_addon["body"] = 200, b"a" * 5000
    monkeypatch.setattr(const, "BUNDLE_MAX_BYTES", 1000)
    entry = await setup(hass, hub, raw_addon["data"])
    with pytest.raises(BundleRejected, match="too large"):
        await entry.runtime_data.async_refresh_from_addon()
    assert sets == [] and not entry.runtime_data.managed
    await hass.config_entries.async_unload(entry.entry_id)


async def test_slow_loris_times_out_without_changing_anything(
    hass, fake_hub, raw_addon, hass_storage, monkeypatch
):
    sets = []
    hub = await fake_hub(hub_handler(sets))
    preseed(hass_storage)
    raw_addon["dribble"] = True
    monkeypatch.setattr(const, "LINK_FETCH_TIMEOUT", 0.3)
    monkeypatch.setattr(const, "LINK_WAIT_TIMEOUT", 0.3)
    entry = await setup(hass, hub, raw_addon["data"])
    coord = entry.runtime_data
    with pytest.raises(AddonUnreachable):
        await coord.async_refresh_from_addon()
    assert coord.rev == "cached" and hass_storage[STORE_KEY]["data"]["rev"] == "cached"
    assert len(sets) == 1  # only the cached config was pushed
    await hass.config_entries.async_unload(entry.entry_id)


async def test_schema_mismatch_raises_a_repair_issue(hass, fake_hub, raw_addon, hass_storage):
    sets = []
    hub = await fake_hub(hub_handler(sets))
    preseed(hass_storage)
    raw_addon["body"] = HOSTILE["wrong_schema"]
    entry = await setup(hass, hub, raw_addon["data"])
    coord = entry.runtime_data
    with pytest.raises(BundleRejected):
        await coord.async_refresh_from_addon()
    issue = ir.async_get(hass).async_get_issue(const.DOMAIN, f"bundle_schema_{HUB_ID}")
    assert issue and "update the add-on" in coord.last_error
    assert coord.rev == "cached" and len(sets) == 1
    raw_addon["body"] = GOOD  # a matching add-on clears it
    await coord.async_refresh_from_addon()
    assert ir.async_get(hass).async_get_issue(const.DOMAIN, f"bundle_schema_{HUB_ID}") is None
    await hass.config_entries.async_unload(entry.entry_id)


# ------------------------------------------------------------------ cache + long-poll
async def test_apply_in_addon_reaches_hub_promptly_and_is_cached_versioned(
    hass, fake_hub, fake_addon, hass_storage
):
    sets = []
    hub = await fake_hub(hub_handler(sets))
    fake_addon.apply(SOURCE)
    entry = await setup(hass, hub, fake_addon.link_data())
    coord = entry.runtime_data
    assert coord.link_state == "ok" and len(sets) == 1
    fake_addon.apply(SOURCE2)  # no polling: the long-poll wakes
    await until(lambda: len(sets) == 2)
    assert coord.compiled.activities == ["Movie"]
    saved = hass_storage[STORE_KEY]
    assert saved["version"] == const.STORE_VERSION and saved["data"]["config"] == SOURCE2
    assert saved["data"]["rev"] == coord.rev
    await hass.config_entries.async_unload(entry.entry_id)


async def test_addon_down_at_start_runs_on_cache_with_one_log_line(
    hass, fake_hub, hass_storage, monkeypatch, caplog
):
    sets = []
    hub = await fake_hub(hub_handler(sets))
    preseed(hass_storage)
    monkeypatch.setattr(const, "LINK_BACKOFF_MIN", 0.05)
    monkeypatch.setattr(const, "LINK_BACKOFF_MAX", 0.1)
    data = {"addon_host": "127.0.0.1", "addon_port": free_port(), "addon_token": ADDON_TOKEN}
    entry = await setup(hass, hub, data)
    coord = entry.runtime_data
    await until(lambda: coord.link_state == "unreachable")
    await asyncio.sleep(0.5)  # many retries
    assert coord.managed and coord.compiled.activities == ["Watch"] and len(sets) == 1
    assert caplog.text.count("Harmony Editor add-on:") == 1  # not one line per retry
    state = hass.states.get("sensor.harmony_hub_sim_hub_add_on_link")
    assert state.state == "unreachable" and state.attributes["rev"] == "cached"
    await hass.config_entries.async_unload(entry.entry_id)


async def test_wrong_token_is_auth_failed(hass, fake_hub, fake_addon, hass_storage, caplog):
    hub = await fake_hub(hub_handler([]))
    preseed(hass_storage)
    entry = await setup(hass, hub, {**fake_addon.link_data(), "addon_token": "0" * 64})
    await until(lambda: entry.runtime_data.link_state == "auth_failed")
    assert entry.runtime_data.managed and "0" * 64 not in caplog.text
    await hass.config_entries.async_unload(entry.entry_id)


async def test_corrupt_cache_is_ignored(hass, fake_hub, hass_storage):
    preseed(hass_storage, source={"devices": [{"id": "x", "bogus": 1}]})
    hub = await fake_hub(hub_handler([]))
    entry = make_entry("127.0.0.1", hub.port)
    entry.add_to_hass(hass)
    assert await hass.config_entries.async_setup(entry.entry_id)
    assert not entry.runtime_data.managed and entry.state is ConfigEntryState.LOADED
    await hass.config_entries.async_unload(entry.entry_id)


async def test_client_maps_statuses(aiohttp_server):
    async def h401(request):
        return web.Response(status=401)

    app = web.Application()
    app.router.add_get("/api/v1/bundle", h401)
    server = await aiohttp_server(app)
    async with aiohttp.ClientSession() as s:
        with pytest.raises(AddonAuthError):
            await AddonClient(s, "127.0.0.1", server.port, "t").fetch(HUB_ID)
        with pytest.raises(AddonUnreachable):
            await AddonClient(s, "127.0.0.1", free_port(), "t").fetch(HUB_ID)


# ------------------------------------------------------------------ YAML removal
async def test_leftover_yaml_raises_one_issue_and_is_left_alone(hass, fake_hub, tmp_path):
    hass.config.config_dir = str(tmp_path / "cfg")
    f = tmp_path / "cfg" / "ha_harmony" / f"{HUB_ID}.yaml"
    f.parent.mkdir(parents=True)
    f.write_text("devices: [not even yaml", encoding="utf-8")  # never parsed
    hub = await fake_hub(hub_handler([]))
    entry = make_entry("127.0.0.1", hub.port)
    entry.add_to_hass(hass)
    assert await hass.config_entries.async_setup(entry.entry_id)
    issue = ir.async_get(hass).async_get_issue(const.DOMAIN, f"yaml_unused_{HUB_ID}")
    assert issue and issue.translation_placeholders["path"] == str(f)
    assert f.read_text(encoding="utf-8") == "devices: [not even yaml"
    f.unlink()
    await hass.config_entries.async_reload(entry.entry_id)
    assert ir.async_get(hass).async_get_issue(const.DOMAIN, f"yaml_unused_{HUB_ID}") is None
    await hass.config_entries.async_unload(entry.entry_id)


async def test_reload_service_needs_a_link_and_send_ir_refuses_files(hass, fake_hub):
    hub = await fake_hub(hub_handler([]))
    entry = make_entry("127.0.0.1", hub.port)
    entry.add_to_hass(hass)
    assert await hass.config_entries.async_setup(entry.entry_id)
    with pytest.raises(HomeAssistantError, match="not linked"):
        await hass.services.async_call(const.DOMAIN, "reload_config", {}, blocking=True)
    with pytest.raises(ServiceValidationError, match="Invalid IR action"):
        await hass.services.async_call(
            const.DOMAIN,
            "send_ir",
            {"ir": {"flipper": {"file": "x.ir", "name": "Power"}}},
            blocking=True,
        )
    await hass.config_entries.async_unload(entry.entry_id)


# ------------------------------------------------------------------ Supervisor discovery flow
def info(slug="ha_harmony_editor", **cfg):
    return HassioServiceInfo(config=cfg, name="Harmony Editor", slug=slug, uuid="u1")


async def hassio_flow(hass, slug="ha_harmony_editor", **cfg):
    return await hass.config_entries.flow.async_init(
        const.DOMAIN, context={"source": config_entries.SOURCE_HASSIO}, data=info(slug, **cfg)
    )


async def test_discovery_without_a_hub_aborts(hass, fake_addon):
    res = await hassio_flow(hass, host="127.0.0.1", port=fake_addon.port, token="t")
    assert res["type"] is FlowResultType.ABORT and res["reason"] == "no_hub"


async def test_discovery_links_the_existing_entry_not_a_second_one(hass, fake_addon):
    entry = make_entry("127.0.0.1", 1)
    entry.add_to_hass(hass)
    cfg = {"host": "127.0.0.1", "port": fake_addon.port, "token": ADDON_TOKEN}
    res = await hassio_flow(hass, **cfg)
    assert res["type"] is FlowResultType.FORM and res["step_id"] == "hassio_confirm"
    res = await hass.config_entries.flow.async_configure(res["flow_id"], {})
    assert res["type"] is FlowResultType.ABORT and res["reason"] == "addon_linked"
    assert len(hass.config_entries.async_entries(const.DOMAIN)) == 1
    assert entry.data["addon_host"] == "127.0.0.1" and entry.data["addon_token"] == ADDON_TOKEN
    assert entry.data["token"] == "test-token" and entry.data["port"] == 1  # hub data intact


async def test_rediscovery_with_new_token_updates_without_asking(hass, fake_addon):
    entry = make_entry("127.0.0.1", 1, addon=fake_addon)
    entry.add_to_hass(hass)
    hass.config_entries.async_update_entry(
        entry, data={**entry.data, "addon_slug": "ha_harmony_editor"}
    )
    res = await hassio_flow(hass, host="newhost", port=fake_addon.port, token="f" * 64)
    assert res["type"] is FlowResultType.ABORT and res["reason"] == "addon_updated"
    assert entry.data["addon_host"] == "newhost" and entry.data["addon_token"] == "f" * 64
    assert len(hass.config_entries.async_entries(const.DOMAIN)) == 1


@pytest.mark.parametrize(
    ("override", "reason"),
    [({"token": "bad"}, "invalid_auth"), ({"port": 1}, "cannot_connect")],
)
async def test_discovery_confirm_checks_the_link(hass, fake_addon, override, reason):
    make_entry("127.0.0.1", 1).add_to_hass(hass)
    cfg = {"host": "127.0.0.1", "port": fake_addon.port, "token": ADDON_TOKEN, **override}
    res = await hassio_flow(hass, **cfg)
    res = await hass.config_entries.flow.async_configure(res["flow_id"], {})
    assert res["type"] is FlowResultType.ABORT and res["reason"] == reason


async def test_discovery_with_garbage_config_aborts(hass):
    make_entry("127.0.0.1", 1).add_to_hass(hass)
    res = await hassio_flow(hass, host="h", port="not-a-port", token="t")
    assert res["type"] is FlowResultType.ABORT and res["reason"] == "invalid_discovery"


@pytest.mark.parametrize("slug", ["evil_addon", "ha_harmony_editor_x", "xha_harmony_editor", ""])
async def test_discovery_from_a_foreign_addon_is_rejected(hass, fake_addon, slug):
    entry = make_entry("127.0.0.1", 1)
    entry.add_to_hass(hass)
    cfg = {"host": "evil", "port": fake_addon.port, "token": "t"}
    res = await hassio_flow(hass, slug, **cfg)
    assert res["type"] is FlowResultType.ABORT and res["reason"] == "not_our_addon"
    assert "addon_host" not in entry.data


async def test_repository_prefixed_slug_is_ours_and_stored(hass, fake_addon):
    entry = make_entry("127.0.0.1", 1)
    entry.add_to_hass(hass)
    cfg = {"host": "127.0.0.1", "port": fake_addon.port, "token": ADDON_TOKEN}
    res = await hassio_flow(hass, "a1b2c3d4_ha_harmony_editor", **cfg)
    res = await hass.config_entries.flow.async_configure(res["flow_id"], {})
    assert res["reason"] == "addon_linked"
    assert entry.data["addon_slug"] == "a1b2c3d4_ha_harmony_editor"
    assert entry.data["addon_uuid"] == "u1"


async def test_changed_slug_needs_confirm_and_never_updates_silently(hass, fake_addon):
    entry = make_entry("127.0.0.1", 1, addon=fake_addon)
    entry.add_to_hass(hass)
    hass.config_entries.async_update_entry(
        entry, data={**entry.data, "addon_slug": "ha_harmony_editor"}
    )
    res = await hassio_flow(hass, "a1b2c3d4_ha_harmony_editor", host="newhost", port=1, token="f")
    assert res["type"] is FlowResultType.FORM and res["step_id"] == "hassio_confirm"
    assert entry.data["addon_host"] == "127.0.0.1" and entry.data["addon_token"] == ADDON_TOKEN


async def test_legacy_link_without_slug_needs_confirm(hass, fake_addon):
    make_entry("127.0.0.1", 1, addon=fake_addon).add_to_hass(hass)
    res = await hassio_flow(hass, host="newhost", port=1, token="f")
    assert res["type"] is FlowResultType.FORM and res["step_id"] == "hassio_confirm"


async def test_ignored_discovery_is_honoured_and_never_probed(hass, fake_addon):
    from pytest_homeassistant_custom_component.common import MockConfigEntry

    entry = make_entry("127.0.0.1", 1)
    entry.add_to_hass(hass)
    MockConfigEntry(
        domain=const.DOMAIN,
        unique_id="hassio_ha_harmony_editor",
        source=config_entries.SOURCE_IGNORE,
        data={},
    ).add_to_hass(hass)
    cfg = {"host": "127.0.0.1", "port": fake_addon.port, "token": ADDON_TOKEN}
    res = await hassio_flow(hass, **cfg)
    assert res["type"] is FlowResultType.ABORT and res["reason"] == "already_configured"
    assert "addon_host" not in entry.data


async def test_ignored_hub_entry_never_gets_the_token(hass, fake_addon):
    from pytest_homeassistant_custom_component.common import MockConfigEntry

    MockConfigEntry(
        domain=const.DOMAIN, unique_id="ha-harmony-x", source=config_entries.SOURCE_IGNORE, data={}
    ).add_to_hass(hass)
    cfg = {"host": "127.0.0.1", "port": fake_addon.port, "token": ADDON_TOKEN}
    res = await hassio_flow(hass, **cfg)
    assert res["type"] is FlowResultType.ABORT and res["reason"] == "no_hub"


async def test_429_is_backoff_not_an_auth_failure(hass, fake_hub, raw_addon, hass_storage):
    hub = await fake_hub(hub_handler([]))
    preseed(hass_storage)
    raw_addon["status"] = 429
    entry = await setup(hass, hub, raw_addon["data"])
    coord = entry.runtime_data
    with pytest.raises(AddonUnreachable):
        await coord.async_refresh_from_addon()
    assert coord.link_state == "unreachable"
    raw_addon["status"] = 401
    with pytest.raises(AddonAuthError):
        await coord._link.fetch(HUB_ID)
    await hass.config_entries.async_unload(entry.entry_id)


# ------------------------------------------------------------------ apply acknowledgements
def ack_of(fake_addon):
    return fake_addon.store.load(HUB_ID)["ack"]


async def test_successful_push_is_acked_ok(hass, fake_hub, fake_addon):
    hub = await fake_hub(hub_handler([]))
    fake_addon.apply(SOURCE)
    entry = await setup(hass, hub, fake_addon.link_data())
    await hass.async_block_till_done()
    ack = ack_of(fake_addon)
    assert ack["result"] == "ok" and ack["rev"] == entry.runtime_data.rev
    await hass.config_entries.async_unload(entry.entry_id)


async def test_invalid_config_is_acked_as_error_with_a_short_reason(hass, fake_hub, fake_addon):
    hub = await fake_hub(hub_handler([]))
    fake_addon.apply({"devices": [{"id": "BAD ID"}]})
    entry = await setup(hass, hub, fake_addon.link_data())
    await hass.async_block_till_done()
    ack = ack_of(fake_addon)
    assert ack["result"] == "error" and ack["detail"].startswith("invalid config")
    assert len(ack["detail"]) <= 200
    await hass.config_entries.async_unload(entry.entry_id)


async def test_hub_rejection_is_acked_as_error(hass, fake_hub, fake_addon):
    async def reject(ws, env):
        reply = {"v": 1, "id": env["id"]}
        if env["t"] == "hello":
            await ws.send_json(
                {**reply, "t": "hello_ack", "d": {"hub_id": HUB_ID, "activities": []}}
            )
        elif env["t"] == "set_config":
            await ws.send_json(
                {**reply, "t": "config_ack", "d": {"ok": False, "message": "too big"}}
            )

    hub = await fake_hub(reject)
    fake_addon.apply(SOURCE)
    entry = await setup(hass, hub, fake_addon.link_data())
    await hass.async_block_till_done()
    ack = ack_of(fake_addon)
    assert ack["result"] == "error" and "too big" in ack["detail"]
    await hass.config_entries.async_unload(entry.entry_id)


async def test_schema_mismatch_is_acked_as_error(hass, fake_hub, fake_addon, monkeypatch):
    hub = await fake_hub(hub_handler([]))
    fake_addon.apply(SOURCE)
    monkeypatch.setattr("custom_components.ha_harmony.addon_link.BUNDLE_SCHEMA", BUNDLE_SCHEMA + 1)
    entry = await setup(hass, hub, fake_addon.link_data())
    await hass.async_block_till_done()
    ack = ack_of(fake_addon)
    assert ack["result"] == "error" and ack["detail"] == "schema not supported"
    await hass.config_entries.async_unload(entry.entry_id)


async def test_ack_failure_is_not_fatal(hass, fake_hub, raw_addon, hass_storage):
    sets = []
    hub = await fake_hub(hub_handler(sets))
    preseed(hass_storage)
    raw_addon["status"] = 404  # POST is not even routed in the raw add-on: the ack fails
    entry = await setup(hass, hub, raw_addon["data"])
    await hass.async_block_till_done()
    assert len(sets) == 1 and entry.state is ConfigEntryState.LOADED
    assert entry.runtime_data._acked is None  # will be retried on the next sync
    await hass.config_entries.async_unload(entry.entry_id)


async def test_ack_client_sends_only_bounded_fields(aiohttp_server):
    seen = {}

    async def handler(request):
        seen["body"] = await request.json()
        seen["auth"] = request.headers["Authorization"]
        return web.json_response({"ok": True})

    app = web.Application()
    app.router.add_post("/api/v1/bundle/ack", handler)
    srv = await aiohttp_server(app)
    async with aiohttp.ClientSession() as s:
        c = AddonClient(s, "127.0.0.1", srv.port, "tok")
        await c.ack("h", "r1", "error", "x" * 500)
    assert seen["auth"] == "Bearer tok"
    assert (
        set(seen["body"]) == {"hub", "rev", "result", "detail"}
        and len(seen["body"]["detail"]) == 200
    )


async def test_send_ir_selects_the_hub_by_its_id_as_the_add_on_sends_it(hass, fake_hub):
    hub = await fake_hub(hub_handler([]))
    entry = make_entry("127.0.0.1", hub.port)
    entry.add_to_hass(hass)
    assert await hass.config_entries.async_setup(entry.entry_id)
    bad = {"proto": "nec", "addr": 999, "cmd": 1}
    with pytest.raises(ServiceValidationError, match="Invalid IR action"):  # entry was found
        await hass.services.async_call(
            const.DOMAIN, "send_ir", {"config_entry": HUB_ID, "ir": bad}, blocking=True
        )
    with pytest.raises(ServiceValidationError, match="No loaded ha_harmony entry"):
        await hass.services.async_call(
            const.DOMAIN, "send_ir", {"config_entry": "other-hub", "ir": bad}, blocking=True
        )
    await hass.config_entries.async_unload(entry.entry_id)


async def test_first_link_confirm_shows_what_is_linked(hass, fake_addon):
    entry = make_entry("127.0.0.1", 1)
    entry.add_to_hass(hass)
    res = await hassio_flow(hass, host="addon-host", port=8199, token="t")
    assert res["type"] is FlowResultType.FORM and res["step_id"] == "hassio_confirm"
    assert res["description_placeholders"] == {"slug": "ha_harmony_editor", "host": "addon-host:8199"}
