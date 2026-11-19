"""The integration's Bluetooth support against a scripted hub (no hub binary): state and gen ordering, entities, services,
remote-key events, diagnostics, rename, and the editor relay round trip through the real add-on app."""

import asyncio
import collections
import json

import aiohttp
import pytest
import voluptuous as vol
from conftest import HUB_ID, make_entry
from homeassistant.config_entries import ConfigEntryState
from homeassistant.exceptions import HomeAssistantError, ServiceValidationError
from homeassistant.helpers import entity_registry as er
from pytest_homeassistant_custom_component.common import async_capture_events

import custom_components.ha_harmony  # noqa: F401  (before the HA plugin shadows it)
from custom_components.ha_harmony import const, rename
from custom_components.ha_harmony.addon_link import AddonClient, AddonRejected, AddonUnreachable
from custom_components.ha_harmony.bluetooth import BtError
from custom_components.ha_harmony.const import DOMAIN
from custom_components.ha_harmony.diagnostics import async_get_config_entry_diagnostics
from custom_components.ha_harmony.harmony_core import bt_relay
from harmony_addon import bt_routes

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

    async def handler(self, ws, env):
        t, i, d = env["t"], env["id"], env.get("d", {})
        self.ws = ws
        if t == "hello":
            ack = {"hub_id": HUB_ID, "activities": [], **({"bt": self.bt} if self.bt else {})}
            await ws.send_json({"v": 1, "id": i, "t": "hello_ack", "d": ack})
            if self.bt:
                await self.push("bt_state", self.state)
        elif t == "set_config":
            await ws.send_json({"v": 1, "id": i, "t": "config_ack", "d": {"ok": True}})
        elif t == "ping":
            await ws.send_json({"v": 1, "id": i, "t": "pong", "d": {}})
        elif t.startswith("bt_"):
            self.log.append((t, d))
            if t in self.fail:
                err = {"code": self.fail[t], "message": "refused"}
                await ws.send_json({"v": 1, "id": i, "t": "error", "d": err})
            elif t in ("bt_pair", "bt_forget"):
                await ws.send_json({"v": 1, "id": i, "t": "bt_state", "d": self.state})
            else:
                await ws.send_json({"v": 1, "id": i, "t": "command_result", "d": {"ok": True}})

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


@pytest.fixture(autouse=True)
def fast_relay(monkeypatch):
    monkeypatch.setattr(const, "BT_POLL_FAST", 0.05)
    monkeypatch.setattr(const, "BT_POLL_IDLE", 0.05)


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


async def editor(fake_addon, method, path, **body):
    async with aiohttp.ClientSession() as s:
        url = f"http://127.0.0.1:{fake_addon.port}/api/v1/bt/{path}"
        if method == "GET":
            r = await s.get(url, params={"hub": HUB_ID, **body})
        else:
            r = await s.post(url, json={"hub": HUB_ID, **body}, headers={"X-HH-CSRF": "1"})
        return r.status, await r.json()


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
    await until(hass, lambda: c.btm.exchanges > 0)
    _st, body = await editor(fake_addon, "GET", "status")
    assert body["status"] == "unsupported" and body["state"] is None
    assert (await editor(fake_addon, "POST", "pair"))[1]["error"]["code"] == "bt_unsupported"
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


# ------------------------------------------------------------------ the editor relay, end to end
async def test_relay_round_trip(hass, fake_hub, fake_addon):
    bt = BtHub()
    _entry, c, _ = await boot(hass, fake_hub, fake_addon, bt)
    await until(hass, lambda: c.btm.exchanges > 0)
    st, body = await editor(fake_addon, "GET", "status")
    assert body["status"] == "ok" and body["limits"]["hosts_max"] == 4
    assert [h["addr"] for h in body["state"]["hosts"]] == [TV, STRAY]

    st, body = await editor(fake_addon, "POST", "pair", mode="remote", transport="le", secs=45)
    assert st == 200 and body["ok"] is True
    assert bt.sent("bt_pair")[-1] == {"on": True, "secs": 45, "transport": "le", "mode": "remote"}
    scan = state(8, window="remote", candidates=[{"addr": "11:22:33:44:55:02", "name": "New", "rssi": -60}])
    await bt.push("bt_state", scan)
    await until(hass, lambda: c.btm.fast())  # a window is open: the integration polls fast
    async with asyncio.timeout(5):
        while True:
            _, body = await editor(fake_addon, "GET", "status")
            if body["state"] and body["state"].get("candidates"):
                break
            await asyncio.sleep(0.05)
    assert body["state"]["window"] == "remote" and body["state"]["candidates"][0]["name"] == "New"

    assert (await editor(fake_addon, "POST", "pick", addr="11:22:33:44:55:02"))[0] == 200
    assert bt.sent("bt_pair")[-1] == {"on": True, "mode": "remote", "addr": "11:22:33:44:55:02"}
    bt.fail["bt_pair"] = "no_candidate"
    st, body = await editor(fake_addon, "POST", "pick", addr="11:22:33:44:55:03")
    assert st == 502 and body["error"]["hub_code"] == "no_candidate"
    assert (await editor(fake_addon, "POST", "pair/stop"))[0] == 502  # the scripted failure applies to every bt_pair
    del bt.fail["bt_pair"]
    assert (await editor(fake_addon, "POST", "pair/stop"))[0] == 200
    assert bt.sent("bt_pair")[-1] == {"on": False}
    assert (await editor(fake_addon, "POST", "forget", slot=1, kind="host"))[0] == 200
    assert bt.sent("bt_forget") == [{"slot": 1, "kind": "host"}]
    st, body = await editor(fake_addon, "POST", "key", slot=0, page=7, usage=40, action="press")
    assert st == 200 and bt.sent("bt_key") == [{"slot": 0, "page": 7, "usage": 40, "action": "press"}]
    st, body = await editor(fake_addon, "POST", "forget", slot=7)  # beyond hosts_max: refused by the add-on
    assert st == 422 and body["error"]["bt_code"] == "bad_slot"
    assert len(bt.sent("bt_forget")) == 1


async def test_relay_listen_and_events(hass, fake_hub, fake_addon):
    bt = BtHub()
    _entry, c, _ = await boot(hass, fake_hub, fake_addon, bt)
    events = async_capture_events(hass, const.EVENT_BT_REMOTE_KEY)
    await until(hass, lambda: c.btm.exchanges > 0)
    assert (await editor(fake_addon, "POST", "listen", on=True))[1]["listening"] is True
    await until(hass, lambda: bt.sent("bt_listen") == [{"on": True}])
    assert c.btm.fast()
    key = {"slot": 0, "page": 12, "usage": 233, "down": True}
    await bt.push("bt_remote_key", key)
    async with asyncio.timeout(5):
        while not (out := (await editor(fake_addon, "GET", "events"))[1])["events"]:
            await asyncio.sleep(0.05)
    assert [{k: e[k] for k in key} for e in out["events"]] == [key] and out["listening"] is True
    assert len(events) == 1  # the HA event fires too, while the editor listens
    assert (await editor(fake_addon, "POST", "listen", on=False))[0] == 200
    await until(hass, lambda: bt.sent("bt_listen")[-1] == {"on": False})
    await bt.push("bt_remote_key", key)
    await asyncio.sleep(0.2)
    assert len(events) == 1


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
async def test_integration_revalidates_what_the_addon_queues(hass, fake_hub, fake_addon, cmd):
    """The add-on is not trusted: commands it should never have queued are refused here and never reach the hub."""
    bt = BtHub()
    _entry, c, _ = await boot(hass, fake_hub, fake_addon, bt)
    await until(hass, lambda: c.btm.exchanges > 0)
    h = fake_addon.app[bt_routes.BT_KEY].hub(HUB_ID)
    fut = asyncio.get_running_loop().create_future()
    h.waiting["evil1"] = fut
    h.queue.append({"id": "evil1", **cmd})
    res = await asyncio.wait_for(fut, 5)
    assert res["ok"] is False and res["code"]
    assert bt.log == []


async def test_integration_ignores_commands_without_a_usable_id_and_caps_each_batch(hass, fake_hub, fake_addon):
    bt = BtHub()
    _entry, c, _ = await boot(hass, fake_hub, fake_addon, bt)
    await until(hass, lambda: c.btm.exchanges > 0)
    await c.btm._run_command({"op": "pair_stop"})  # no id
    await c.btm._run_command({"id": "../x", "op": "pair_stop"})
    await c.btm._run_command("pair_stop")
    assert bt.log == [] and not c.btm._results
    for i in range(bt_relay.MAX_COMMANDS + 5):
        await c.btm._run_command({"id": f"k{i}", "op": "pair_stop"})
    assert len(c.btm._results) <= 32  # results are a bounded queue


async def test_relay_survives_an_addon_without_it(hass, fake_hub, fake_addon, monkeypatch):
    calls = []

    async def gone(self, body):
        calls.append(body)
        raise AddonUnreachable("no relay")

    monkeypatch.setattr(AddonClient, "bt_exchange", gone)
    monkeypatch.setattr(const, "LINK_BACKOFF_MIN", 0.05)
    bt = BtHub()
    entry, c, _ = await boot(hass, fake_hub, fake_addon, bt)
    await until(hass, lambda: len(calls) >= 2)  # keeps trying, with backoff, and the entry stays loaded
    assert entry.state is ConfigEntryState.LOADED and c.bt_state is not None


# ------------------------------------------------------------------ hardening
def _quiet_addon(monkeypatch, reply):
    """Replace the exchange: `reply(body)` returns the add-on's answer (or raises). Returns the list of bodies sent."""
    sent = []

    async def fake(self, body):
        sent.append(body)
        return reply(body)

    monkeypatch.setattr(AddonClient, "bt_exchange", fake)
    return sent


async def test_out_of_range_keys_never_reach_the_report(hass, fake_hub, fake_addon, monkeypatch):
    sent = _quiet_addon(monkeypatch, lambda b: {"commands": [], "listen": True, "fast": False})
    _entry, c, _ = await boot(hass, fake_hub, fake_addon, BtHub())
    await until(hass, lambda: c.btm.addon_listen)
    for bad in ({"slot": 256, "page": 12, "usage": 1, "down": True}, {"slot": 0, "page": 300, "usage": 1, "down": True},
                {"slot": 0, "page": 12, "usage": 70000, "down": True}, {"slot": -1, "page": 12, "usage": 1, "down": True}):
        c.btm.on_key(bad)
    assert not c.btm._events
    c.btm.on_key({"slot": 255, "page": 255, "usage": 65535, "down": False})
    assert len(c.btm._events) == 1
    await until(hass, lambda: any(b["events"] for b in sent))


async def test_a_refused_report_is_dropped_not_resent(hass, fake_hub, fake_addon, monkeypatch):
    calls = []

    def reply(body):
        calls.append(body)
        if body["events"]:
            raise AddonRejected("400")
        return {"commands": [], "listen": True, "fast": False}

    _quiet_addon(monkeypatch, reply)
    _entry, c, _ = await boot(hass, fake_hub, fake_addon, BtHub())
    await until(hass, lambda: c.btm.addon_listen)
    c.btm.on_key({"slot": 0, "page": 12, "usage": 1, "down": True})
    await until(hass, lambda: not c.btm._events and sum(1 for b in calls if b["events"]) == 1)
    n = len(calls)
    await until(hass, lambda: len(calls) > n + 2)
    assert sum(1 for b in calls if b["events"]) == 1  # sent once, never again


async def test_events_are_acknowledged_by_seq(hass, fake_hub, fake_addon):
    _entry, c, _ = await boot(hass, fake_hub, fake_addon, BtHub())
    m = c.btm
    m.addon_listen = True
    m.on_key({"slot": 0, "page": 12, "usage": 1, "down": True})
    _body, n_results, seq = m._report()
    m.on_key({"slot": 0, "page": 12, "usage": 2, "down": True})  # arrives while the report is in flight
    m._sent(n_results, seq)
    assert [e["usage"] for _, e in m._events] == [2]
    for i in range(bt_relay.MAX_COMMANDS * 20):
        m.on_key({"slot": 0, "page": 12, "usage": i, "down": True})
    assert len(m._events) == 64 and m.events_dropped > 0
    m.addon_listen = False


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
    await c.btm._run_command({"id": "e1", "op": "pair_stop"})
    assert c.btm._results[-1] == {"id": "e1", "ok": False, "code": "failed", "msg": "refused"}


async def test_when_the_addon_goes_away_listening_stops(hass, fake_hub, fake_addon, monkeypatch):
    bt = BtHub()
    state_ = {"down": False}

    def reply(body):
        if state_["down"]:
            raise AddonUnreachable("gone")
        return {"commands": [], "listen": True, "fast": False}

    _quiet_addon(monkeypatch, reply)
    _entry, c, _ = await boot(hass, fake_hub, fake_addon, bt)
    await until(hass, lambda: bt.sent("bt_listen") == [{"on": True}])
    c.btm.on_key({"slot": 0, "page": 12, "usage": 1, "down": True})
    state_["down"] = True
    await until(hass, lambda: bt.sent("bt_listen")[-1] == {"on": False})
    assert not c.btm.addon_listen and not c.btm._events


async def test_commands_without_an_id_do_not_spin_the_loop(hass, fake_hub, fake_addon, monkeypatch):
    sent = _quiet_addon(monkeypatch, lambda b: {"commands": [{"op": "pair_stop"}, {"id": "../x", "op": "pair_stop"}], "fast": False})
    _entry, c, _ = await boot(hass, fake_hub, fake_addon, BtHub())
    n = len(sent)
    await asyncio.sleep(0.5)
    assert len(sent) - n <= 15  # paced at the (patched, 0.05 s) idle cadence, not a tight loop
    assert bt_relay is not None and c.btm._results == collections.deque()


async def test_each_result_is_reported_as_its_command_finishes(hass, fake_hub, fake_addon, monkeypatch):
    bodies = []
    batch = [{"id": "a1", "op": "pair_stop"}, {"id": "a2", "op": "pair_stop"}]

    def reply(body):
        bodies.append(body)
        return {"commands": [dict(x) for x in batch] if len(bodies) == 1 else [], "fast": False}

    _quiet_addon(monkeypatch, reply)
    bt = BtHub()
    _entry, _c, _ = await boot(hass, fake_hub, fake_addon, bt)
    await until(hass, lambda: len(bt.sent("bt_pair")) == 2)
    await until(hass, lambda: any(b["results"] for b in bodies))
    assert [r["id"] for r in next(b for b in bodies if b["results"])["results"]] == ["a1"]  # before a2 had run
