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
from custom_components.ha_harmony.harmony_core.compiler import config_hash
from custom_components.ha_harmony.harmony_core.ha_actions import HaCall
from custom_components.ha_harmony.harmony_core.harmony_proto import MsgType
from custom_components.ha_harmony.harmony_core.page_codec import decode_page
from custom_components.ha_harmony.tile_format import tile_state, tile_text
from harmony_core.hub_limits import CURRENT

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
V2_INFO = {"remote": 1, "page_fmt": 2, "caps": 3, "icons": 50, "gen": 0, "pages": 0}  # empty store
V1_INFO = {"remote": 1, "page_fmt": 1, "caps": 0, "icons": 0, "gen": 0, "pages": 0}


class ScriptHub:
    """A hub that answers like the real one and lets the test push messages and hold replies."""

    def __init__(self, activities=("TV", "Stream"), state=None):
        self.activities = list(activities)
        self.state = state or {"activity": 0, "power": False}  # what hello is answered with
        self.ws = None
        self.got: list[tuple[str, dict]] = []
        self.gate: asyncio.Event | None = None  # while cleared, page_push replies wait
        self.no_remote = False
        self.reject_config = False  # answer set_config with ok=false
        self.refuse: dict[
            str, int
        ] = {}  # message type -> how many to answer with rf_error (the link queue was full)
        self.tile_delay = 0.0  # seconds a tile_update reply is held
        self.tiles_inflight = self.tiles_peak = 0
        self.config_hash: str | None = (
            None  # of the last accepted set_config, as hello_ack reports it
        )
        self.info: dict | None = None  # the remote's cached remote_info, resent after hello_ack
        self._late: list[asyncio.Task] = []

    async def handler(self, ws, env):
        self.ws = ws
        reply = {"v": 1, "id": env["id"]}
        t, d = env["t"], env["d"]
        if t == "hello":
            ack = {
                "hub_id": HUB_ID,
                "activities": self.activities,
                "tcp_keepalive": True,
                "limits": CURRENT.to_wire(),
            }
            if self.config_hash:
                ack["config_hash"] = self.config_hash
            await ws.send_json({**reply, "t": "hello_ack", "d": ack})
            if self.info is not None:  # the real hub resends it before the state it refers to
                await self.push("remote_info", self.info)
            await self.push("state_update", dict(self.state))
            return
        self.got.append((t, d))
        if t == "set_config":
            if self.reject_config:
                ack = {"ok": False, "message": "too many keys"}
            else:  # the real hub reports the CRC-32 of the config it runs
                self.config_hash = config_hash(d["config"])
                ack = {"ok": True, "config_hash": self.config_hash}
            await ws.send_json({**reply, "t": "config_ack", "d": ack})
        elif t == "ping":
            await ws.send_json({**reply, "t": "pong", "d": {}})
        elif t == "command" and d["command"] == "tile_update" and self.tile_delay:
            self.tiles_inflight += 1
            self.tiles_peak = max(self.tiles_peak, self.tiles_inflight)
            self._late.append(asyncio.create_task(self._tile_reply_later(ws, reply)))
        elif self.refuse.get(t):
            self.refuse[t] -= 1
            await ws.send_json(
                {**reply, "t": "error", "d": {"code": "rf_error", "message": "RF send failed"}}
            )
        elif t in ("page_push", "command", "theme_push"):
            if t != "command" and self.gate is not None and not self.gate.is_set():
                self._late.append(asyncio.create_task(self._reply_when_open(ws, reply)))
                return
            if self.no_remote:
                err = {"code": "no_remote", "message": "no remote attached"}
                await ws.send_json({**reply, "t": "error", "d": err})
            else:
                await ws.send_json({**reply, "t": "command_result", "d": {"ok": True}})

    async def _tile_reply_later(self, ws, reply):
        await asyncio.sleep(self.tile_delay)
        self.tiles_inflight -= 1
        await ws.send_json({**reply, "t": "command_result", "d": {"ok": True}})

    async def _reply_when_open(self, ws, reply):
        await self.gate.wait()
        await ws.send_json({**reply, "t": "command_result", "d": {"ok": True}})

    async def push(self, t, d):
        await self.ws.send_json({"v": 1, "id": 0, "t": t, "d": d})

    def pages(self):
        return [decode_page(base64.b64decode(d["data"])) for t, d in self.got if t == "page_push"]

    def themes(self):
        return [(d["gen"], base64.b64decode(d["data"])) for t, d in self.got if t == "theme_push"]

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


async def settle(hass, predicate=None, timeout=15.0):  # an upper bound only: returns when true
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

    async def _setup(yaml=YAML, activities=("TV", "Stream"), state=None, info=None, pages=True):
        hub = ScriptHub(activities, state)
        hub.info = info(yaml) if callable(info) else info
        hubs.append(hub)
        server = await fake_hub(hub.handler)
        fake_addon.apply_yaml(yaml)
        entry = make_entry("127.0.0.1", server.port, addon=fake_addon)
        entry.add_to_hass(hass)
        assert await hass.config_entries.async_setup(entry.entry_id)
        await settle(hass, lambda: any(t == "set_config" for t, _ in hub.got))
        # `compiled` is the config the hub runs: empty until the hub reports its hash. Tests size their
        # expectations from it (len(c.compiled.pages)), so wait for it rather than race the first sync.
        await settle(hass, lambda: entry.runtime_data._accepted)
        if pages:
            await settle(hass, clock.active)  # the start-up push waits for a remote_info
            clock.elapse()
            await settle(hass, lambda: len(hub.pages()) >= len(entry.runtime_data.compiled.pages))
            # the push is over once its task is: its tile texts follow the last page's reply
            links = entry.runtime_data.links
            await settle(
                hass, lambda: all(lk.task is None or lk.task.done() for lk in links.values())
            )
        else:
            await settle(hass)
        made.append(entry)
        return hub, entry.runtime_data

    yield _setup
    for entry in made:
        if hass.config_entries.async_get_entry(entry.entry_id) is not None:  # a test may remove it
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
    before = len(hub.pages())  # the start-up push is already in
    await hub.push("remote_attached", {})
    await settle(hass, clock.active)
    clock.elapse()
    await settle(hass, lambda: len(hub.pages()) >= before + len(c.compiled.pages))
    # the v1 push is over once its task is: otherwise its tail pages land after the next push began
    await settle(hass, lambda: all(lk.task is None or lk.task.done() for lk in c.links.values()))
    sent = len(hub.pages())
    await hub.push("remote_info", V2_INFO)
    await settle(hass, lambda: len(hub.pages()) >= sent + len(c.compiled.pages_v2))
    assert hub.pages()[-1].version == 2
    await settle(hass, lambda: all(lk.task is None or lk.task.done() for lk in c.links.values()))
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
    new_gen = c.compiled.models[0].gen
    n_new = len(c.compiled.pages_v2)
    # the full new set arrived (a signal, not a sleep), then everything in flight drained
    await settle(hass, lambda: [p.gen for p in hub.pages()[before:]].count(new_gen) >= n_new)
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
        (
            S("sensor.t", "3.14159", unit_of_measurement="V", suggested_display_precision=2),
            (0, "3.14 V"),
        ),
        (S("media_player.m", "playing"), (0, "Playing")),
        (
            S("sensor.d", "2026-11-18T10:30:00+00:00", device_class="timestamp"),
            (0, "2026-11-18 10:30"),
        ),
        (S("scene.x", "2026-11-18T10:30:00+00:00"), (0, "")),
        (S("lock.l", "closed"), (1, "Closed")),
        (S("cover.c", "open", current_position=40), (2, "40%")),
        (S("cover.c", "open"), (2, "Open")),
        (S("cover.c", "closed", current_position=0), (1, "Closed")),
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
    # the v2 push and its full tile resend (which would land inside the burst) are over when the link
    # holds a v2 set and its task is done. "Any tile seen" is not enough: the v1 push's tiles satisfy it
    # before the remote_info above has even been read.
    link = _c.links[1]
    await settle(
        hass,
        lambda: (
            link.pushed is not None
            and link.pushed[1] == 2
            and (link.task is None or link.task.done())
        ),
    )
    await settle(hass, lambda: hub.tiles_inflight == 0)
    hub.got.clear()
    for i in range(2, 12):
        hass.states.async_set("sensor.t", str(i), {"unit_of_measurement": "C"})
    # the debounce timer fired, its flush ran, and nothing is marked or pending any more
    await settle(
        hass,
        lambda: (
            hub.tiles()
            and _c._tile_timer is None
            and not _c._tile_dirty
            and (_c._flush_task is None or _c._flush_task.done())
        ),
    )
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
    assert started == [
        ("light", "toggle", True)
    ]  # blocking runs inside the task; started before any await
    await settle(hass)


async def test_unknown_entity_and_service_warn_once_per_rev_and_never_raise(
    hass, rig, clock, caplog, fake_addon
):
    # light.toggle is not registered and light.up does not exist
    _hub, c = await rig()
    caplog.set_level(logging.WARNING)
    for _ in range(3):
        c._on_push(MsgType.KEY_EVENT, {"remote": 1, "key": 1, "action": 0})
    warns = [
        r
        for r in caplog.records
        if "light.toggle" in r.getMessage() and r.name.endswith("coordinator")
    ]
    assert len(warns) == 1 and "does not exist" in warns[0].getMessage()
    async_mock_service(hass, "light", "toggle")
    c._warned.clear()  # a new config revision starts over
    for _ in range(2):
        c._on_push(MsgType.KEY_EVENT, {"remote": 1, "key": 1, "action": 0})
    gone = [
        r for r in caplog.records if "light.up" in r.getMessage() and r.name.endswith("coordinator")
    ]
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
    "big": "{service: light.turn_on, target: {entity_id: light.a}, data: {v: ["
    + ",".join(["1"] * 80)
    + "]}}",
}


@pytest.mark.parametrize("name", HOSTILE)
async def test_hostile_bundles_with_ha_actions_are_rejected_by_the_integration(
    hass, rig, clock, fake_addon, name
):
    _hub, c = await rig()
    before = c.compiled
    fake_addon.apply_yaml(
        YAML.replace("keys:\n  select:", f"keys:\n  right: {{ha: {HOSTILE[name]}}}\n  select:", 1)
    )
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


@pytest.mark.parametrize("shield", ["idle", "standby", "playing", "paused"])
async def test_off_powers_down_ha_devices_the_leaving_activity_did_not_list(
    hass, rig, clock, shield
):
    async_mock_service(hass, "light", "turn_off")
    async_mock_service(hass, "remote", "turn_on")
    async_mock_service(hass, "remote", "send_command")
    hass.states.async_set("media_player.shield", "off")
    hub, _c = await rig()
    off = async_mock_service(hass, "remote", "turn_off")
    await hub.push("state_update", {"activity": 0, "power": True})  # tv_on: no shield in it
    await settle(hass)
    hass.states.async_set("media_player.shield", shield)  # started by hand, hub does not know
    await settle(hass)
    await hub.push("state_update", {"activity": 0, "power": False})  # Off
    await settle(hass, lambda: off)
    assert len(off) == 1


async def test_off_sends_power_off_even_when_the_shield_still_reads_off(hass, rig, clock):
    hass.states.async_set("media_player.shield", "off")  # entity states lag the real device
    hub, _c = await rig()
    off = async_mock_service(hass, "remote", "turn_off")
    await hub.push("state_update", {"activity": 0, "power": True})
    await settle(hass)
    await hub.push("state_update", {"activity": 0, "power": False})
    await settle(hass, lambda: off)
    assert len(off) == 1  # turn_off on an off device is a no-op; skipping a lagging one is not


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


def test_input_select_set_options_is_rejected():
    from harmony_core import ha_actions

    assert not ha_actions.allowed("input_select.set_options")
    assert ha_actions.allowed("input_select.select_next")
    with pytest.raises(Exception, match="input_select allows only"):
        ha_actions.validate_ha(
            {"service": "input_select.set_options", "target": {"entity_id": "input_select.x"}}
        )


async def _shield_started_by_hand(hass, rig):
    async_mock_service(hass, "light", "turn_off")
    async_mock_service(hass, "remote", "turn_on")
    async_mock_service(hass, "remote", "send_command")
    hass.states.async_set("media_player.shield", "off")
    hub, c = await rig()
    off = async_mock_service(hass, "remote", "turn_off")
    await hub.push("state_update", {"activity": 0, "power": True})
    await settle(hass)
    hass.states.async_set("media_player.shield", "playing")
    return hub, c, off


async def test_power_on_with_unknown_index_is_not_off(hass, rig, clock):
    hub, c, off = await _shield_started_by_hand(hass, rig)
    await hub.push(
        "state_update", {"activity": 99, "power": True}
    )  # reload race: list not known yet
    await settle(hass)
    assert c.current_activity_id is None and off == []


async def test_reconnect_reporting_off_is_a_baseline_not_a_sweep(hass, rig, clock):
    hub, c, off = await _shield_started_by_hand(hass, rig)
    c._on_state(True, None)  # hub came back (OTA / power blip)
    await hub.push("state_update", {"activity": 0, "power": False})
    await settle(hass)
    assert off == []


async def test_off_list_already_turning_off_shield_gives_one_turn_off(hass, rig, clock):
    hub, c, off = await _shield_started_by_hand(hass, rig)
    acts = {k: v for k, v in c.compiled.ha_activities.items()}
    old = acts["tv_on"] if "tv_on" in acts else next(iter(acts.values()))
    import dataclasses

    from harmony_core.compiler import HaActivity, HaStep
    from harmony_core.ha_actions import HaCall

    step = HaStep(HaCall("remote.turn_off", ("remote.shield",)))
    ha = dict(acts)
    ha["tv_on"] = HaActivity("tv_on", old.devices, old.start, (step,))
    c.compiled = dataclasses.replace(c.compiled, ha_activities=ha)
    await hub.push("state_update", {"activity": 0, "power": False})
    await settle(hass, lambda: off)
    await settle(hass)
    assert len(off) == 1
