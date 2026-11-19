"""ha_harmony.install_firmware: client-side refusals, chunked upload, resume, hub errors, rollback repair issue.

A scripted hub (no binaries) plays the ota_* messages; images carry a real HOTA header but a dummy
signature, because verification is the hub's job (tests/py/test_ota_hub.py covers it against the real daemon).
"""

import asyncio
import base64
import hashlib
import struct

import pytest
from conftest import HUB_ID, make_entry
from homeassistant.exceptions import HomeAssistantError, ServiceValidationError
from homeassistant.helpers import issue_registry as ir

import custom_components.ha_harmony  # noqa: F401  (before the HA plugin shadows it)
from custom_components.ha_harmony.ota import CHUNK, HOTA_HEADER, parse_image

pytestmark = pytest.mark.usefixtures("enable_custom_integrations", "fast_client")


def make_image(payload: bytes, build: int = 12, target: int = 1, trim: int = 0) -> bytes:
    hdr = struct.pack("<4sBBHII", b"HOTA", 1, target, 0, build, len(payload)) + hashlib.sha512(payload).digest()
    img = hdr + b"\0" * 64 + payload
    assert len(img) == HOTA_HEADER + len(payload)
    return img[: len(img) - trim]


class ScriptedHub:
    """Holds one upload like the daemon does: begin returns the offset it has, chunks append in order."""

    def __init__(self, *, rollback=None, drop_after=None, fail_end=None, ota_key="release", lose_end_reply=False):
        self.rollback = rollback  # (build, reason) pushed after hello
        self.drop_after = drop_after  # close the first connection after this many chunks
        self.fail_end = fail_end  # error code answered to ota_end
        self.ota_key = ota_key
        self.lose_end_reply = lose_end_reply  # apply, then vanish without answering ota_end (the daemon exits)
        self.fw_build = 11
        self.pending_build = 0
        self.held = bytearray()
        self.begins = []
        self.ends = 0
        self.dropped = False
        self.seen = []

    async def __call__(self, ws, env):
        t, d, rid = env["t"], env["d"], env["id"]
        self.seen.append(t)
        reply = {"v": 1, "id": rid}
        if t == "hello":
            await ws.send_json({**reply, "t": "hello_ack", "d": {"hub_id": HUB_ID, "activities": [], "fw_build": self.fw_build, "ota_key": self.ota_key}})
            if self.rollback:
                await ws.send_json({"v": 1, "id": 0, "t": "ota_status", "d": {
                    "state": "rolled_back", "offset": 0, "size": 0, "build": self.rollback[0], "reason": self.rollback[1]}})  # fmt: skip
        elif t == "ota_begin":
            self.begins.append(d)
            self.pending_build = d["build"]
            await ws.send_json({**reply, "t": "ota_status", "d": {"state": "receiving", "offset": len(self.held), "size": d["size"]}})  # fmt: skip
        elif t == "ota_chunk":
            assert d["offset"] == len(self.held), "chunks must continue at the held offset"
            self.held += base64.b64decode(d["data"])
            if self.drop_after and not self.dropped and len(self.held) >= self.drop_after * CHUNK:
                self.dropped = True
                await ws.close()
                return
            await ws.send_json({**reply, "t": "ota_status", "d": {"state": "receiving", "offset": len(self.held), "size": 0}})  # fmt: skip
        elif t == "ota_end":
            self.ends += 1
            if self.fail_end:
                await ws.send_json({**reply, "t": "error", "d": {"code": self.fail_end, "message": "signature check failed"}})  # fmt: skip
            else:
                self.fw_build = self.pending_build
                if not self.lose_end_reply:
                    await ws.send_json({**reply, "t": "ota_status", "d": {"state": "applied", "offset": 0, "size": 0, "build": self.fw_build}})  # fmt: skip
                await ws.close()  # the daemon exits for the supervisor; the client reconnects
        elif t == "ping":
            await ws.send_json({**reply, "t": "pong", "d": {}})


@pytest.fixture(autouse=True)
def private_config_dir(hass, tmp_path):
    """The HA test config dir is shared by every xdist worker: give each test its own."""
    hass.config.config_dir = str(tmp_path / "cfg")


@pytest.fixture
async def rig(hass, fake_hub):
    entries = []

    async def setup(script: ScriptedHub):
        hub = await fake_hub(script)
        entry = make_entry("127.0.0.1", hub.port)
        entry.add_to_hass(hass)
        entries.append(entry)
        assert await hass.config_entries.async_setup(entry.entry_id)
        await hass.async_block_till_done()
        c = entry.runtime_data

        async def quick(_s):
            await asyncio.sleep(0.01)

        c._sleep = quick  # reconnect waits: no 0.5 s real sleeps
        return c

    yield setup
    for entry in entries:
        await hass.config_entries.async_unload(entry.entry_id)


@pytest.fixture
def fw_dir(hass):
    import pathlib

    d = pathlib.Path(hass.config.path("ha_harmony", "firmware"))
    d.mkdir(parents=True, exist_ok=True)
    return d


async def install(hass, path):
    await hass.services.async_call("ha_harmony", "install_firmware", {"path": str(path)}, blocking=True)


async def test_uploads_a_signed_image_in_bounded_chunks_and_reports_progress(hass, rig, fw_dir):
    hub = ScriptedHub()
    c = await rig(hub)
    img = make_image(bytes(range(256)) * 80, build=12)  # ~20 KiB: five chunks
    f = fw_dir / "hub-12.img"
    f.write_bytes(img)
    seen = []
    c.async_subscribe_topics(("ota",), lambda: seen.append((c.ota_state, c.ota_progress)))
    await install(hass, f)
    assert bytes(hub.held) == img
    assert hub.begins == [{"target": 1, "size": len(img), "build": 12, "sha": hashlib.sha512(img).hexdigest()}]
    assert hub.ends == 1
    states = [s for s, _ in seen]
    assert states[0] == "uploading" and "applying" in states and states[-1] == "restarting"
    progress = [p for s, p in seen if s == "uploading"]
    assert progress == sorted(progress) and progress[-1] == 100
    st = hass.states.get("sensor.harmony_hub_firmware_update") or next(
        s for s in hass.states.async_all("sensor") if s.attributes.get("build") == 12
    )
    assert st.state == "restarting" and st.attributes["progress"] == 100


@pytest.mark.parametrize(
    ("data", "key"),
    [
        (b"MZ" + b"\0" * 400, "ota_not_signed"),
        (b"HOT", "ota_not_signed"),
        (make_image(b"x" * 500, target=2), "ota_wrong_target"),
        (make_image(b"x" * 500, trim=1), "ota_truncated"),
        (make_image(b"x" * 500)[:300], "ota_truncated"),
    ],
)
async def test_unsigned_truncated_or_remote_images_are_refused_before_anything_is_sent(hass, rig, fw_dir, data, key):
    hub = ScriptedHub()
    await rig(hub)
    f = fw_dir / "x.img"
    f.write_bytes(data)
    with pytest.raises(ServiceValidationError) as e:
        await install(hass, f)
    assert e.value.translation_key == key
    assert not any(t.startswith("ota_") for t in hub.seen)


async def test_paths_outside_the_allowed_directories_are_refused(hass, rig, tmp_path, fw_dir):
    hub = ScriptedHub()
    await rig(hub)
    outside = tmp_path / "elsewhere.img"
    outside.write_bytes(make_image(b"x" * 100))
    with pytest.raises(ServiceValidationError) as e:
        await install(hass, outside)
    assert e.value.translation_key == "ota_path_not_allowed"
    (fw_dir / "link.img").symlink_to(outside)  # a symlink out of the allowed dir does not smuggle it in
    with pytest.raises(ServiceValidationError) as e:
        await install(hass, fw_dir / "link.img")
    assert e.value.translation_key == "ota_path_not_allowed"
    with pytest.raises(ServiceValidationError) as e:
        await install(hass, fw_dir / ".." / ".." / "elsewhere.img")
    assert e.value.translation_key == "ota_path_not_allowed"
    with pytest.raises(ServiceValidationError) as e:
        await install(hass, fw_dir / "missing.img")
    assert e.value.translation_key == "ota_file_missing"
    assert not any(t.startswith("ota_") for t in hub.seen)


async def test_an_external_dir_on_the_allowlist_is_accepted(hass, rig, tmp_path):
    hub = ScriptedHub()
    await rig(hub)
    ext = tmp_path / "ext"
    ext.mkdir()
    hass.config.allowlist_external_dirs = {str(ext)}
    f = ext / "a.img"
    f.write_bytes(make_image(b"y" * 100))
    await install(hass, f)
    assert hub.ends == 1


async def test_hub_verification_failure_is_surfaced_and_marks_the_update_failed(hass, rig, fw_dir):
    hub = ScriptedHub(fail_end="ota_verify")
    c = await rig(hub)
    f = fw_dir / "bad.img"
    f.write_bytes(make_image(b"z" * 5000))
    with pytest.raises(HomeAssistantError) as e:
        await install(hass, f)
    assert "ota_verify" in str(e.value) or e.value.translation_key == "ota_failed"
    assert c.ota_state == "failed" and "ota_verify" in (c.ota_error or "")


async def test_dropped_connection_resumes_from_the_hub_offset(hass, rig, fw_dir):
    hub = ScriptedHub(drop_after=2)
    c = await rig(hub)
    img = make_image(bytes(range(256)) * 80)
    f = fw_dir / "r.img"
    f.write_bytes(img)
    await install(hass, f)
    assert hub.dropped and bytes(hub.held) == img  # nothing re-sent, nothing missing
    assert len(hub.begins) == 2 and hub.ends == 1
    assert c.ota_state == "restarting"


async def test_rollback_report_becomes_a_repair_issue_and_a_good_update_clears_it(hass, rig, fw_dir):
    hub = ScriptedHub(rollback=(9, "died_early"))
    c = await rig(hub)
    reg = ir.async_get(hass)
    issue = reg.async_get_issue("ha_harmony", f"ota_rolled_back_{HUB_ID}_9")
    assert issue is not None and issue.translation_placeholders["reason"] == "died_early"
    assert issue.severity is ir.IssueSeverity.WARNING and c.ota_state == "rolled_back"
    f = fw_dir / "fix.img"
    f.write_bytes(make_image(b"q" * 300, build=13))
    await install(hass, f)
    assert reg.async_get_issue("ha_harmony", f"ota_rolled_back_{HUB_ID}_9") is None


async def test_a_second_install_while_one_runs_is_refused(hass, rig, fw_dir):
    hub = ScriptedHub()
    c = await rig(hub)
    f = fw_dir / "a.img"
    f.write_bytes(make_image(b"p" * 100))
    await c._ota_lock.acquire()
    with pytest.raises(ServiceValidationError) as e:
        await install(hass, f)
    assert e.value.translation_key == "ota_in_progress"
    c._ota_lock.release()


def test_parse_image_reads_build_and_whole_file_digest():
    img = make_image(b"abc", build=77)
    info = parse_image(img)
    assert info.build == 77 and info.sha512 == hashlib.sha512(img).hexdigest() and info.size == len(img)


async def test_a_lost_ota_end_reply_never_uploads_or_applies_twice(hass, rig, fw_dir):
    hub = ScriptedHub(lose_end_reply=True)
    c = await rig(hub)
    img = make_image(bytes(range(256)) * 40, build=12)
    f = fw_dir / "lost.img"
    f.write_bytes(img)
    await install(hass, f)
    assert hub.ends == 1 and len(hub.begins) == 1  # exactly one begin and one end
    assert c.ota_state == "restarting" and c.client.fw_build == 12


async def test_never_confirmed_build_fails_the_update_without_resending(hass, rig, fw_dir, monkeypatch):
    hub = ScriptedHub(lose_end_reply=True)
    c = await rig(hub)
    hub.pending_build = 0
    monkeypatch.setattr(type(c), "_await_build", lambda self, build, tries=480: _never(self, build), raising=True)
    f = fw_dir / "x.img"
    f.write_bytes(make_image(b"k" * 300, build=12))
    with pytest.raises(HomeAssistantError):
        await install(hass, f)
    assert hub.ends == 1 and c.ota_state == "failed"


async def _never(self, build):
    return False


async def test_dev_key_hub_needs_an_explicit_opt_in(hass, rig, fw_dir):
    hub = ScriptedHub(ota_key="dev")
    c = await rig(hub)
    reg = ir.async_get(hass)
    assert reg.async_get_issue("ha_harmony", f"ota_dev_key_{HUB_ID}") is not None
    f = fw_dir / "d.img"
    f.write_bytes(make_image(b"d" * 200, build=12))
    with pytest.raises(ServiceValidationError) as e:
        await install(hass, f)
    assert e.value.translation_key == "ota_dev_key"
    assert not any(t.startswith("ota_") for t in hub.seen)
    await hass.services.async_call(
        "ha_harmony", "install_firmware", {"path": str(f), "allow_dev_key": True}, blocking=True
    )
    assert hub.ends == 1
    assert c.client.ota_key == "dev"
    st = next(x for x in hass.states.async_all("sensor") if "ota_key" in x.attributes)
    assert st.attributes["ota_key"] == "dev"


async def test_release_key_hub_has_no_dev_issue(hass, rig):
    await rig(ScriptedHub())
    assert ir.async_get(hass).async_get_issue("ha_harmony", f"ota_dev_key_{HUB_ID}") is None
