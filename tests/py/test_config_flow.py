"""Config flow against the real sim hub and an in-test hub that speaks another protocol version."""

import pytest
from conftest import HUB_ID, TOKEN
from homeassistant import config_entries
from homeassistant.data_entry_flow import FlowResultType
from homeassistant.helpers.service_info.zeroconf import ZeroconfServiceInfo

from custom_components.ha_harmony.const import DOMAIN

from conftest import make_entry  # isort: skip

import custom_components.ha_harmony  # noqa: F401  (before the HA plugin shadows it)

pytestmark = pytest.mark.usefixtures("enable_custom_integrations")


async def _start(hass, source=config_entries.SOURCE_USER):
    return await hass.config_entries.flow.async_init(DOMAIN, context={"source": source})


def _input(port, token=TOKEN, host="127.0.0.1"):
    return {"host": host, "port": port, "token": token}


async def test_user_flow_creates_entry(hass, hub_process, fast_client):
    res = await _start(hass)
    assert res["type"] is FlowResultType.FORM and res["step_id"] == "user"
    res = await hass.config_entries.flow.async_configure(res["flow_id"], _input(hub_process.port))
    assert res["type"] is FlowResultType.CREATE_ENTRY
    assert res["data"] == _input(hub_process.port)
    assert res["result"].unique_id == HUB_ID


async def test_bad_token_shows_invalid_auth(hass, hub_process):
    res = await _start(hass)
    res = await hass.config_entries.flow.async_configure(
        res["flow_id"], _input(hub_process.port, token="nope")
    )
    assert res["type"] is FlowResultType.FORM and res["errors"] == {"base": "invalid_auth"}


async def test_unreachable_host_shows_cannot_connect(hass):
    from conftest import free_port

    res = await _start(hass)
    res = await hass.config_entries.flow.async_configure(res["flow_id"], _input(free_port()))
    assert res["type"] is FlowResultType.FORM and res["errors"] == {"base": "cannot_connect"}


async def test_other_protocol_version_shows_unsupported_version(hass, fake_hub):
    async def handler(ws, env):
        await ws.send_json(
            {"v": 1, "id": env["id"], "t": "error", "d": {"code": "version", "message": "v2 only"}}
        )

    hub = await fake_hub(handler)
    res = await _start(hass)
    res = await hass.config_entries.flow.async_configure(res["flow_id"], _input(hub.port))
    assert res["type"] is FlowResultType.FORM and res["errors"] == {"base": "unsupported_version"}


async def test_envelope_with_other_v_shows_unsupported_version(hass, fake_hub):
    async def handler(ws, env):
        await ws.send_json({"v": 2, "id": env["id"], "t": "hello_ack", "d": {"hub_id": "x"}})

    hub = await fake_hub(handler)
    res = await _start(hass)
    res = await hass.config_entries.flow.async_configure(res["flow_id"], _input(hub.port))
    assert res["errors"] == {"base": "unsupported_version"}


async def test_duplicate_hub_aborts(hass, hub_process, fast_client):
    make_entry("127.0.0.1", hub_process.port).add_to_hass(hass)
    res = await _start(hass)
    res = await hass.config_entries.flow.async_configure(res["flow_id"], _input(hub_process.port))
    assert res["type"] is FlowResultType.ABORT and res["reason"] == "already_configured"


def _zc(hub_id=HUB_ID, port=8099):
    return ZeroconfServiceInfo(
        ip_address="10.1.2.3",
        ip_addresses=["10.1.2.3"],
        hostname="hub.local.",
        name=f"{hub_id}._haharmony._tcp.local.",
        port=port,
        properties={"hub_id": hub_id},
        type="_haharmony._tcp.local.",
    )


async def test_zeroconf_prefills_host(hass):
    res = await hass.config_entries.flow.async_init(
        DOMAIN, context={"source": config_entries.SOURCE_ZEROCONF}, data=_zc()
    )
    assert res["type"] is FlowResultType.FORM and res["step_id"] == "user"
    schema = {str(k): k.default() for k in res["data_schema"].schema if callable(k.default)}
    assert schema["host"] == "10.1.2.3" and schema["port"] == 8099


async def test_zeroconf_same_address_aborts(hass):
    make_entry("10.1.2.3", 8099).add_to_hass(hass)
    res = await hass.config_entries.flow.async_init(
        DOMAIN, context={"source": config_entries.SOURCE_ZEROCONF}, data=_zc()
    )
    assert res["type"] is FlowResultType.ABORT and res["reason"] == "already_configured"


async def test_hostile_zeroconf_does_not_retarget_or_connect(hass, monkeypatch):
    """A LAN host answering mDNS as a configured hub must not get the token (sent in `hello`)."""
    from custom_components.ha_harmony import config_flow

    probes = []

    async def probe(hass, host, port, token):
        probes.append((host, port, token))
        return HUB_ID

    monkeypatch.setattr(config_flow, "_probe", probe)
    entry = make_entry("10.9.9.9", 8099)
    entry.add_to_hass(hass)
    res = await hass.config_entries.flow.async_init(
        DOMAIN, context={"source": config_entries.SOURCE_ZEROCONF}, data=_zc()
    )
    assert res["type"] is FlowResultType.FORM and res["step_id"] == "hub_moved"
    assert res["description_placeholders"]["new_host"] == "10.1.2.3:8099"
    assert res["description_placeholders"]["old_host"] == "10.9.9.9:8099"
    assert probes == [] and entry.data["host"] == "10.9.9.9"  # nothing happened without the user
    # the user confirms: only then is the token sent, and the address changes
    res = await hass.config_entries.flow.async_configure(res["flow_id"], {})
    assert res["type"] is FlowResultType.ABORT and res["reason"] == "hub_moved_updated"
    assert probes == [("10.1.2.3", 8099, entry.data["token"])]
    assert entry.data["host"] == "10.1.2.3"


async def test_moved_hub_that_is_another_hub_aborts(hass, monkeypatch):
    from custom_components.ha_harmony import config_flow

    async def probe(hass, host, port, token):
        return "some-other-hub"

    monkeypatch.setattr(config_flow, "_probe", probe)
    entry = make_entry("10.9.9.9", 8099)
    entry.add_to_hass(hass)
    res = await hass.config_entries.flow.async_init(
        DOMAIN, context={"source": config_entries.SOURCE_ZEROCONF}, data=_zc()
    )
    res = await hass.config_entries.flow.async_configure(res["flow_id"], {})
    assert res["type"] is FlowResultType.ABORT and res["reason"] == "wrong_hub"
    assert entry.data["host"] == "10.9.9.9"


async def test_reconfigure_changes_host_port_and_token(hass, hub_process, fast_client):
    entry = make_entry("10.9.9.9", 1, token="old")
    entry.add_to_hass(hass)
    res = await entry.start_reconfigure_flow(hass)
    assert res["type"] is FlowResultType.FORM and res["step_id"] == "reconfigure"
    res = await hass.config_entries.flow.async_configure(
        res["flow_id"], _input(hub_process.port, token="bad")
    )
    assert res["errors"] == {"base": "invalid_auth"}
    res = await hass.config_entries.flow.async_configure(res["flow_id"], _input(hub_process.port))
    assert res["type"] is FlowResultType.ABORT and res["reason"] == "reconfigure_successful"
    assert entry.data["host"] == "127.0.0.1" and entry.data["port"] == hub_process.port
    assert entry.data["token"] == TOKEN


async def test_options_flow_toggles_hold_events(hass):
    entry = make_entry("10.9.9.9", 8099)
    entry.add_to_hass(hass)
    res = await hass.config_entries.options.async_init(entry.entry_id)
    assert res["type"] is FlowResultType.FORM
    res = await hass.config_entries.options.async_configure(res["flow_id"], {"hold_events": True, "all_command_buttons": False}
    )
    assert res["type"] is FlowResultType.CREATE_ENTRY
    assert entry.options == {"hold_events": True, "all_command_buttons": False}
