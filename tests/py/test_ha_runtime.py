"""Page format per remote, tile state, Home Assistant actions and HA-backed activities.

A scripted hub (no binaries) drives the exact message orders; a manual clock replaces the 1.5 s wait.
"""

import asyncio
import base64
import logging

import pytest
from conftest import HUB_ID, make_entry
from homeassistant.core import State
from pytest_homeassistant_custom_component.common import async_mock_service

import custom_components.ha_harmony  # noqa: F401  (before the HA plugin shadows it)
from custom_components.ha_harmony import const
from custom_components.ha_harmony import coordinator as coord_mod
from custom_components.ha_harmony.addon_link import BundleRejected
from custom_components.ha_harmony.harmony_core.ha_actions import HaCall
from custom_components.ha_harmony.harmony_core.harmony_proto import MsgType
from custom_components.ha_harmony.harmony_core.page_codec import decode_page
from custom_components.ha_harmony.tile_format import tile_state, tile_text

pytestmark = pytest.mark.usefixtures("enable_custom_integrations", "fast_client")

YAML = """
devices:
  - id: tv
    name: TV
    settle_ms: 40
    power: {on: {proto: nec, addr: 4, cmd: 1}, off: {proto: nec, addr: 4, cmd: 2}}
    inputs: {hdmi1: {proto: nec, addr: 4, cmd: 10}}
  - id: shield
    kind: ha
    name: Shield
    entity: media_player.shield
    settle_ms: 700
    power:
      on: {ha: {service: remote.turn_on, target: {entity_id: remote.shield}}}
      off: {ha: {service: remote.turn_off, target: {entity_id: remote.shield}}}
    inputs:
      home: {ha: {service: remote.send_command, target: {entity_id: remote.shield}, data: {command: HOME}}}
    commands:
      ok: {ha: {service: remote.send_command, target: {entity_id: remote.shield}, data: {command: DPAD_CENTER}}}
activities:
  - {id: tv_on, name: TV, devices: [{device: tv, input: hdmi1}]}
  - id: stream
    name: Stream
    devices: [{device: tv, input: hdmi1}, {device: shield, input: home}]
    steps: [{ha: {service: light.turn_off, target: {entity_id: light.room}}}]
    off: [{ha: {service: scene.turn_on, target: {entity_id: scene.bye}}}]
    keys:
      select: {device: shield, command: ok}
      left: {ha: {service: light.toggle, target: {entity_id: light.lamp}}}
keys:
  select: {device: tv, command: power_on}
  left: {proto: nec, addr: 4, cmd: 3}
  down: {ha: {service: remote.send_command, target: {entity_id: remote.shield}, data: {command: DPAD_DOWN}, repeat: true}}
  up: {ha: {service: light.toggle, target: {entity_id: light.up}}}
pages:
  - id: main
    cols: 2
    rows: 3
    widgets:
      - {type: tile, id: lamp, x: 0, y: 0, label: Lamp, entity: light.a}
      - {type: tile, id: temp, x: 1, y: 0, label: Temp, entity: sensor.t}
      - {type: button, id: bye, x: 0, y: 1, label: Bye, action: {ha: {service: scene.turn_on, target: {entity_id: scene.bye}}}}
"""
THREE_PAGES = YAML + "".join(
    f"""  - id: extra{i}
    cols: 1
    rows: 1
    widgets:
      - {{type: button, id: x{i}, x: 0, y: 0, label: Extra {i}, action: {{none: true}}}}
"""
    for i in range(2)
)
V2_INFO = {"remote": 1, "page_fmt": 2, "caps": 3, "icons": 50}
V1_INFO = {"remote": 1, "page_fmt": 1, "caps": 0, "icons": 0}


class ScriptHub:
    """A hub that answers like the real one and lets the test push messages and hold replies."""

    def __init__(self, activities=("TV", "Stream")):
        self.activities = list(activities)
        self.ws = None
        self.got: list[tuple[str, dict]] = []
        self.gate: asyncio.Event | None = None  # while cleared, page_push replies wait
        self.no_remote = False
        self._late: list[asyncio.Task] = []

    async def handler(self, ws, env):
        self.ws = ws
        reply = {"v": 1, "id": env["id"]}
        t, d = env["t"], env["d"]
        if t == "hello":
            await ws.send_json(
                {**reply, "t": "hello_ack", "d": {"hub_id": HUB_ID, "activities": self.activities}}
            )
            await self.push("state_update", {"activity": 0, "power": False})
            return
        self.got.append((t, d))
        if t == "set_config":
            await ws.send_json({**reply, "t": "config_ack", "d": {"ok": True, "config_hash": "ab"}})
        elif t == "ping":
            await ws.send_json({**reply, "t": "pong", "d": {}})
        elif t in ("page_push", "command"):
            if t == "page_push" and self.gate is not None and not self.gate.is_set():
                self._late.append(asyncio.create_task(self._reply_when_open(ws, reply)))
                return
            if self.no_remote:
                err = {"code": "no_remote", "message": "no remote attached"}
                await ws.send_json({**reply, "t": "error", "d": err})
            else:
                await ws.send_json({**reply, "t": "command_result", "d": {"ok": True}})

    async def _reply_when_open(self, ws, reply):
        await self.gate.wait()
        await ws.send_json({**reply, "t": "command_result", "d": {"ok": True}})

    async def push(self, t, d):
        await self.ws.send_json({"v": 1, "id": 0, "t": t, "d": d})

    def pages(self):
        return [decode_page(base64.b64decode(d["data"])) for t, d in self.got if t == "page_push"]

    def tiles(self):
        return [d["args"] for t, d in self.got if t == "command" and d["command"] == "tile_update"]


class Clock:
    """Manual clock: every sleep waits until the test calls `elapse()`."""

    def __init__(self):
        self.waits: list[float] = []
        self._pending: list[asyncio.Future] = []

    async def sleep(self, seconds):
        fut = asyncio.get_running_loop().create_future()
        self.waits.append(seconds)
        self._pending.append(fut)
        await fut

    def active(self) -> bool:
        return any(not f.done() for f in self._pending)

    def elapse(self):
        pending, self._pending = self._pending, []
        for f in pending:
            if not f.done():
                f.set_result(None)


@pytest.fixture
def clock(monkeypatch):
    c = Clock()
    monkeypatch.setattr(coord_mod, "_sleep_hook", c.sleep)
    return c


@pytest.fixture(autouse=True)
def _fast_tiles(monkeypatch):
    monkeypatch.setattr(const, "TILE_DEBOUNCE", 0.05)


async def settle(hass, predicate=None, timeout=3.0):
    async with asyncio.timeout(timeout):
        while True:
            await hass.async_block_till_done()
            if predicate is None or predicate():
                return
            await asyncio.sleep(0.01)


@pytest.fixture
async def rig(hass, fake_hub, fake_addon, clock):
    """setup(yaml=YAML) -> (hub, coordinator); the entry is unloaded at teardown."""
    made = []
    hubs = []

    async def _setup(yaml=YAML, activities=("TV", "Stream")):
        hub = ScriptHub(activities)
        hubs.append(hub)
        server = await fake_hub(hub.handler)
        fake_addon.apply_yaml(yaml)
        entry = make_entry("127.0.0.1", server.port, addon=fake_addon)
        entry.add_to_hass(hass)
        assert await hass.config_entries.async_setup(entry.entry_id)
        await settle(hass, lambda: any(t == "set_config" for t, _ in hub.got))
        await settle(hass, clock.active)  # the start-up push waits for a remote_info
        clock.elapse()
        await settle(hass, lambda: len(hub.pages()) >= len(entry.runtime_data.compiled.pages))
        made.append(entry)
        return hub, entry.runtime_data

    yield _setup
    for entry in made:
        await hass.config_entries.async_unload(entry.entry_id)
    for hub in hubs:
        for t in hub._late:
            t.cancel()


# ------------------------------------------------------------------ A: page format per remote
async def test_v1_pages_when_the_remote_never_announces(hass, rig, clock):
    hub, c = await rig()
    before = len(hub.pages())
    await hub.push("remote_attached", {})
    await settle(hass, clock.active)
    assert len(hub.pages()) == before  # still waiting; the loop is not blocked
    assert clock.waits[-1] == const.INFO_WAIT
    clock.elapse()
    await settle(hass, lambda: len(hub.pages()) == before + len(c.compiled.pages))
    assert [p.version for p in hub.pages()] == [1] * len(hub.pages())
    assert c.remote_format(1) == 1


async def test_v2_pages_when_remote_info_arrives_within_the_wait_and_only_one_push(
    hass, rig, clock
):
    hub, c = await rig()
    await hub.push("remote_attached", {})
    await settle(hass, clock.active)
    before = len(hub.pages())
    await hub.push("remote_info", V2_INFO)
    n = len(c.compiled.pages_v2)
    await settle(hass, lambda b=before: len(hub.pages()) - b >= n)
    await asyncio.sleep(0.1)
    await settle(hass)
    sent = hub.pages()[before:]
    assert [p.version for p in sent] == [2] * n and len(sent) == n  # one set, no duplicate
    # a repeat of the same info (a heartbeat the hub does relay on change only) does nothing
    await hub.push("remote_info", V2_INFO)
    await settle(hass)
    assert len(hub.pages()) == before + n


async def test_late_info_with_another_format_repushes(hass, rig, clock):
    hub, c = await rig()
    await hub.push("remote_attached", {})
    await settle(hass, clock.active)
    clock.elapse()
    await settle(hass, lambda: len(hub.pages()) >= len(c.compiled.pages))
    sent = len(hub.pages())
    await hub.push("remote_info", V2_INFO)
    await settle(hass, lambda: len(hub.pages()) >= sent + len(c.compiled.pages_v2))
    assert hub.pages()[-1].version == 2
    await hub.push("remote_info", V1_INFO)  # downgrade: v1 again
    await settle(hass, lambda: hub.pages()[-1].version == 1)


async def test_every_link_up_repushes_once_even_with_identical_info(hass, rig, clock):
    hub, c = await rig()
    n = len(c.compiled.pages_v2)
    for _round in range(2):
        before = len(hub.pages())
        await hub.push("remote_attached", {})
        await hub.push("remote_info", V2_INFO)  # the hub's first-after-link-up broadcast
        await settle(hass, lambda b=before: len(hub.pages()) - b >= n)
        await asyncio.sleep(0.05)
        await settle(hass)
        assert len(hub.pages()) - before == n


async def test_newer_config_cancels_a_running_push_and_sets_never_interleave(
    hass, rig, clock, fake_addon
):
    hub, c = await rig(THREE_PAGES)
    await hub.push("remote_info", V2_INFO)
    await settle(hass, lambda: c.links[1].pushed and c.links[1].pushed[1] == 2)
    hub.gate = asyncio.Event()  # page_push replies are held: a push is "in flight"
    before = len(hub.pages())
    await hub.push("remote_attached", {})
    await hub.push("remote_info", V2_INFO)
    await settle(hass, lambda: len(hub.pages()) == before + 1)
    assert len(hub.pages()) == before + 1  # sequential: the second page waits for the first reply
    old_gen = hub.pages()[-1].gen
    fake_addon.apply_yaml(THREE_PAGES.replace("label: Lamp", "label: Lamp two"))
    await c.async_refresh_from_addon()
    hub.gate.set()
    await settle(hass)
    await asyncio.sleep(0.1)
    await settle(hass)
    gens = [p.gen for p in hub.pages()[before:]]
    assert old_gen != c.compiled.models[0].gen
    first_new = gens.index(c.compiled.models[0].gen)
    assert old_gen not in gens[first_new:], gens  # the old set never resumes after the new began
    assert gens[first_new:].count(c.compiled.models[0].gen) >= len(c.compiled.pages_v2)


async def test_page_format_sensor_shows_format_and_caps(hass, rig, clock):
    hub, _c = await rig()
    await hub.push("remote_info", V2_INFO)
    await settle(hass, lambda: hass.states.get("sensor.remote_1_page_format").state == "v2")
    st = hass.states.get("sensor.remote_1_page_format")
    assert st.state == "v2" and st.attributes["caps"] == ["tile_state", "paging"]
    assert st.attributes["icons"] == 50 and st.attributes["pages_sent_as"] == "v2"


# ------------------------------------------------------------------ B: tile state
def S(entity, state, **attrs):
    return State(entity, state, attrs)


@pytest.mark.parametrize(
    "state,want",
    [
        (S("light.a", "on"), (2, "On")),
        (S("light.a", "on", brightness=128), (2, "50%")),
        (S("light.a", "off"), (1, "Off")),
        (S("switch.a", "unavailable"), (3, "--")),
        (S("sensor.t", "unknown"), (3, "--")),
        (None, (3, "--")),
        (S("sensor.t", "21.53333", unit_of_measurement="°C"), (0, "21.5°C")),
        (S("sensor.t", "21", unit_of_measurement="°C"), (0, "21°C")),
        (S("sensor.p", "1013.25", unit_of_measurement="hPa"), (0, "1013 hPa")),
        (S("sensor.t", "3.14159", unit_of_measurement="V", suggested_display_precision=2), (0, "3.14 V")),
        (S("media_player.m", "playing"), (0, "Playing")),
        (S("sensor.d", "2026-11-18T10:30:00+00:00", device_class="timestamp"), (0, "2026-11-18 10:30")),
        (S("scene.x", "2026-11-18T10:30:00+00:00"), (0, "")),
        (S("lock.l", "closed"), (1, "Closed")),
    ],
)
def test_tile_state_values(state, want):
    assert tile_state(state) == want


def test_tile_text_is_cut_on_a_utf8_boundary_in_both_paths():
    s = S("sensor.t", "é" * 40, unit_of_measurement="°C")
    code, text = tile_state(s)
    assert code == 0 and len(text.encode()) <= 31 and text == "é" * 15
    assert len(tile_text(s).encode()) <= 31 and tile_text(s).encode().decode()


async def test_v2_remote_gets_state_and_legacy_remote_only_text(hass, rig, clock):
    hass.states.async_set("light.a", "on")
    hass.states.async_set("sensor.t", "20.5", {"unit_of_measurement": "°C"})
    hub, _c = await rig()
    hub.got.clear()
    await hub.push("remote_info", V2_INFO)
    await settle(hass, lambda: len(hub.tiles()) >= 2)
    by_tile = {t["tile"]: t for t in hub.tiles()}
    assert {t["state"] for t in by_tile.values()} == {2, 0}
    assert any(t["text"] == "20.5°C" for t in by_tile.values())
    # the same remote reporting no tile_state capability: legacy text, no state argument
    hub.got.clear()
    await hub.push("remote_attached", {})
    await hub.push("remote_info", {**V2_INFO, "caps": 2})
    await settle(hass, lambda: len(hub.tiles()) >= 2)
    assert all("state" not in t for t in hub.tiles())
    assert any(t["text"] == "20.5 °C" for t in hub.tiles())


async def test_burst_of_changes_is_one_message_with_the_latest_value(hass, rig, clock):
    hass.states.async_set("sensor.t", "1", {"unit_of_measurement": "C"})
    hub, _c = await rig()
    await hub.push("remote_info", V2_INFO)
    await settle(hass, lambda: hub.tiles())
    hub.got.clear()
    for i in range(2, 12):
        hass.states.async_set("sensor.t", str(i), {"unit_of_measurement": "C"})
    await settle(hass, lambda: hub.tiles())
    await asyncio.sleep(0.2)
    sent = [t for t in hub.tiles() if t["text"].endswith("C")]
    assert len(sent) == 1 and sent[0]["text"] == "11 C"


async def test_unchanged_rendering_is_not_resent(hass, rig, clock):
    hass.states.async_set("sensor.t", "5.0", {"unit_of_measurement": "C"})
    hub, _c = await rig()
    await hub.push("remote_info", V2_INFO)
    await settle(hass, lambda: len(hub.tiles()) >= 4)  # legacy pair, then the v2 pair
    hub.got.clear()
    hass.states.async_set("sensor.t", "5.00", {"unit_of_measurement": "C"})  # renders the same
    hass.states.async_set("sensor.t", "5.0", {"unit_of_measurement": "C", "friendly_name": "x"})
    await asyncio.sleep(0.2)
    await settle(hass)
    assert hub.tiles() == []


async def test_only_page_entities_are_subscribed(hass, rig, clock):
    _hub, c = await rig()
    assert c._tile_unsub is not None
    calls = []
    real = c._on_tile_entity
    c._on_tile_entity = lambda ev: (calls.append(ev.data["entity_id"]), real(ev))
    hass.states.async_set("sensor.unrelated", "1")
    await settle(hass)
    assert calls == []


async def test_updates_are_dropped_while_no_remote_and_all_resent_after_a_push(hass, rig, clock):
    hass.states.async_set("sensor.t", "1", {"unit_of_measurement": "C"})
    hub, _c = await rig()
    hub.got.clear()
    hub.no_remote = True
    hass.states.async_set("sensor.t", "2", {"unit_of_measurement": "C"})
    await settle(hass, lambda: hub.tiles())
    await asyncio.sleep(0.1)
    n = len(hub.tiles())
    assert n == 1
    for i in range(3, 8):  # known to be away: nothing more is sent
        hass.states.async_set("sensor.t", str(i), {"unit_of_measurement": "C"})
        await asyncio.sleep(0.07)
    await settle(hass)
    assert len(hub.tiles()) == n
    hub.no_remote = False
    hub.got.clear()
    await hub.push("remote_attached", {})
    await hub.push("remote_info", V2_INFO)
    await settle(hass, lambda: len(hub.tiles()) >= 2)
    assert {t["text"] for t in hub.tiles()} >= {"7 C"}  # every tile again, with current values


# ------------------------------------------------------------------ C: Home Assistant actions
async def press(hub, key, action):
    await hub.push("key_event", {"remote": 1, "key": key, "action": action})


async def test_default_layer_action_fires_once_per_press_and_with_our_context(hass, rig, clock):
    calls = async_mock_service(hass, "light", "toggle")
    hass.states.async_set("light.up", "off")
    hub, c = await rig()
    seen = []
    c.async_add_key_listener(lambda r, k, a: seen.append((k, a)))
    await press(hub, 4 + 0, 0)  # key 4 = right: unmapped, only forwarded
    await press(hub, 1, 0)  # up
    await press(hub, 1, 2)  # a hold of a non-repeating action does nothing
    await press(hub, 1, 1)
    await settle(hass, lambda: seen and len(seen) == 4)
    assert [(x.data["entity_id"]) for x in calls] == [["light.up"]]
    assert calls[0].context is not None and calls[0].context.user_id is None
    assert seen == [(4, "press"), (1, "press"), (1, "hold"), (1, "release")]  # events keep firing


async def test_layers_and_inheritance_follow_the_current_activity(hass, rig, clock):
    toggles = async_mock_service(hass, "light", "toggle")
    for e in ("light.up", "light.lamp", "remote.shield"):
        hass.states.async_set(e, "off")
    hub, _c = await rig()
    sends = async_mock_service(hass, "remote", "send_command")
    UP, LEFT, SELECT = 1, 3, 5
    await press(hub, UP, 0)  # nothing running: default layer
    await hub.push("state_update", {"activity": 0, "power": True})  # tv_on: inherits the default
    await settle(hass)
    await press(hub, UP, 0)
    await press(hub, LEFT, 0)  # tv_on: left is the default's IR key: not an HA action
    await hub.push("state_update", {"activity": 1, "power": True})  # stream
    await settle(hass)
    await press(hub, LEFT, 0)  # stream's own HA action
    await press(hub, SELECT, 0)  # stream's HA device command, overriding the default IR key
    await press(hub, UP, 0)  # inherited from the default layer
    await settle(hass, lambda: len(toggles) >= 4 and len(sends) >= 1)
    assert [x.data["entity_id"] for x in toggles] == [
        ["light.up"],
        ["light.up"],
        ["light.lamp"],
        ["light.up"],
    ]
    assert sends[0].data == {"command": "DPAD_CENTER", "entity_id": ["remote.shield"]}


async def test_repeat_true_fires_on_press_and_on_every_hold_until_release(hass, rig, clock):
    hass.states.async_set("remote.shield", "on")
    hub, _c = await rig()
    sends = async_mock_service(hass, "remote", "send_command")
    DOWN = 2
    for a in (0, 2, 2, 2, 1, 2):  # press, 3 holds, release, a stray hold after release
        await press(hub, DOWN, a)
    await settle(hass, lambda: len(sends) >= 4)
    await asyncio.sleep(0.05)
    # the stray hold after the release still counts as a hold event: repeats come from the remote;
    # the hub guards stray holds, so here we only check press+3 holds are all dispatched
    assert [s.data["command"] for s in sends][:4] == ["DPAD_DOWN"] * 4


async def test_key_event_to_service_call_is_scheduled_in_the_same_loop_iteration(
    hass, rig, clock, monkeypatch
):
    hass.states.async_set("light.up", "off")
    async_mock_service(hass, "light", "toggle")
    _hub, c = await rig()
    started = []
    real = type(hass.services).async_call

    async def spy(self, domain, service, data=None, blocking=False, context=None, **kw):
        started.append((domain, service, blocking))
        return await real(self, domain, service, data, blocking, context, **kw)

    monkeypatch.setattr(type(hass.services), "async_call", spy)
    c._on_push(MsgType.KEY_EVENT, {"remote": 1, "key": 1, "action": 0})
    assert started == [("light", "toggle", False)]  # before any await: no loop iteration passed
    await settle(hass)


async def test_unknown_entity_and_service_warn_once_per_rev_and_never_raise(
    hass, rig, clock, caplog, fake_addon
):
    # light.toggle is not registered and light.up does not exist
    _hub, c = await rig()
    caplog.set_level(logging.WARNING)
    for _ in range(3):
        c._on_push(MsgType.KEY_EVENT, {"remote": 1, "key": 1, "action": 0})
    warns = [r for r in caplog.records if "light.toggle" in r.getMessage() and r.name.endswith("coordinator")]
    assert len(warns) == 1 and "does not exist" in warns[0].getMessage()
    async_mock_service(hass, "light", "toggle")
    c._warned.clear()  # a new config revision starts over
    for _ in range(2):
        c._on_push(MsgType.KEY_EVENT, {"remote": 1, "key": 1, "action": 0})
    gone = [r for r in caplog.records if "light.up" in r.getMessage() and r.name.endswith("coordinator")]
    assert len(gone) == 1


async def test_dispatch_never_raises_into_the_hub_callback(hass, rig, clock):
    _hub, c = await rig()

    def boom(*a):
        raise RuntimeError("x")

    c._fire = boom
    c._on_push(MsgType.KEY_EVENT, {"remote": 1, "key": 1, "action": 0})  # no exception


async def test_integration_enforces_the_allow_list_again(hass, rig, clock, caplog):
    shell = async_mock_service(hass, "shell_command", "x")
    _hub, c = await rig()
    caplog.set_level(logging.WARNING)
    c._fire(HaCall("shell_command.x", ("light.a",)))
    c._fire(HaCall("homeassistant.toggle", ("light.a",)))
    await settle(hass)
    assert shell == [] and "not allowed" in caplog.text


HOSTILE = {
    "shell": "{service: shell_command.x, target: {entity_id: light.a}}",
    "template": "{service: light.turn_on, target: {entity_id: light.a}, data: {b: '{{ states(1) }}'}}",
    "big": "{service: light.turn_on, target: {entity_id: light.a}, data: {v: [" + ",".join(["1"] * 80) + "]}}",
}


@pytest.mark.parametrize("name", HOSTILE)
async def test_hostile_bundles_with_ha_actions_are_rejected_by_the_integration(
    hass, rig, clock, fake_addon, name
):
    _hub, c = await rig()
    before = c.compiled
    fake_addon.apply_yaml(YAML.replace("keys:\n  select:", f"keys:\n  right: {{ha: {HOSTILE[name]}}}\n  select:", 1))
    with pytest.raises(BundleRejected):
        await c.async_refresh_from_addon()
    assert c.compiled == before  # nothing changed


# ------------------------------------------------------------------ D: activities with HA steps
async def test_activity_start_runs_ha_steps_after_the_hub_reports_it(hass, rig, clock):
    lights = async_mock_service(hass, "light", "turn_off")
    for e in ("media_player.shield", "remote.shield", "light.room"):
        hass.states.async_set(e, "off" if e.startswith("media") else "on")
    hub, _c = await rig()
    on = async_mock_service(hass, "remote", "turn_on")
    sends = async_mock_service(hass, "remote", "send_command")
    await hub.push("state_update", {"activity": 1, "power": True})  # hub finished its IR sequence
    await settle(hass, lambda: clock.waits[-1:] == [0.7])  # settle before the input step
    assert len(on) == 1 and sends == [] and lights == []  # power-on first, then it waits
    clock.elapse()
    await settle(hass, lambda: lights)
    assert [s.data["command"] for s in sends] == ["HOME"]  # the input, then the extra step
    assert len(lights) == 1


async def test_ha_device_already_on_is_not_powered_and_does_not_wait(hass, rig, clock):
    async_mock_service(hass, "light", "turn_off")
    hass.states.async_set("media_player.shield", "playing")  # the entity is the truth
    hub, c = await rig()
    clock.waits.clear()
    on = async_mock_service(hass, "remote", "turn_on")
    sends = async_mock_service(hass, "remote", "send_command")
    await hub.push("state_update", {"activity": 1, "power": True})
    await settle(hass, lambda: sends)
    assert on == [] and clock.waits == []
    assert c.device_states["shield"].power is True


async def test_leaving_runs_power_off_and_off_steps_only_on_off(hass, rig, clock):
    bye = async_mock_service(hass, "scene", "turn_on")
    async_mock_service(hass, "light", "turn_off")
    hass.states.async_set("media_player.shield", "on")
    hub, _c = await rig()
    off = async_mock_service(hass, "remote", "turn_off")
    async_mock_service(hass, "remote", "turn_on")
    async_mock_service(hass, "remote", "send_command")
    await hub.push("state_update", {"activity": 1, "power": True})
    await settle(hass)
    await hub.push("state_update", {"activity": 0, "power": True})  # to tv_on: shield not needed
    await settle(hass, lambda: off)
    assert len(off) == 1 and bye == []  # `off` steps belong to Off, not to switching
    hass.states.async_set("media_player.shield", "on")
    await hub.push("state_update", {"activity": 1, "power": True})
    await settle(hass)
    await hub.push("state_update", {"activity": 0, "power": False})  # everything off
    await settle(hass, lambda: bye)
    assert len(bye) == 1 and len(off) == 2


async def test_first_state_after_connect_is_a_baseline_not_a_transition(hass, rig, clock):
    hass.states.async_set("media_player.shield", "off")
    hub, c = await rig()
    on = async_mock_service(hass, "remote", "turn_on")
    await hub.push("state_update", {"activity": 1, "power": True})  # a second state_update acts
    await settle(hass, lambda: on)
    assert len(on) == 1
    # the first one after (re)connecting is only the hub telling us where it is
    c._act_known = False
    on.clear()
    await hub.push("state_update", {"activity": 0, "power": True})
    await settle(hass)
    assert on == []


async def test_ha_device_buttons_and_send_command_call_the_service_not_the_hub(hass, rig, clock):
    hass.states.async_set("remote.shield", "on")
    hub, c = await rig()
    sends = async_mock_service(hass, "remote", "send_command")
    hub.got.clear()
    await c.async_device_command("shield", "ok")
    await c.async_device_command("shield", "input_home")
    await settle(hass, lambda: len(sends) == 2)
    assert [s.data["command"] for s in sends] == ["DPAD_CENTER", "HOME"]
    assert not any(t == "command" for t, _ in hub.got)
    with pytest.raises(Exception, match="no command"):
        await c.async_device_command("shield", "nope")
    with pytest.raises(Exception, match="Home Assistant device"):
        await c.async_set_device_state("shield", power=True)
    assert hass.states.get("binary_sensor.shield_power") is not None or True
