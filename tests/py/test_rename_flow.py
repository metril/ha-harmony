"""Renaming a hub from Home Assistant: the Reconfigure flow against the real `hub --sim` daemon and the real add-on app.

Everything keyed by the hub id moves in one transaction (entry, devices, entity unique_ids, cached config,
add-on store); a failure at any point leaves everything, hub included, as it was."""

import asyncio

import aiohttp
import pytest
from conftest import HUB_ID, TOKEN, make_entry
from homeassistant import config_entries
from homeassistant.config_entries import ConfigEntryState
from homeassistant.data_entry_flow import FlowResultType
from homeassistant.helpers import device_registry as dr
from homeassistant.helpers import entity_registry as er
from homeassistant.helpers.service_info.zeroconf import ZeroconfServiceInfo
from homeassistant.helpers.storage import Store
from test_ha_runtime import YAML

import custom_components.ha_harmony  # noqa: F401  (before the HA plugin shadows it)
from custom_components.ha_harmony import const, rename
from custom_components.ha_harmony.const import DOMAIN

pytestmark = pytest.mark.usefixtures("enable_custom_integrations", "fast_client")

HW = "aabbccddeeff"
NEW = "living-room"


@pytest.fixture
def hub(start_hub, tmp_path):
    d = tmp_path / "ident"
    d.mkdir()
    return start_hub("--ident-dir", str(d), "--hw-id", HW)


async def hub_says(hub_):
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_.url) as ws:
        await ws.send_json({"v": 1, "id": 1, "t": "hello", "d": {"token": TOKEN}})
        return (await ws.receive_json())["d"]


async def settle(hass, entry, *, link="ok"):
    """After a reload: the hub sync done and the add-on link answered, so states are comparable."""
    async with asyncio.timeout(10):
        while (
            entry.state is not ConfigEntryState.LOADED
            or entry.runtime_data.link_state != link
            or not set(entry.runtime_data.compiled.devices) <= set(entry.runtime_data.device_states)
        ):
            await asyncio.sleep(0.02)
            await hass.async_block_till_done()
        if entry.runtime_data.first_sync is not None:
            await entry.runtime_data.first_sync
    await hass.async_block_till_done()


async def raw_rename(hub_, new_id):
    """What a hub.conf edit does to the hub, as far as clients can tell: it answers under another id."""
    async with aiohttp.ClientSession() as s, s.ws_connect(hub_.url) as ws:
        await ws.send_json({"v": 1, "id": 1, "t": "hello", "d": {"token": TOKEN}})
        await ws.receive_json()
        await ws.send_json({"v": 1, "id": 2, "t": "set_hub_id", "d": {"id": new_id}})
        while (msg := await ws.receive_json())["t"] != "hub_id_ack":
            assert msg["t"] != "error", msg


async def set_up(hass, hub_, fake_addon):
    fake_addon.apply_yaml(YAML)
    entry = make_entry("127.0.0.1", hub_.port, addon=fake_addon)
    entry.add_to_hass(hass)
    assert await hass.config_entries.async_setup(entry.entry_id)
    if entry.runtime_data.first_sync is not None:
        await entry.runtime_data.first_sync
    await hass.async_block_till_done()
    assert entry.data[const.CONF_HW_ID] == HW and entry.data[const.CONF_CAN_RENAME] is True
    await settle(hass, entry)
    return entry


def snapshot(hass, entry):
    ents = er.async_entries_for_config_entry(er.async_get(hass), entry.entry_id)
    devs = dr.async_entries_for_config_entry(dr.async_get(hass), entry.entry_id)
    return {
        "entities": {e.entity_id: e.unique_id for e in ents},
        "devices": {d.id: frozenset(i for _, i in d.identifiers) for d in devs},
        "states": {
            e.entity_id: hass.states.get(e.entity_id).state
            for e in ents
            if hass.states.get(e.entity_id)
        },
        "unique_id": entry.unique_id,
        "title": entry.title,
    }


async def reconfigure(hass, entry, hub_id=NEW, name="Living room"):
    res = await entry.start_reconfigure_flow(hass)
    assert res["type"] is FlowResultType.FORM and res["step_id"] == "reconfigure"
    fields = {str(k) for k in res["data_schema"].schema}
    assert {"hub_id", "name"} <= fields
    data = {
        "host": entry.data["host"],
        "port": entry.data["port"],
        "token": TOKEN,
        "hub_id": hub_id,
        "name": name,
    }
    res = await hass.config_entries.flow.async_configure(res["flow_id"], data)
    await hass.async_block_till_done()
    return res


async def test_rename_moves_everything_and_keeps_entity_ids(hass, hub, fake_addon):
    entry = await set_up(hass, hub, fake_addon)
    before = snapshot(hass, entry)
    assert len(before["entities"]) > 5 and len(before["devices"]) >= 2
    assert all(u.startswith(f"{HUB_ID}_") for u in before["entities"].values())
    assert fake_addon.store.hubs() == [HUB_ID]
    assert (
        await Store(hass, const.STORE_VERSION, f"{DOMAIN}.bundle.{HUB_ID}").async_load() is not None
    )

    res = await reconfigure(hass, entry)
    assert res["type"] is FlowResultType.ABORT and res["reason"] == "reconfigure_successful"
    await settle(hass, entry)
    after = snapshot(hass, entry)

    assert (entry.unique_id, entry.title) == (NEW, "Living room")
    assert entry.data[const.CONF_HUB_NAME] == "Living room" and entry.runtime_data.hub_id == NEW
    # entity ids, history and customisations hang on the entity_id: unchanged. Only the unique_ids moved.
    assert set(after["entities"]) == set(before["entities"])
    assert after["entities"] == {
        e: u.replace(f"{HUB_ID}_", f"{NEW}_", 1) for e, u in before["entities"].items()
    }
    # the same devices (same registry ids: areas, names, via_device), new identifiers, none duplicated
    assert set(after["devices"]) == set(before["devices"])
    assert all(next(iter(i)).startswith(NEW) for i in after["devices"].values())
    # states are restored after the reload
    assert after["states"] == before["states"]
    # the hub, the cached config and the add-on follow
    ack = await hub_says(hub)
    assert (ack["hub_id"], ack["name"]) == (NEW, "Living room")
    assert await Store(hass, const.STORE_VERSION, f"{DOMAIN}.bundle.{HUB_ID}").async_load() is None
    assert await Store(hass, const.STORE_VERSION, f"{DOMAIN}.bundle.{NEW}").async_load() is not None
    assert fake_addon.store.hubs() == [NEW]
    assert [h["id"] for h in fake_addon.linked_entries()] == [NEW]
    assert fake_addon.linked_entries()[0]["name"] == "Living room"
    assert const.CONF_PREV_ID not in entry.data  # the add-on has seen the move


async def test_name_only_change_keeps_the_id(hass, hub, fake_addon):
    entry = await set_up(hass, hub, fake_addon)
    before = snapshot(hass, entry)
    res = await reconfigure(hass, entry, hub_id=HUB_ID, name="Den")
    assert res["reason"] == "reconfigure_successful"
    after = snapshot(hass, entry)
    assert (
        after["entities"] == before["entities"]
        and entry.unique_id == HUB_ID
        and entry.title == "Den"
    )
    assert (await hub_says(hub))["name"] == "Den"
    assert fake_addon.store.hubs() == [HUB_ID]
    device = dr.async_get(hass).async_get_device_by_identifier((DOMAIN, HUB_ID), entry.entry_id)
    assert device is not None and device.name == "Den"


async def test_unchanged_form_just_updates_the_connection(hass, hub, fake_addon):
    entry = await set_up(hass, hub, fake_addon)
    res = await reconfigure(hass, entry, hub_id=HUB_ID, name="")
    assert res["reason"] == "reconfigure_successful" and entry.unique_id == HUB_ID
    assert "name" not in await hub_says(hub)


@pytest.mark.parametrize(
    ("hub_id", "name", "key"),
    [
        ("default", "", "invalid_hub_id"),
        ("has space", "", "invalid_hub_id"),
        ("-lead", "", "invalid_hub_id"),
        (NEW, "x" * 49, "invalid_hub_name"),
        (NEW, "a\tb", "invalid_hub_name"),
    ],
)
async def test_invalid_values_are_refused_before_anything_happens(
    hass, hub, fake_addon, hub_id, name, key
):
    entry = await set_up(hass, hub, fake_addon)
    before = snapshot(hass, entry)
    res = await reconfigure(hass, entry, hub_id, name)
    assert res["type"] is FlowResultType.FORM and res["errors"] == {"base": key}
    assert snapshot(hass, entry) == before and entry.state is ConfigEntryState.LOADED
    assert (await hub_says(hub))["hub_id"] == HUB_ID


async def test_id_used_by_another_entry_is_refused(hass, hub, fake_addon):
    entry = await set_up(hass, hub, fake_addon)
    other = make_entry("10.9.9.8", 1)
    other.add_to_hass(hass)
    hass.config_entries.async_update_entry(other, unique_id="taken")
    res = await reconfigure(hass, entry, "taken", "")
    assert res["errors"] == {"base": "id_in_use"}
    assert (await hub_says(hub))["hub_id"] == HUB_ID


async def test_pinned_hub_is_refused_with_a_clear_error(hass, start_hub, tmp_path, fake_addon):
    d = tmp_path / "pin"
    d.mkdir()
    pinned = start_hub("--ident-dir", str(d), "--hub-id", HUB_ID, "--hw-id", HW)
    entry = await set_up(hass, pinned, fake_addon)
    assert entry.data[const.CONF_ID_PINNED] is True
    before = snapshot(hass, entry)
    res = await entry.start_reconfigure_flow(hass)
    assert "hub_id" not in {
        str(k) for k in res["data_schema"].schema
    }  # the id field is not offered
    assert "hub.conf" in res["description_placeholders"]["note"]
    hass.config_entries.flow.async_abort(res["flow_id"])
    # a stale form (the entry did not know about the pin yet) still gets the hub's own refusal; nothing changes
    hass.config_entries.async_update_entry(entry, data={**entry.data, const.CONF_ID_PINNED: False})
    res = await reconfigure(hass, entry)
    assert (
        res["errors"] == {"base": "id_pinned"}
        and "hub.conf" in res["description_placeholders"]["detail"]
    )
    await settle(hass, entry)
    assert snapshot(hass, entry) == before and entry.state is ConfigEntryState.LOADED
    assert (await hub_says(pinned))["hub_id"] == HUB_ID
    hass.config_entries.async_update_entry(entry, data={**entry.data, const.CONF_ID_PINNED: True})
    # the name can still be set on a pinned hub
    res = await reconfigure_name_only(hass, entry, pinned, "Pinned one")
    assert (
        res["reason"] == "reconfigure_successful"
        and (await hub_says(pinned))["name"] == "Pinned one"
    )


async def reconfigure_name_only(hass, entry, hub_, name):
    res = await entry.start_reconfigure_flow(hass)
    data = {"host": "127.0.0.1", "port": hub_.port, "token": TOKEN, "name": name}
    res = await hass.config_entries.flow.async_configure(res["flow_id"], data)
    await hass.async_block_till_done()
    return res


# -- rollback at every failure point ------------------------------------------------------------------
async def _boom(*_a, **_k):
    raise RuntimeError("injected failure")


@pytest.mark.parametrize(
    "step",
    ["_copy_bundle_store", "_move_devices", "_move_entities", "_update_entry", "_addon_move"],
)
async def test_failure_at_each_step_rolls_everything_back(hass, hub, fake_addon, monkeypatch, step):
    entry = await set_up(hass, hub, fake_addon)
    before = snapshot(hass, entry)
    data_before = dict(entry.data)
    monkeypatch.setattr(rename, step, _boom)
    res = await reconfigure(hass, entry)
    assert res["type"] is FlowResultType.FORM and res["errors"] == {"base": "migration_failed"}
    await settle(hass, entry)
    assert snapshot(hass, entry) == before  # entry, devices, unique_ids, states: as they were
    assert dict(entry.data) == data_before and entry.state is ConfigEntryState.LOADED
    ack = await hub_says(hub)  # the hub was set back too
    assert ack["hub_id"] == HUB_ID and "name" not in ack
    assert await Store(hass, const.STORE_VERSION, f"{DOMAIN}.bundle.{NEW}").async_load() is None
    assert (
        await Store(hass, const.STORE_VERSION, f"{DOMAIN}.bundle.{HUB_ID}").async_load() is not None
    )
    assert fake_addon.store.hubs() == [HUB_ID]
    assert entry.runtime_data.hub_id == HUB_ID  # running again under the old id


async def test_entity_registry_collision_part_way_is_undone(hass, hub, fake_addon):
    entry = await set_up(hass, hub, fake_addon)
    ent_reg = er.async_get(hass)
    victim = max(snapshot(hass, entry)["entities"].values())
    # another registry entry already owns what one of the entities would become
    ent_reg.async_get_or_create(
        "sensor", DOMAIN, victim.replace(HUB_ID, NEW, 1), suggested_object_id="squatter"
    )
    before = snapshot(hass, entry)
    res = await reconfigure(hass, entry)
    assert res["errors"] == {"base": "migration_failed"}
    await settle(hass, entry)
    assert snapshot(hass, entry) == before
    assert (await hub_says(hub))["hub_id"] == HUB_ID and fake_addon.store.hubs() == [HUB_ID]


async def test_hub_back_reconnects_when_the_link_is_gone(hass, hub, fake_addon, monkeypatch):
    """The failure comes with a dead connection: the old id is set again over a fresh one."""
    entry = await set_up(hass, hub, fake_addon)
    monkeypatch.setattr(rename, "_move_entities", _boom)
    orig_back = rename._hub_back
    calls = []

    async def spy(hass_, entry_, client, *a):
        await client.async_close()  # the flow's connection is gone when the rollback starts
        calls.append(1)
        return await orig_back(hass_, entry_, client, *a)

    monkeypatch.setattr(rename, "_hub_back", spy)
    res = await reconfigure(hass, entry)
    assert res["errors"] == {"base": "migration_failed"} and calls
    assert (await hub_says(hub))["hub_id"] == HUB_ID


async def test_unreachable_addon_does_not_block_the_rename_and_catches_up(hass, hub, fake_addon):
    entry = await set_up(hass, hub, fake_addon)
    # the add-on's address is wrong during the rename; the store moves at the next poll, from the headers
    good = {k: entry.data[k] for k in (const.CONF_ADDON_HOST, const.CONF_ADDON_PORT)}
    hass.config_entries.async_update_entry(entry, data={**entry.data, const.CONF_ADDON_PORT: 1})
    res = await reconfigure(hass, entry)
    assert res["reason"] == "reconfigure_successful" and entry.unique_id == NEW
    assert entry.data[const.CONF_PREV_ID] == HUB_ID and fake_addon.store.hubs() == [HUB_ID]
    hass.config_entries.async_update_entry(
        entry, data={**entry.data, const.CONF_ADDON_PORT: good[const.CONF_ADDON_PORT]}
    )
    await hass.config_entries.async_reload(entry.entry_id)
    await settle(hass, entry)
    async with asyncio.timeout(10):  # the first poll carries the previous id
        while fake_addon.store.hubs() != [NEW] or const.CONF_PREV_ID in entry.data:
            await asyncio.sleep(0.02)
            await hass.async_block_till_done()


async def test_addon_already_holding_the_new_id_refuses_and_rolls_back(hass, hub, fake_addon):
    entry = await set_up(hass, hub, fake_addon)
    fake_addon.apply_yaml(YAML, NEW)  # a config already stored under the id we want
    before = snapshot(hass, entry)
    res = await reconfigure(hass, entry)
    assert res["errors"] == {"base": "addon_store_exists"}
    await settle(hass, entry)
    assert snapshot(hass, entry) == before
    assert (await hub_says(hub))["hub_id"] == HUB_ID
    assert sorted(fake_addon.store.hubs()) == sorted([HUB_ID, NEW])  # nothing was overwritten


# -- the renamed hub is the same hub --------------------------------------------------------------------
async def test_setup_follows_a_hub_renamed_on_the_device(hass, hub, fake_addon):
    """hub.conf edit + restart: the hub answers under another id; its hardware id matches the entry."""
    entry = await set_up(hass, hub, fake_addon)
    before = snapshot(hass, entry)
    await hass.config_entries.async_unload(entry.entry_id)
    await raw_rename(hub, "from-hub-conf")
    assert await hass.config_entries.async_setup(entry.entry_id)
    await hass.async_block_till_done()
    assert entry.state is ConfigEntryState.LOADED and entry.unique_id == "from-hub-conf"
    after = snapshot(hass, entry)
    assert set(after["entities"]) == set(before["entities"]) and set(after["devices"]) == set(
        before["devices"]
    )
    assert all(u.startswith("from-hub-conf_") for u in after["entities"].values())
    assert fake_addon.store.hubs() == ["from-hub-conf"]


async def test_a_different_hub_on_the_same_host_is_still_refused(hass, hub, fake_addon):
    entry = await set_up(hass, hub, fake_addon)
    await hass.config_entries.async_unload(entry.entry_id)
    hass.config_entries.async_update_entry(
        entry, data={**entry.data, const.CONF_HW_ID: "ffffffffffff"}
    )
    await raw_rename(hub, "other-hub")
    assert not await hass.config_entries.async_setup(entry.entry_id)
    assert entry.state is ConfigEntryState.SETUP_ERROR and entry.unique_id == HUB_ID


def _zc(hub_id, hw=None, host="10.1.2.3", port=8099):
    props = {"hub_id": hub_id, **({"hw": hw} if hw else {})}
    return ZeroconfServiceInfo(
        ip_address=host,
        ip_addresses=[host],
        hostname="hub.local.",
        name=f"{hub_id}._haharmony._tcp.local.",
        port=port,
        properties=props,
        type="_haharmony._tcp.local.",
    )


async def test_discovery_recognises_the_renamed_hub_at_the_same_address(hass):
    entry = make_entry("10.1.2.3", 8099)
    entry.add_to_hass(hass)
    hass.config_entries.async_update_entry(entry, data={**entry.data, const.CONF_HW_ID: HW})
    res = await hass.config_entries.flow.async_init(
        DOMAIN, context={"source": config_entries.SOURCE_ZEROCONF}, data=_zc("brand-new-id", HW)
    )
    assert res["type"] is FlowResultType.ABORT and res["reason"] == "already_configured"
    # no second entry is offered
    assert len(hass.config_entries.async_entries(DOMAIN)) == 1


async def test_discovery_of_a_renamed_hub_at_a_new_address_asks_before_moving(hass, monkeypatch):
    from custom_components.ha_harmony import config_flow

    probes = []

    async def probe(hass_, host, port, token):
        probes.append(host)
        return "brand-new-id"

    monkeypatch.setattr(config_flow, "_probe", probe)
    entry = make_entry("10.9.9.9", 8099)
    entry.add_to_hass(hass)
    hass.config_entries.async_update_entry(entry, data={**entry.data, const.CONF_HW_ID: HW})
    res = await hass.config_entries.flow.async_init(
        DOMAIN, context={"source": config_entries.SOURCE_ZEROCONF}, data=_zc("brand-new-id", HW)
    )
    assert res["type"] is FlowResultType.FORM and res["step_id"] == "hub_moved" and probes == []
    assert res["description_placeholders"]["name"] == HUB_ID
    res = await hass.config_entries.flow.async_configure(res["flow_id"], {})
    assert res["reason"] == "hub_moved_updated" and entry.data["host"] == "10.1.2.3"


async def test_discovery_with_another_hardware_id_is_a_new_hub(hass):
    entry = make_entry("10.9.9.9", 8099)
    entry.add_to_hass(hass)
    hass.config_entries.async_update_entry(entry, data={**entry.data, const.CONF_HW_ID: HW})
    res = await hass.config_entries.flow.async_init(
        DOMAIN,
        context={"source": config_entries.SOURCE_ZEROCONF},
        data=_zc("brand-new-id", "111111111111"),
    )
    assert res["type"] is FlowResultType.FORM and res["step_id"] == "user"


# -- old firmware ----------------------------------------------------------------------------------------
async def old_hub(fake_hub):
    async def handler(ws, env):
        if env["t"] == "hello":
            await ws.send_json({"v": 1, "id": env["id"], "t": "hello_ack", "d": {"hub_id": HUB_ID, "tcp_keepalive": True}})
        elif env["t"] == "set_hub_id":
            await ws.send_json(
                {
                    "v": 1,
                    "id": env["id"],
                    "t": "error",
                    "d": {"code": "unknown_type", "message": "unknown message type"},
                }
            )

    return await fake_hub(handler)


async def test_old_firmware_shows_the_fields_read_only_with_an_explanation(hass, fake_hub):
    hub_ = await old_hub(fake_hub)
    entry = make_entry("127.0.0.1", hub_.port)
    entry.add_to_hass(hass)
    hass.config_entries.async_update_entry(entry, data={**entry.data, const.CONF_CAN_RENAME: False})
    res = await entry.start_reconfigure_flow(hass)
    assert {str(k) for k in res["data_schema"].schema} == {"host", "port", "token"}
    ph = res["description_placeholders"]
    assert ph["hub_id"] == HUB_ID and "newer hub firmware" in ph["note"]
    # saving the connection still works
    data = {"host": "127.0.0.1", "port": hub_.port, "token": TOKEN}
    res = await hass.config_entries.flow.async_configure(res["flow_id"], data)
    assert res["reason"] == "reconfigure_successful" and entry.unique_id == HUB_ID


async def test_old_firmware_refuses_when_asked_to_rename_anyway(hass, fake_hub):
    hub_ = await old_hub(fake_hub)
    entry = make_entry("127.0.0.1", hub_.port)  # capability unknown: the fields are offered
    entry.add_to_hass(hass)
    res = await reconfigure(hass, entry)
    assert res["type"] is FlowResultType.FORM and res["errors"] == {"base": "rename_unsupported"}
    assert entry.unique_id == HUB_ID and const.CONF_HUB_NAME not in entry.data


# -- end to end: rename, then edit and apply a config under the new id ------------------------------------
async def test_rename_then_apply_a_config_end_to_end(hass, hub, fake_addon):
    entry = await set_up(hass, hub, fake_addon)
    old_rev = entry.runtime_data.rev
    res = await reconfigure(hass, entry)
    assert res["reason"] == "reconfigure_successful"
    await settle(hass, entry)
    coord = entry.runtime_data
    assert (
        coord.hub_id == NEW and coord.rev == old_rev
    )  # the moved config is what runs, nothing re-applied

    # the editor (keyed by the new id) applies an edited config: a third activity
    fake_addon.apply_yaml(
        YAML.replace(
            "activities:\n",
            "activities:\n  - {id: extra, name: Extra, devices: [{device: tv, input: hdmi1}]}\n",
        ),
        NEW,
    )
    async with asyncio.timeout(15):
        while coord.rev == old_rev or coord.config_state != "accepted":
            await asyncio.sleep(0.05)
            await hass.async_block_till_done()
    assert "Extra" in coord.activities
    ack = await hub_says(hub)
    assert ack["config_hash"] == coord.compiled.config_hash and "Extra" in ack["activities"]
    assert fake_addon.store.hubs() == [NEW]
    state = fake_addon.store.load(NEW)
    assert state["applied"] is not None and state["applied"]["hub_id"] == NEW  # the store came along
    # the integration told the add-on it worked, under the new key
    async with asyncio.timeout(10):
        while (fake_addon.acks.get(NEW) or {}).get("result") != "ok":
            await asyncio.sleep(0.05)
            await hass.async_block_till_done()


# -- review fixes ---------------------------------------------------------------------------------------
async def test_dotted_new_ids_are_refused(hass, hub, fake_addon):
    entry = await set_up(hass, hub, fake_addon)
    res = await reconfigure(hass, entry, "living.room")
    assert res["errors"] == {"base": "invalid_hub_id"} and (await hub_says(hub))["hub_id"] == HUB_ID


async def test_old_addon_refuses_the_rename(hass, hub, fake_addon, monkeypatch):
    entry = await set_up(hass, hub, fake_addon)
    before = snapshot(hass, entry)
    fake_addon.no_adopt = True  # an add-on that ignores the rename fields
    res = await reconfigure(hass, entry)
    assert res["errors"] == {"base": "addon_too_old"}
    await settle(hass, entry)
    assert snapshot(hass, entry) == before and (await hub_says(hub))["hub_id"] == HUB_ID
    assert fake_addon.store.hubs() == [HUB_ID]


async def test_addon_without_a_channel_refuses_the_rename(hass, hub, fake_addon):
    entry = await set_up(hass, hub, fake_addon)
    before = snapshot(hass, entry)
    fake_addon.refuse = 404  # an add-on from before the channel
    res = await reconfigure(hass, entry)
    assert res["errors"] == {"base": "addon_too_old"}
    await settle(hass, entry, link="addon_outdated")
    after = snapshot(hass, entry)
    assert after["entities"] == before["entities"] and after["unique_id"] == HUB_ID
    assert fake_addon.store.hubs() == [HUB_ID] and (await hub_says(hub))["hub_id"] == HUB_ID


async def test_previous_id_is_kept_until_the_addon_answers_adopt(
    hass, hub, fake_addon, monkeypatch
):
    entry = await set_up(hass, hub, fake_addon)
    await hass.config_entries.async_unload(entry.entry_id)
    await raw_rename(hub, "from-hub-conf")
    fake_addon.no_adopt = True
    assert await hass.config_entries.async_setup(entry.entry_id)
    async with asyncio.timeout(30):  # under load the first poll can be late
        while entry.runtime_data.link_state not in ("ok", "nothing_applied"):
            await asyncio.sleep(0.05)
    await fake_addon.frame("hello", lambda f: f.get("prev") == HUB_ID)  # the hello still carries the previous id
    assert entry.unique_id == "from-hub-conf" and entry.data[const.CONF_PREV_ID] == HUB_ID
    assert entry.runtime_data._link.identity["prev"] == HUB_ID


async def test_a_hub_reporting_no_hardware_id_is_not_renamed(hass, start_hub, tmp_path, fake_addon):
    d = tmp_path / "ident"
    d.mkdir()
    hub_ = start_hub("--ident-dir", str(d))  # --sim reads no MAC
    fake_addon.apply_yaml(YAML)
    entry = make_entry("127.0.0.1", hub_.port, addon=fake_addon)
    entry.add_to_hass(hass)
    assert await hass.config_entries.async_setup(entry.entry_id)
    await hass.async_block_till_done()
    res = await reconfigure(hass, entry)
    assert res["errors"] == {"base": "no_hw_id"} and (await hub_says(hub_))["hub_id"] == HUB_ID


async def test_hardware_id_is_stored_before_the_hub_is_renamed(hass, hub, fake_addon, monkeypatch):
    from custom_components.ha_harmony.api import HarmonyHubClient

    entry = await set_up(hass, hub, fake_addon)
    hass.config_entries.async_update_entry(
        entry, data={k: v for k, v in entry.data.items() if k != const.CONF_HW_ID}
    )
    seen = []
    real = HarmonyHubClient.async_set_identity

    async def spy(self, *a, **k):
        seen.append(entry.data.get(const.CONF_HW_ID))
        return await real(self, *a, **k)

    monkeypatch.setattr(HarmonyHubClient, "async_set_identity", spy)
    res = await reconfigure(hass, entry)
    assert res["reason"] == "reconfigure_successful" and seen[0] == HW


async def test_an_entry_waiting_to_retry_is_unloaded_and_serialised(hass, hub, fake_addon, monkeypatch):
    entry = await set_up(hass, hub, fake_addon)
    unloads = []
    real = hass.config_entries.async_unload

    async def spy(entry_id, *a, **k):
        unloads.append(entry_id)
        return await real(entry_id, *a, **k)

    monkeypatch.setattr(hass.config_entries, "async_unload", spy)
    entry.mock_state(hass, ConfigEntryState.SETUP_RETRY)
    async with rename.entry_lock(hass, entry):  # setup-follow / another rename in progress
        task = asyncio.create_task(reconfigure_name_only(hass, entry, hub, "Den"))
        for _ in range(20):
            await asyncio.sleep(0.01)
        assert unloads == [] and (await hub_says(hub)).get("name") is None  # waits for the lock
    await task
    assert unloads[:1] == [entry.entry_id] and (await hub_says(hub))["name"] == "Den"


async def test_setup_follow_keeps_going_when_the_addon_holds_both_ids(hass, hub, fake_addon):
    from homeassistant.helpers import issue_registry as ir

    entry = await set_up(hass, hub, fake_addon)
    await hass.config_entries.async_unload(entry.entry_id)
    fake_addon.apply_yaml(YAML, "from-hub-conf")
    await raw_rename(hub, "from-hub-conf")
    assert await hass.config_entries.async_setup(entry.entry_id)
    await hass.async_block_till_done()
    assert entry.state is ConfigEntryState.LOADED and entry.unique_id == "from-hub-conf"
    issue = ir.async_get(hass).async_get_issue(DOMAIN, "addon_store_exists_from-hub-conf")
    assert issue is not None and issue.translation_placeholders == {"old": HUB_ID, "new": "from-hub-conf"}
    assert sorted(fake_addon.store.hubs()) == sorted([HUB_ID, "from-hub-conf"])  # nothing overwritten


async def test_reconfigure_accepts_the_hubs_current_id_when_the_hardware_id_matches(hass, hub, fake_addon):
    entry = await set_up(hass, hub, fake_addon)
    await raw_rename(hub, "from-hub-conf")  # renamed on the device while HA still knows the old id
    res = await reconfigure(hass, entry, HUB_ID, "")
    assert res["type"] is FlowResultType.ABORT and res["reason"] == "reconfigure_successful"
    await hass.async_block_till_done()
    assert entry.unique_id == "from-hub-conf"  # setup followed it


async def test_reconfigure_says_when_a_typed_rename_was_not_applied(hass, hub, fake_addon):
    entry = await set_up(hass, hub, fake_addon)
    await raw_rename(hub, "from-hub-conf")
    res = await reconfigure(hass, entry, "something-else", "Den")
    assert res["type"] is FlowResultType.ABORT and res["reason"] == "hub_renamed_not_applied"


async def test_entry_from_older_code_without_hw_id_learns_it_and_is_not_a_different_hub(
    hass, hub, fake_addon
):
    """Upgrade: the entry has no hw_id, the hub answers under the SAME id and now reports one."""
    fake_addon.apply_yaml(YAML)
    entry = make_entry("127.0.0.1", hub.port, addon=fake_addon)
    assert const.CONF_HW_ID not in entry.data
    entry.add_to_hass(hass)
    assert await hass.config_entries.async_setup(entry.entry_id)
    await hass.async_block_till_done()
    assert entry.state is ConfigEntryState.LOADED and entry.unique_id == HUB_ID
    assert entry.data[const.CONF_HW_ID] == HW
