"""The hub firmware update entity: the add-on's `fw` announcement, version compare by build, the install flow."""

import hashlib
from types import SimpleNamespace

import pytest
from homeassistant.exceptions import ServiceValidationError
from test_ota_ha import ScriptedHub, make_image, rig  # noqa: F401  (rig is a fixture)

import custom_components.ha_harmony  # noqa: F401  (before the HA plugin shadows it)
from custom_components.ha_harmony.addon_link import FwInfo, parse_fw

pytestmark = pytest.mark.usefixtures("enable_custom_integrations", "fast_client")

EID = "update.harmony_hub_sim_hub_firmware"


def fw_for(img: bytes, build: int, version="0.5.9") -> FwInfo:
    return FwInfo(version, build, len(img), hashlib.sha512(img).hexdigest())


def test_parse_fw_accepts_a_well_formed_announcement():
    raw = {"version": "0.5.9", "build": 760, "size": 5, "sha512": "ab" * 64}
    assert parse_fw(raw) == FwInfo("0.5.9", 760, 5, "ab" * 64)


@pytest.mark.parametrize(
    "bad",
    [
        None,
        "x",
        [],
        {},
        {"version": "1", "build": 1, "size": 1},  # no sha512
        {"version": 1, "build": 1, "size": 1, "sha512": "a"},
        {"version": "1", "build": "1", "size": 1, "sha512": "a"},
        {"version": "1", "build": True, "size": 1, "sha512": "a"},
        {"version": "1", "build": 1, "size": -1, "sha512": "a"},
        {"version": "1", "build": 1, "size": 1 << 40, "sha512": "a"},
        {"version": "x" * 100, "build": 1, "size": 1, "sha512": "a"},
    ],
)
def test_parse_fw_ignores_malformed_announcements(bad):
    assert parse_fw(bad) is None


def entity_state(hass):
    st = hass.states.get(EID)
    assert st is not None, [s.entity_id for s in hass.states.async_all("update")]
    return st


async def test_no_addon_image_means_no_latest_version(hass, rig):  # noqa: F811
    await rig(ScriptedHub())
    st = entity_state(hass)
    assert st.state == "unknown"
    assert st.attributes["installed_version"].endswith("(11)")
    assert st.attributes["latest_version"] is None


async def test_newer_by_build_not_by_version_string(hass, rig):  # noqa: F811
    c = await rig(ScriptedHub())  # the hub runs build 11
    c.addon_fw = fw_for(
        b"x" * 500, 12, version="0.0.1"
    )  # the version string is lower, the build higher
    c._notify("ota")
    await hass.async_block_till_done()
    st = entity_state(hass)
    assert st.state == "on" and st.attributes["latest_version"].endswith("(12)")
    c.addon_fw = fw_for(b"x" * 500, 11)
    c._notify("ota")
    await hass.async_block_till_done()
    assert entity_state(hass).state == "off"
    c.addon_fw = fw_for(b"x" * 500, 10)
    c._notify("ota")
    await hass.async_block_till_done()
    assert entity_state(hass).state == "off"


async def test_install_streams_the_addon_image_and_reports_progress(hass, rig):  # noqa: F811
    hub = ScriptedHub()
    c = await rig(hub)
    img = make_image(bytes(range(256)) * 40, build=12)
    c.addon_fw = fw_for(img, 12)

    async def fetch():
        return img

    c._link = SimpleNamespace(fetch_firmware=fetch)
    seen = []
    c.async_subscribe_topics(
        ("ota",), lambda: seen.append(entity_state(hass).attributes["in_progress"])
    )
    await hass.services.async_call("update", "install", {"entity_id": EID}, blocking=True)
    assert bytes(hub.held) == img and hub.ends == 1
    assert True in seen  # in_progress while uploading/applying
    st = entity_state(hass)
    assert st.attributes["in_progress"] is False
    assert st.attributes["installed_version"].endswith("(12)")
    assert st.state == "off"


@pytest.mark.parametrize(
    "mutate",
    [
        lambda img, fw: (
            img,
            FwInfo(fw.version, fw.build + 1, fw.size, fw.sha512),
        ),  # build differs
        lambda img, fw: (img, FwInfo(fw.version, fw.build, fw.size + 1, fw.sha512)),  # size differs
        lambda img, fw: (img, FwInfo(fw.version, fw.build, fw.size, "00" * 64)),  # hash differs
        lambda img, fw: (img[:-1] + b"\x01", fw),  # tampered bytes
    ],
)
async def test_install_refuses_an_image_that_disagrees_with_the_announcement(hass, rig, mutate):  # noqa: F811
    hub = ScriptedHub()
    c = await rig(hub)
    img = make_image(b"y" * 600, build=12)
    img, fw = mutate(img, fw_for(img, 12))
    c.addon_fw = fw

    async def fetch():
        return img

    c._link = SimpleNamespace(fetch_firmware=fetch)
    with pytest.raises(ServiceValidationError) as e:
        await hass.services.async_call("update", "install", {"entity_id": EID}, blocking=True)
    assert e.value.translation_key == "ota_image_mismatch"
    assert not any(t.startswith("ota_") for t in hub.seen)


async def test_install_refuses_a_hub_that_trusts_the_dev_key_before_fetching(hass, rig):  # noqa: F811
    hub = ScriptedHub(ota_key="dev")
    c = await rig(hub)
    img = make_image(b"y" * 600, build=12)
    c.addon_fw = fw_for(img, 12)
    fetched = []

    async def fetch():
        fetched.append(1)
        return img

    c._link = SimpleNamespace(fetch_firmware=fetch)
    with pytest.raises(ServiceValidationError) as e:
        await hass.services.async_call("update", "install", {"entity_id": EID}, blocking=True)
    assert e.value.translation_key == "ota_dev_key"
    assert not fetched and not any(t.startswith("ota_") for t in hub.seen)


async def test_install_without_an_addon_image_is_refused(hass, rig):  # noqa: F811
    hub = ScriptedHub()
    c = await rig(hub)
    with pytest.raises(ServiceValidationError) as e:
        await c.async_install_addon_firmware()
    assert e.value.translation_key == "ota_no_addon_image"
    assert not any(t.startswith("ota_") for t in hub.seen)
