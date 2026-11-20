"""The integration's Bluetooth support against a scripted hub (no hub binary): state and gen ordering, entities, services,
remote-key events, diagnostics, rename, and the editor relay over the add-on channel (a fake add-on, fake_channel.py)."""

import asyncio
import json

import pytest
import voluptuous as vol
from conftest import HUB_ID, make_entry
from homeassistant.exceptions import HomeAssistantError, ServiceValidationError
from homeassistant.helpers import entity_registry as er
from pytest_homeassistant_custom_component.common import async_capture_events

import custom_components.ha_harmony  # noqa: F401  (before the HA plugin shadows it)
from custom_components.ha_harmony import const, rename
from custom_components.ha_harmony.bluetooth import BtError
from custom_components.ha_harmony.const import DOMAIN
from custom_components.ha_harmony.diagnostics import async_get_config_entry_diagnostics

pytestmark = pytest.mark.usefixtures("enable_custom_integrations", "fast_client")

LIMITS = {"hosts_max": 4, "conn_max": 2, "remotes_max": 2, "le_bonds_max": 8, "pages": [7, 12], "le": True}
TV, RC, STRAY = "AA:BB:CC:DD:EE:01", "11:22:33:44:55:01", "AA:BB:CC:DD:EE:99"
CFG = f"""
devices:
  - id: shield
    kind: bt
    name: Shield
    address: "{TV}"
    commands: {{Home: Home}}
bt_remotes:
  - name: Homatics
    address: "{RC}"
"""
CFG2 = f"""
devices:
  - id: box
    kind: bt
    name: Box
    address: "AA:BB:CC:DD:EE:02"
    commands: {{Home: Home}}
  - id: shield
    kind: bt
    name: Shield
    address: "{TV}"
    commands: {{Home: Home}}
"""


def state(gen, **kw):
    base = {
        "state": "ready",
        "discoverable": False,
        "le": True,
        "gen": gen,
        "hosts": [
            {"slot": 0, "addr": TV, "name": "Shield", "transport": "le", "connected": True, "configured": True},
            {"slot": 1, "addr": STRAY, "name": "Stray", "transport": "classic", "connected": True, "configured": False},
        ],
        "remotes": [{"slot": 0, "addr": RC, "name": "Homatics", "connected": False, "battery": 55, "configured": True}],
    }
    return {**base, **kw}


class BtHub:
    """The parts of a hub the Bluetooth code talks to, scripted."""

    def __init__(self, bt=LIMITS, first=None):
        self.bt, self.state, self.ws = bt, first or state(1), None
        self.log: list[tuple[str, dict]] = []
        self.fail: dict[str, str] = {}
        self.hold: asyncio.Event | None = None  # bt_pair is answered only once this is set

    async def handler(self, ws, env):
        t, i, d = env["t"], env["id"], env.get("d", {})
        self.ws = ws
        if t == "hello":
            ack = {"hub_id": HUB_ID, "activities": [], "tcp_keepalive": True, **({"bt": self.bt} if self.bt else {})}
            await ws.send_json({"v": 1, "id": i, "t": "hello_ack", "d": ack})
            if self.bt:
                await self.push("bt_state", self.state)
        elif t == "set_config":
            await ws.send_json({"v": 1, "id": i, "t": "config_ack", "d": {"ok": True}})
        elif t == "ping":
            await ws.send_json({"v": 1, "id": i, "t": "pong", "d": {}})
        elif t.startswith("bt_"):
            self.log.append((t, d))
            if t == "bt_pair" and self.hold is not None:
                asyncio.ensure_future(self._after_hold(ws, i))
            elif t in self.fail:
                err = {"code": self.fail[t], "message": "refused"}
                await ws.send_json({"v": 1, "id": i, "t": "error", "d": err})
            elif t in ("bt_pair", "bt_forget"):
                await ws.send_json({"v": 1, "id": i, "t": "bt_state", "d": self.state})
            else:
                await ws.send_json({"v": 1, "id": i, "t": "command_result", "d": {"ok": True}})

    async def _after_hold(self, ws, i):
        await self.hold.wait()
        await ws.send_json({"v": 1, "id": i, "t": "bt_state", "d": self.state})

    async def push(self, t, d):
        await self.ws.send_json({"v": 1, "id": 0, "t": t, "d": d})

    def sent(self, t):
        return [d for n, d in self.log if n == t]


async def until(hass, pred, timeout=8.0):
    async with asyncio.timeout(timeout):
        while not pred():
            await asyncio.sleep(0.02)
            await hass.async_block_till_done()


@pytest.fixture(autouse=True)
async def _unload_before_the_servers_close(hass, fake_hub, fake_addon):
    """Depends on the servers, so it is torn down first: a client still connected makes their shutdown wait."""
    yield
    for entry in hass.config_entries.async_entries(DOMAIN):
        await hass.config_entries.async_unload(entry.entry_id)


async def boot(hass, fake_hub, fake_addon, bt_hub, yaml=CFG):
    hub = await fake_hub(bt_hub.handler)
    if yaml is not None:
        fake_addon.apply_yaml(yaml)
    entry = make_entry("127.0.0.1", hub.port, addon=fake_addon)
    entry.add_to_hass(hass)
    assert await hass.config_entries.async_setup(entry.entry_id)
    c = entry.runtime_data
    await until(hass, lambda: c.config_state == "accepted" and c.link_state == "ok")
    await hass.async_block_till_done()
    return entry, c, hub


def uids(hass, entry):
    return {e.unique_id: e for e in er.async_entries_for_config_entry(er.async_get(hass), entry.entry_id)}


def peer_id(kind, addr):
    return f"{HUB_ID}_btconn_{kind}_{addr.replace(':', '').lower()}"


async def call(hass, service, **data):
    await hass.services.async_call(DOMAIN, service, data, blocking=True)



# ------------------------------------------------------------------ a hub without Bluetooth
async def test_hub_without_bluetooth_has_nothing_and_says_so(hass, fake_hub, fake_addon):
    bt = BtHub(bt=None)
    entry, c, _ = await boot(hass, fake_hub, fake_addon, bt, yaml="devices: []\n")
    assert not [u for u in uids(hass, entry) if "_bt" in u]
    assert not c.btm.supported and c.bt_state is None
    for service, data in (
        ("bt_pair", {}),
        ("bt_stop_pairing", {}),
        ("bt_forget", {"address": TV}),
        ("bt_listen", {}),
    ):
        with pytest.raises(HomeAssistantError) as exc:
            await call(hass, service, **data)
        assert exc.value.translation_key == "bt_unsupported"
    assert bt.log == []  # nothing was sent to a hub that cannot answer
    assert (await fake_addon.frame("bt_state"))["supported"] is False
    diag = await async_get_config_entry_diagnostics(hass, entry)
    assert diag["bluetooth"]["supported"] is False and diag["bluetooth"]["state"] is None


async def test_bluetooth_config_on_a_hub_without_it_is_refused_and_makes_no_entities(hass, fake_hub, fake_addon):
    hub = await fake_hub(BtHub(bt=None).handler)
    fake_addon.apply_yaml(CFG)
    entry = make_entry("127.0.0.1", hub.port, addon=fake_addon)
    entry.add_to_hass(hass)
    assert await hass.config_entries.async_setup(entry.entry_id)
    c = entry.runtime_data
    await until(hass, lambda: c.config_state == "rejected")
    assert "Bluetooth" in c.last_error and not [u for u in uids(hass, entry) if "_bt" in u]


# ------------------------------------------------------------------ state and entities
async def test_entities_for_configured_peers_only(hass, fake_hub, fake_addon):
    entry, _c, _ = await boot(hass, fake_hub, fake_addon, BtHub())
    ids = uids(hass, entry)
    bt_ids = {u for u in ids if "_bt" in u}
    assert bt_ids == {f"{HUB_ID}_bt", peer_id("host", TV), peer_id("remote", RC)}  # the stray bond has none
    helper = hass.states.get(ids[f"{HUB_ID}_bt"].entity_id)
    assert helper.state == "ready" and helper.attributes["limits"]["hosts_max"] == 4
    assert helper.attributes["le"] is True and helper.attributes["window"] is None
    assert ids[f"{HUB_ID}_bt"].entity_category.value == "diagnostic"
    host = hass.states.get(ids[peer_id("host", TV)].entity_id)
    assert host.state == "on" and host.attributes["address"] == TV and host.attributes["slot"] == 0
    assert host.attributes["device_class"] == "connectivity" and host.attributes["transport"] == "le"
    remote = hass.states.get(ids[peer_id("remote", RC)].entity_id)
    assert remote.state == "off" and remote.attributes["battery"] == 55


async def test_pushes_update_entities_and_old_gen_is_ignored(hass, fake_hub, fake_addon):
    bt = BtHub()
    entry, c, _ = await boot(hass, fake_hub, fake_addon, bt)
    ids = uids(hass, entry)
    helper, rem = ids[f"{HUB_ID}_bt"].entity_id, ids[peer_id("remote", RC)].entity_id
    new = state(5, window="remote", remotes=[{"slot": 0, "addr": RC, "connected": True, "configured": True}])
    await bt.push("bt_state", new)
    await until(hass, lambda: hass.states.get(rem).state == "on")
    assert hass.states.get(helper).attributes["window"] == "remote" and "battery" not in hass.states.get(rem).attributes
    await bt.push("bt_state", state(4, state="disabled"))  # older: dropped
    await bt.push("bt_state", state(5, state="starting"))  # same gen: accepted
    await until(hass, lambda: hass.states.get(helper).state == "starting")
    assert c.btm.gen == 5
    await bt.push("bt_state", state(0xFFFFFFFF, state="off"))  # a jump back-or-forward by half the range: older than 5
    await asyncio.sleep(0.1)
    assert hass.states.get(helper).state == "starting"
    await bt.push("bt_state", {"state": "bogus"})
    await bt.push("bt_state", state(6, state="no_adapter"))
    await until(hass, lambda: hass.states.get(helper).state == "no_adapter")


async def test_reconnect_forgets_the_old_generation(hass, fake_hub, fake_addon):
    bt = BtHub(first=state(900))
    _entry, c, hub = await boot(hass, fake_hub, fake_addon, bt)
    assert c.btm.gen == 900
    bt.state = state(1, state="starting")  # a restarted hub counts from the start again
    await bt.ws.close()
    await until(hass, lambda: c.btm.gen == 1 and c.btm.state["state"] == "starting")
    assert hub.connections == 2


async def test_entities_follow_the_config(hass, fake_hub, fake_addon):
    bt = BtHub()
    entry, _c, _ = await boot(hass, fake_hub, fake_addon, bt)
    assert peer_id("remote", RC) in uids(hass, entry)
    fake_addon.apply_yaml(CFG2)  # the remote is gone, a second host is new
    await until(hass, lambda: peer_id("host", "AA:BB:CC:DD:EE:02") in uids(hass, entry))
    ids = uids(hass, entry)
    assert peer_id("remote", RC) not in ids and peer_id("host", TV) in ids
    box = hass.states.get(ids[peer_id("host", "AA:BB:CC:DD:EE:02")].entity_id)
    assert box.state == "off" and box.attributes["bonded"] is False  # in the config, not bonded
    fake_addon.apply_yaml("devices: []\n")
    await until(hass, lambda: not [u for u in uids(hass, entry) if "_btconn_" in u])
    assert f"{HUB_ID}_bt" in uids(hass, entry)  # the helper sensor stays while the hub has Bluetooth


async def test_unique_ids_follow_a_hub_rename(hass, fake_hub, fake_addon):
    entry, _c, _ = await boot(hass, fake_hub, fake_addon, BtHub())
    before = {u: e.entity_id for u, e in uids(hass, entry).items() if "_bt" in u}
    assert len(before) == 3
    await rename._move_entities(hass, entry, HUB_ID, "den")  # the registry step of a rename
    after = {u: e.entity_id for u, e in uids(hass, entry).items() if "_bt" in u}
    assert after == {u.replace(f"{HUB_ID}_", "den_", 1): eid for u, eid in before.items()}
    assert "den_bt" in after and "den_btconn_host_aabbccddee01" in after  # keyed by address, not slot


# ------------------------------------------------------------------ services
async def test_services_send_what_they_say(hass, fake_hub, fake_addon):
    bt = BtHub()
    _entry, c, _ = await boot(hass, fake_hub, fake_addon, bt)
    await call(hass, "bt_pair")
    assert bt.sent("bt_pair")[-1] == {"on": True, "secs": 120, "transport": "any", "mode": "host"}
    await call(hass, "bt_pair", mode="remote", transport="le", seconds=30)
    assert bt.sent("bt_pair")[-1] == {"on": True, "secs": 30, "transport": "le", "mode": "remote"}
    await call(hass, "bt_pair", mode="remote", address="11:22:33:44:55:02")
    assert bt.sent("bt_pair")[-1] == {"on": True, "mode": "remote", "addr": "11:22:33:44:55:02"}
    await call(hass, "bt_stop_pairing")
    assert bt.sent("bt_pair")[-1] == {"on": False}
    await call(hass, "bt_forget", address=STRAY.lower())  # resolved from bt_state: a host, slot 1
    assert bt.sent("bt_forget") == [{"slot": 1, "kind": "host"}]
    await call(hass, "bt_forget", address=RC)
    assert bt.sent("bt_forget")[-1] == {"slot": 0, "kind": "remote"}
    await c.async_bt_key(0, 12, 233, "press")
    assert bt.sent("bt_key") == [{"slot": 0, "page": 12, "usage": 233, "action": "press"}]


async def test_service_refusals(hass, fake_hub, fake_addon):
    bt = BtHub()
    _entry, c, _ = await boot(hass, fake_hub, fake_addon, bt)
    with pytest.raises(vol.Invalid):
        await call(hass, "bt_pair", seconds=301)
    with pytest.raises(vol.Invalid):
        await call(hass, "bt_pair", mode="both")
    with pytest.raises(ServiceValidationError):
        await call(hass, "bt_pair", mode="host", address=RC)  # an address is for picking a remote
    with pytest.raises(ServiceValidationError):
        await call(hass, "bt_pair", mode="remote", address="nope")
    with pytest.raises(ServiceValidationError) as exc:
        await call(hass, "bt_forget", address="AA:BB:CC:DD:EE:77")
    assert exc.value.translation_key == "bt_unknown_address"
    with pytest.raises(ServiceValidationError):
        await call(hass, "bt_forget", address="not an address")
    with pytest.raises(ServiceValidationError):
        await c.async_bt_key(0, 9, 4, "press")  # page 9
    with pytest.raises(ServiceValidationError):
        await c.async_bt_forget("host", 9)  # beyond the hub's hosts_max
    assert bt.log == []  # nothing invalid reached the hub
    bt.fail["bt_pair"] = "bt_busy"
    with pytest.raises(HomeAssistantError) as exc:
        await call(hass, "bt_pair")
    assert exc.value.translation_key == "bt_failed" and "busy" in exc.value.translation_placeholders["error"]
    bt.fail["bt_pair"] = "no_candidate"
    with pytest.raises(HomeAssistantError) as exc:
        await call(hass, "bt_pair", mode="remote", address=RC)
    assert exc.value.code == "no_candidate"


# ------------------------------------------------------------------ remote key events
async def test_remote_keys_fire_only_while_something_listens(hass, fake_hub, fake_addon):
    bt = BtHub()
    _entry, _c, _ = await boot(hass, fake_hub, fake_addon, bt)
    events = async_capture_events(hass, const.EVENT_BT_REMOTE_KEY)
    key = {"slot": 0, "page": 12, "usage": 233, "down": True}
    assert bt.sent("bt_listen") == []  # never on by default
    await bt.push("bt_remote_key", key)
    await asyncio.sleep(0.1)
    assert events == []
    await call(hass, "bt_listen", seconds=30)
    assert bt.sent("bt_listen") == [{"on": True}]
    await bt.push("bt_remote_key", key)
    await until(hass, lambda: len(events) == 1)
    assert events[0].data == {"hub_id": HUB_ID, **key, "address": RC, "name": "Homatics"}
    await call(hass, "bt_listen", on=False)
    assert bt.sent("bt_listen")[-1] == {"on": False}
    await bt.push("bt_remote_key", key)
    await asyncio.sleep(0.1)
    assert len(events) == 1


async def test_listen_lease_lapses_and_is_resent_after_a_reconnect(hass, fake_hub, fake_addon):
    bt = BtHub()
    _entry, c, _ = await boot(hass, fake_hub, fake_addon, bt)
    await call(hass, "bt_listen", seconds=30)
    await bt.ws.close()  # hello resets the hub's opt-in: it must be asked again
    await until(hass, lambda: len(bt.sent("bt_listen")) == 2)
    c.btm._leases["service"] = 0.0  # the lease runs out
    await c.btm._expired()
    assert bt.sent("bt_listen")[-1] == {"on": False} and not c.btm.listening()


async def test_malformed_remote_keys_are_dropped(hass, fake_hub, fake_addon):
    bt = BtHub()
    _entry, c, _ = await boot(hass, fake_hub, fake_addon, bt)
    events = async_capture_events(hass, const.EVENT_BT_REMOTE_KEY)
    await call(hass, "bt_listen")
    for bad in ({"slot": "0", "page": 12, "usage": 1, "down": True}, {"slot": 0, "page": 12, "usage": 1, "down": 1}):
        c.btm.on_key(bad)
    assert events == []


# ------------------------------------------------------------------ diagnostics
async def test_diagnostics_redact_addresses(hass, fake_hub, fake_addon):
    bt = BtHub(first=state(3, pairing={"result": "ok", "addr": STRAY, "kind": "host"}, candidates=[{"addr": RC, "rssi": -50}]))
    entry, _c, _ = await boot(hass, fake_hub, fake_addon, bt)
    diag = await async_get_config_entry_diagnostics(hass, entry)
    text = json.dumps(diag)
    for addr in (TV, RC, STRAY):
        assert addr not in text
    b = diag["bluetooth"]
    assert b["supported"] is True and b["hello_bt"]["hosts_max"] == 4 and b["gen"] == 3
    assert b["state"]["hosts"][0]["addr"] == "**REDACTED**" and b["state"]["pairing"]["addr"] == "**REDACTED**"
    assert b["state"]["candidates"][0]["addr"] == "**REDACTED**" and b["state"]["hosts"][0]["slot"] == 0
    assert b["state"]["hosts"][0]["name"] == "**REDACTED**" and b["state"]["remotes"][0]["name"] == "**REDACTED**"
    assert "Homatics" not in text.replace("Harmony", "")  # device names are redacted too


# ------------------------------------------------------------------ the editor relay (add-on channel), end to end
async def test_bt_state_is_pushed_after_hello_and_on_every_change(hass, fake_hub, fake_addon):
    bt = BtHub()
    _entry, _c, _ = await boot(hass, fake_hub, fake_addon, bt)
    first = await fake_addon.frame("bt_state", lambda f: f.get("state"))
    assert first["online"] is True and first["supported"] is True and first["limits"]["hosts_max"] == 4
    assert [h["addr"] for h in first["state"]["hosts"]] == [TV, STRAY]
    await bt.push("bt_state", state(8, window="remote", candidates=[{"addr": "11:22:33:44:55:02", "name": "New", "rssi": -60}]))
    got = await fake_addon.frame("bt_state", lambda f: (f.get("state") or {}).get("window") == "remote")
    assert got["state"]["candidates"][0]["name"] == "New"
    n = len(fake_addon.of("bt_state"))
    await bt.push("bt_state", state(8, window="remote", candidates=[{"addr": "11:22:33:44:55:02", "name": "New", "rssi": -60}]))
    await bt.push("bt_state", state(9, state="starting"))
    await fake_addon.frame("bt_state", lambda f: (f.get("state") or {}).get("state") == "starting")
    assert len(fake_addon.of("bt_state")) == n + 1  # an unchanged state is not repeated


async def test_a_hub_without_bluetooth_reports_unsupported(hass, fake_hub, fake_addon):
    _entry, _c, _ = await boot(hass, fake_hub, fake_addon, BtHub(bt=None), yaml="devices: []\n")
    got = await fake_addon.frame("bt_state")
    assert got["supported"] is False and "state" not in got and "limits" not in got
    fake_addon.send(HUB_ID, {"t": "bt_cmd", "id": "x1", "op": "pair_stop"})
    res = await fake_addon.frame("bt_result", lambda f: f["id"] == "x1")
    assert res["ok"] is False and res["code"] == "unsupported"


async def test_bt_cmd_runs_on_the_hub_and_its_result_is_sent(hass, fake_hub, fake_addon):
    bt = BtHub()
    _entry, _c, _ = await boot(hass, fake_hub, fake_addon, bt)

    async def run(cid, **cmd):
        fake_addon.send(HUB_ID, {"t": "bt_cmd", "id": cid, **cmd})
        return await fake_addon.frame("bt_result", lambda f: f["id"] == cid)

    assert (await run("c1", op="pair", mode="remote", transport="le", secs=45))["ok"] is True
    assert bt.sent("bt_pair")[-1] == {"on": True, "secs": 45, "transport": "le", "mode": "remote"}
    assert (await run("c2", op="pick", addr="11:22:33:44:55:02"))["ok"] is True
    assert bt.sent("bt_pair")[-1] == {"on": True, "mode": "remote", "addr": "11:22:33:44:55:02"}
    bt.fail["bt_pair"] = "no_candidate"
    bad = await run("c3", op="pick", addr="11:22:33:44:55:03")
    assert bad["ok"] is False and bad["code"] == "no_candidate" and bad["msg"]
    del bt.fail["bt_pair"]
    assert (await run("c4", op="pair_stop"))["ok"] is True and bt.sent("bt_pair")[-1] == {"on": False}
    assert (await run("c5", op="forget", slot=1, kind="host"))["ok"] is True
    assert bt.sent("bt_forget") == [{"slot": 1, "kind": "host"}]
    ok = await run("c6", op="key", slot=0, page=7, usage=40, action="press")
    assert ok["ok"] is True and bt.sent("bt_key") == [{"slot": 0, "page": 7, "usage": 40, "action": "press"}]
    assert ok["state"]["gen"] == 1  # the hub's state rides along
    assert (await run("c7", op="forget", slot=7, kind="host"))["ok"] is False  # beyond hosts_max
    assert len(bt.sent("bt_forget")) == 1


async def test_listen_forwards_hub_keys_as_bt_key_until_told_to_stop(hass, fake_hub, fake_addon):
    bt = BtHub()
    _entry, _c, _ = await boot(hass, fake_hub, fake_addon, bt)
    events = async_capture_events(hass, const.EVENT_BT_REMOTE_KEY)
    key = {"slot": 0, "page": 12, "usage": 233, "down": True}
    await bt.push("bt_remote_key", key)  # nobody asked: not forwarded
    fake_addon.send(HUB_ID, {"t": "listen", "on": True})
    await until(hass, lambda: bt.sent("bt_listen") == [{"on": True}])
    await bt.push("bt_remote_key", key)
    got = await fake_addon.frame("bt_key")
    assert {k: got[k] for k in key} == key and len(fake_addon.of("bt_key")) == 1
    await until(hass, lambda: len(events) == 1)  # the HA event fires too, while the editor listens
    fake_addon.send(HUB_ID, {"t": "listen", "on": False})
    await until(hass, lambda: bt.sent("bt_listen")[-1] == {"on": False})
    await bt.push("bt_remote_key", key)
    await bt.push("bt_state", state(2))  # a frame behind the key: when it arrives the key was handled
    await fake_addon.frame("bt_state", lambda f: (f.get("state") or {}).get("gen") == 2)
    assert len(fake_addon.of("bt_key")) == 1 and len(events) == 1


async def test_listen_sent_with_the_hello_answer_is_honoured(hass, fake_hub, fake_addon):
    bt = BtHub()
    fake_addon.listen_on = True
    _entry, c, _ = await boot(hass, fake_hub, fake_addon, bt)
    await until(hass, lambda: c.btm.addon_listen and bt.sent("bt_listen") == [{"on": True}])


@pytest.mark.parametrize(
    "cmd",
    [
        {"op": "key", "slot": 0, "page": 9, "usage": 4, "action": "press"},
        {"op": "key", "slot": 0, "page": 7, "usage": 99999, "action": "press"},
        {"op": "key", "slot": 99, "page": 7, "usage": 4, "action": "press"},
        {"op": "forget", "slot": 200, "kind": "host"},
        {"op": "forget", "slot": 0, "kind": "x"},
        {"op": "pair", "secs": 9999},
        {"op": "pair", "mode": "nope"},
        {"op": "pick", "addr": "AA:BB"},
        {"op": "pick", "addr": "AA:BB:CC:DD:EE:FF", "extra": 1},
        {"op": "format_disk"},
    ],
)
async def test_integration_revalidates_what_the_addon_sends(hass, fake_hub, fake_addon, cmd):
    """The add-on is not trusted: commands it should never have sent are refused here and never reach the hub."""
    bt = BtHub()
    _entry, _c, _ = await boot(hass, fake_hub, fake_addon, bt)
    fake_addon.send(HUB_ID, {"t": "bt_cmd", "id": "evil1", **cmd})
    res = await fake_addon.frame("bt_result", lambda f: f["id"] == "evil1")
    assert res["ok"] is False and res["code"]
    assert bt.log == []


async def test_commands_without_a_usable_id_are_ignored_and_the_in_flight_count_is_capped(
    hass, fake_hub, fake_addon, monkeypatch
):
    monkeypatch.setattr(const, "BT_MAX_INFLIGHT", 2)
    bt = BtHub()
    bt.hold = asyncio.Event()  # bt_pair stays unanswered until released
    _entry, c, _ = await boot(hass, fake_hub, fake_addon, bt)
    for raw in ({"op": "pair_stop"}, {"id": "../x", "op": "pair_stop"}, {"id": 5, "op": "pair_stop"}):
        c.btm.on_command(raw)
    c.btm.on_command("pair_stop")
    assert c.btm._inflight == 0 and bt.log == []
    for i in range(3):
        fake_addon.send(HUB_ID, {"t": "bt_cmd", "id": f"p{i}", "op": "pair_stop"})
    busy = await fake_addon.frame("bt_result", lambda f: f["id"] == "p2")  # the third one is over the cap
    assert busy["ok"] is False and busy["code"] == "busy"
    bt.hold.set()
    for i in range(2):
        assert (await fake_addon.frame("bt_result", lambda f, i=i: f["id"] == f"p{i}"))["ok"] is True
    assert c.btm._inflight == 0


async def test_each_result_is_sent_as_its_command_finishes(hass, fake_hub, fake_addon):
    bt = BtHub()
    bt.hold = asyncio.Event()
    _entry, _c, _ = await boot(hass, fake_hub, fake_addon, bt)
    fake_addon.send(HUB_ID, {"t": "bt_cmd", "id": "slow", "op": "pair_stop"})  # held at the hub
    fake_addon.send(HUB_ID, {"t": "bt_cmd", "id": "fast", "op": "key", "slot": 0, "page": 7, "usage": 4, "action": "press"})
    await fake_addon.frame("bt_result", lambda f: f["id"] == "fast")
    assert not [f for f in fake_addon.of("bt_result") if f["id"] == "slow"]
    bt.hold.set()
    await fake_addon.frame("bt_result", lambda f: f["id"] == "slow")


async def test_hub_going_offline_and_back_is_sent(hass, fake_hub, fake_addon):
    bt = BtHub()
    _entry, _c, _ = await boot(hass, fake_hub, fake_addon, bt)
    assert (await fake_addon.frame("hub"))["online"] is True
    await bt.ws.close()
    await fake_addon.frame("hub", lambda f: f["online"] is False)
    off = await fake_addon.frame("bt_state", lambda f: f["online"] is False)
    assert off["supported"] is True
    gone = next(i for i, f in enumerate(fake_addon.frames) if f.get("t") == "hub" and f["online"] is False)
    await fake_addon.frame("hub", lambda f: f["online"] is True, start=gone)


async def test_a_quiet_channel_after_a_reload_still_relays_bluetooth(hass, fake_hub, fake_addon):
    """Regression: with the config cached the add-on has nothing newer to push, so the channel stays silent after
    `hello`. Nothing may wait for a bundle (the old relay waited for the first apply and never started)."""
    bt = BtHub()
    entry, c, _ = await boot(hass, fake_hub, fake_addon, bt)
    rev = c.rev
    assert await hass.config_entries.async_unload(entry.entry_id)
    n = len(fake_addon.frames)
    assert await hass.config_entries.async_setup(entry.entry_id)
    c = entry.runtime_data
    hello = await fake_addon.frame("hello", start=n)
    assert hello["known_rev"] == rev  # the add-on answers with nothing at all
    await asyncio.wait_for(c._first_apply.wait(), 5)  # a quiet channel is an answer: nothing waits for a bundle
    fake_addon.send(HUB_ID, {"t": "bt_cmd", "id": "q1", "op": "pair_stop"})
    assert (await fake_addon.frame("bt_result", lambda f: f["id"] == "q1"))["ok"] is True
    assert bt.sent("bt_pair")[-1] == {"on": False} and c.link_state == "ok"


async def test_when_the_channel_drops_listening_stops(hass, fake_hub, fake_addon):
    bt = BtHub()
    _entry, c, _ = await boot(hass, fake_hub, fake_addon, bt)
    fake_addon.send(HUB_ID, {"t": "listen", "on": True})
    await until(hass, lambda: bt.sent("bt_listen") == [{"on": True}])
    await fake_addon.close_socket(HUB_ID)
    await until(hass, lambda: bt.sent("bt_listen")[-1] == {"on": False})
    assert not c.btm.addon_listen


# ------------------------------------------------------------------ hardening
async def test_out_of_range_keys_are_not_forwarded(hass, fake_hub, fake_addon):
    _entry, c, _ = await boot(hass, fake_hub, fake_addon, BtHub())
    fake_addon.send(HUB_ID, {"t": "listen", "on": True})
    await until(hass, lambda: c.btm.addon_listen)
    for bad in ({"slot": 256, "page": 12, "usage": 1, "down": True}, {"slot": 0, "page": 300, "usage": 1, "down": True},
                {"slot": 0, "page": 12, "usage": 70000, "down": True}, {"slot": -1, "page": 12, "usage": 1, "down": True}):
        c.btm.on_key(bad)
    c.btm.on_key({"slot": 255, "page": 255, "usage": 65535, "down": False})
    got = await fake_addon.frame("bt_key")
    assert got["slot"] == 255 and len(fake_addon.of("bt_key")) == 1


async def test_hub_events_are_rate_limited_on_the_bus(hass, fake_hub, fake_addon):
    entry, c, _ = await boot(hass, fake_hub, fake_addon, BtHub())
    events = async_capture_events(hass, const.EVENT_BT_REMOTE_KEY)
    await call(hass, "bt_listen")
    for i in range(200):
        c.btm.on_key({"slot": 0, "page": 12, "usage": i, "down": True})
    await hass.async_block_till_done()
    assert 15 <= len(events) <= 30 and c.btm.events_rate_dropped >= 170
    diag = await async_get_config_entry_diagnostics(hass, entry)
    assert diag["bluetooth"]["events_rate_dropped"] == c.btm.events_rate_dropped


async def test_odd_hub_error_codes_become_a_valid_result_code(hass, fake_hub, fake_addon):
    bt = BtHub()
    _entry, c, _ = await boot(hass, fake_hub, fake_addon, bt)
    assert BtError("k", "", "m").code == "failed" and BtError("k", None, "m").code == "failed"
    assert BtError("k", "Bad-Code 9", "m").code == "bad_code__"
    bt.fail["bt_pair"] = ""
    res = await c.btm.run_command({"id": "e1", "op": "pair_stop"})
    assert {k: res[k] for k in ("id", "ok", "code", "msg")} == {"id": "e1", "ok": False, "code": "failed", "msg": "refused"}
    assert await c.btm.run_command({"op": "pair_stop"}) is None  # no id: nobody can be told
