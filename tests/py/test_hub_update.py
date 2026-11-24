"""The hub firmware update entity: the add-on's `fw` announcement, version compare by build, the install flow."""

import asyncio
import hashlib
from types import SimpleNamespace

import pytest
from homeassistant.exceptions import HomeAssistantError, ServiceValidationError
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


async def _install_with_trace(hass, c, img):
    """Run the install; record (update entity, a sibling entity) states at every connection change."""
    c.addon_fw = fw_for(img, 12)

    async def fetch():
        return img

    c._link = SimpleNamespace(fetch_firmware=fetch)
    trace = []

    def on_avail():
        up = hass.states.get(EID)
        others = [
            s.state
            for s in hass.states.async_all()
            if s.entity_id != EID
            and s.entity_id.startswith(("sensor.harmony_hub", "binary_sensor.harmony_hub"))
        ]
        trace.append((c.available, up.state, up.attributes.get("in_progress"), others))

    c.async_subscribe_topics(("available",), on_avail)
    return trace


async def test_update_entity_stays_available_in_progress_while_the_hub_reboots(hass, rig):  # noqa: F811
    hub = ScriptedHub()
    c = await rig(hub)
    img = make_image(b"z" * 600, build=12)
    trace = await _install_with_trace(hass, c, img)
    await hass.services.async_call("update", "install", {"entity_id": EID}, blocking=True)
    offline = [t for t in trace if t[0] is False]
    assert offline, trace
    for _, state, in_progress, others in offline:
        assert state != "unavailable" and in_progress is True
        assert others and set(others) == {"unavailable"}  # every other hub entity goes unavailable
    st = entity_state(hass)
    assert st.attributes["in_progress"] is False
    assert st.attributes["installed_version"].endswith("(12)")


async def test_update_entity_clears_on_the_reconnect_itself(hass, rig):  # noqa: F811
    hub = ScriptedHub()
    c = await rig(hub)
    img = make_image(b"z" * 600, build=12)
    c.addon_fw = fw_for(img, 12)

    async def fetch():
        return img

    c._link = SimpleNamespace(fetch_firmware=fetch)
    seen = []  # (connected, update state, in_progress, installed_version) per push

    def note():
        st = hass.states.get(EID)
        seen.append(
            (
                c.available,
                st.state,
                st.attributes.get("in_progress"),
                st.attributes.get("installed_version"),
            )
        )

    c.async_subscribe_topics(("available", "ota"), note)
    await hass.services.async_call("update", "install", {"entity_id": EID}, blocking=True)
    first_off = next(i for i, t in enumerate(seen) if t[0] is False)
    after = seen[first_off:]
    assert all(t[1] != "unavailable" for t in after)  # never "unavailable" across the reboot
    assert all(t[2] is True for t in after[:-1])  # in_progress until the last push
    assert after[-1][0] is True and after[-1][2] is False and after[-1][3].endswith("(12)")


class StuckHub(ScriptedHub):
    """Applies nothing: after ota_end the hub reconnects still on its old build."""

    async def __call__(self, ws, env):
        await super().__call__(ws, env)
        self.fw_build = 11


class GoneHub(ScriptedHub):
    """After ota_end the hub never completes a hello again."""

    async def __call__(self, ws, env):
        if env["t"] == "hello" and self.ends:
            await ws.close()
            return
        await super().__call__(ws, env)


class NoDropHub(ScriptedHub):
    """Accepts ota_end but never restarts: the link stays up on the old build."""

    async def __call__(self, ws, env):
        if env["t"] == "ota_end":
            self.ends += 1
            await ws.send_json({"v": 1, "id": env["id"], "t": "ota_status", "d": {"state": "applied", "offset": 0, "size": 0, "build": 12}})  # fmt: skip
            return
        await super().__call__(ws, env)


def _short_wait(monkeypatch, c, secs=0.5):
    orig = type(c)._await_build

    async def short(self, build, timeout=secs):
        return await orig(self, build, secs)

    monkeypatch.setattr(type(c), "_await_build", short)


async def test_reconnect_on_the_previous_firmware_is_a_failed_update(hass, rig, monkeypatch):  # noqa: F811
    c = await rig(StuckHub())
    await _install_with_trace(hass, c, make_image(b"z" * 600, build=12))
    _short_wait(monkeypatch, c)
    with pytest.raises(HomeAssistantError) as e:
        await hass.services.async_call("update", "install", {"entity_id": EID}, blocking=True)
    assert e.value.translation_key == "ota_previous_firmware"
    assert str(e.value) == "The hub restarted on the previous firmware"
    assert c.ota_state == "failed"
    st = entity_state(hass)
    assert st.attributes["in_progress"] is False
    assert st.attributes["installed_version"].endswith("(11)")


async def test_previous_firmware_verdict_comes_at_the_reconnect_not_after_the_wait(
    hass,
    rig,  # noqa: F811
    monkeypatch,
):
    import time

    c = await rig(StuckHub())
    await _install_with_trace(hass, c, make_image(b"z" * 600, build=12))
    _short_wait(monkeypatch, c, 30.0)
    t0 = time.monotonic()
    with pytest.raises(HomeAssistantError) as e:
        await hass.services.async_call("update", "install", {"entity_id": EID}, blocking=True)
    assert e.value.translation_key == "ota_previous_firmware"
    assert time.monotonic() - t0 < 10  # far under the 30 s wait


async def test_a_hub_that_never_restarts_is_a_plain_failed_install(
    hass,
    rig,  # noqa: F811
    monkeypatch,
):
    c = await rig(NoDropHub())
    await _install_with_trace(hass, c, make_image(b"z" * 600, build=12))
    _short_wait(monkeypatch, c)
    with pytest.raises(HomeAssistantError) as e:
        await hass.services.async_call("update", "install", {"entity_id": EID}, blocking=True)
    assert e.value.translation_key == "ota_failed"
    assert c.ota_state == "failed"
    assert entity_state(hass).attributes["in_progress"] is False


async def test_hub_not_reconnecting_clears_progress_without_calling_it_failed(
    hass,
    rig,  # noqa: F811
    monkeypatch,
):
    hub = GoneHub()
    c = await rig(hub)
    await _install_with_trace(hass, c, make_image(b"z" * 600, build=12))
    _short_wait(monkeypatch, c)
    with pytest.raises(HomeAssistantError) as e:
        await hass.services.async_call("update", "install", {"entity_id": EID}, blocking=True)
    assert e.value.translation_key == "ota_hub_no_reconnect"
    assert str(e.value) == "The hub didn't reconnect after the update"
    assert c.ota_state == "idle"
    assert entity_state(hass).state == "unavailable"  # offline again: normal availability applies
    hub.ends = 0  # the hub now accepts hellos: it is on the new build
    up = asyncio.Event()
    c.async_subscribe_topics(("available",), lambda: up.set() if c.available else None)
    await asyncio.wait_for(up.wait(), 10)
    await hass.async_block_till_done()
    st = entity_state(hass)
    assert st.attributes["in_progress"] is False
    assert st.attributes["installed_version"].endswith("(12)")


async def test_update_entity_is_unavailable_when_the_hub_is_offline_and_no_install_runs(hass, rig):  # noqa: F811
    c = await rig(ScriptedHub())
    c.available = False
    c._set_ota("failed", error="x")
    await hass.async_block_till_done()
    assert entity_state(hass).state == "unavailable"
