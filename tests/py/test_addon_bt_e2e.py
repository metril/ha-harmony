# ruff: noqa: F811
"""The editor's Bluetooth views end to end: the real add-on and the built bundle in Chromium, with a scripted fake
integration on `POST /api/v1/bundle/bt` standing in for Home Assistant and the hub. Skipped like test_addon_e2e.py.
Set HH_BT_SHOTS=<dir> to write a screenshot of every screen at 1440x900 and 390x844:
    HH_CHROMIUM=/usr/bin/google-chrome uv run pytest tests/py/test_addon_bt_e2e.py -n0
"""

import asyncio
import os
import re
from pathlib import Path

import aiohttp
import pytest
from test_addon_e2e import DIST, TOKEN, H, api, browser, open_editor, seed  # noqa: F401

from harmony_addon.app import create_app
from harmony_addon.auth import NoAuth
from harmony_addon.options import Options
from harmony_core.bt_codes import DEFAULT_COMMANDS

pytestmark = pytest.mark.skipif(not (DIST / "index.html").is_file(), reason="frontend not built")
SHOTS = os.environ.get("HH_BT_SHOTS")
BEARER = {"Authorization": f"Bearer {TOKEN}"}
LIMITS = {"hosts_max": 8, "conn_max": 2, "remotes_max": 4, "le_bonds_max": 8, "pages": [7, 12], "le": True}
BOX = "AA:BB:CC:DD:EE:01"
REMOTE = "11:22:33:44:55:01"


@pytest.fixture
async def site(aiohttp_server, tmp_path):
    app = create_app(tmp_path, options=Options("info"), auth=NoAuth(), static_dir=DIST, bundle_token=TOKEN, bundle_networks=None, wait_seconds=1.0)
    server = await aiohttp_server(app)
    return f"http://127.0.0.1:{server.port}"


class FakeIntegration:
    """Plays Home Assistant: polls `bundle/bt` like the real one, runs the editor's commands on a scripted hub."""

    def __init__(self, origin):
        self.origin = origin
        self.supported = True
        self.online = True
        self.state = {"state": "ready", "discoverable": False, "gen": 1, "hosts": [], "remotes": []}
        self.refuse: dict[str, tuple[str, str]] = {}  # op -> (hub code, message)
        self.ran: list[dict] = []
        self.out_events: list[dict] = []
        self.listen = False
        self._results: list[dict] = []
        self._task: asyncio.Task | None = None
        self._candidates: list[dict] = []

    async def start(self):
        await api(self.origin, "GET", "/api/v1/bundle?hub=default", headers=BEARER)  # the hub id is now linked
        self._task = asyncio.create_task(self._loop())
        await asyncio.sleep(0.2)

    async def stop(self):
        if self._task:
            self._task.cancel()

    async def _loop(self):
        async with aiohttp.ClientSession() as s:
            while True:
                body = {"hub": "default", "supported": self.supported, "online": self.online, "results": self._results, "events": self.out_events}
                if self.supported:
                    body["state"] = self.state
                    body["limits"] = LIMITS
                self._results, self.out_events = [], []
                async with s.post(f"{self.origin}/api/v1/bundle/bt", json=body, headers=BEARER) as r:
                    if r.status == 200:
                        got = await r.json()
                        self.listen = got["listen"]
                        for cmd in got["commands"]:
                            self._results.append(self._run(cmd))
                await asyncio.sleep(0.15)

    def _run(self, cmd):
        self.ran.append(cmd)
        op = cmd["op"]
        if op in self.refuse:
            code, msg = self.refuse[op]
            return {"id": cmd["id"], "ok": False, "code": code, "msg": msg}
        st = self.state
        if op == "pair":
            st.update(discoverable=cmd.get("mode", "host") == "host", window=cmd.get("mode", "host"), discoverable_secs=cmd.get("secs", 120))
            if cmd.get("mode") == "remote":
                st["candidates"] = list(self._candidates)
        elif op == "pair_stop":
            self.close_window()
        elif op == "pick":
            self.add_remote(cmd["addr"], "Homatics Remote")
        elif op == "forget":
            key = "remotes" if cmd.get("kind") == "remote" else "hosts"
            st[key] = [e for e in st[key] if e["slot"] != cmd["slot"]]
        st["gen"] += 1
        return {"id": cmd["id"], "ok": True}

    # ---- scripted hub events
    def close_window(self):
        for k in ("window", "discoverable_secs", "candidates"):
            self.state.pop(k, None)
        self.state["discoverable"] = False

    def add_host(self, addr=BOX, name="Living room box", configured=None, result=True):
        entry = {"slot": len(self.state["hosts"]), "addr": addr, "name": name, "transport": "le", "connected": True}
        if configured is not None:
            entry["configured"] = configured
        self.state["hosts"].append(entry)
        if result:
            self.state["pairing"] = {"result": "ok", "addr": addr, "name": name, "transport": "le", "kind": "host"}
        self.close_window()
        self.state["gen"] += 1

    def add_remote(self, addr=REMOTE, name="Homatics Remote", configured=None):
        entry = {"slot": len(self.state["remotes"]), "addr": addr, "name": name, "connected": True, "battery": 82}
        if configured is not None:
            entry["configured"] = configured
        self.state["remotes"].append(entry)
        self.state["pairing"] = {"result": "ok", "addr": addr, "name": name, "kind": "remote"}
        self.close_window()
        self.state["gen"] += 1

    def candidates(self, *c):
        self._candidates = list(c)
        if self.state.get("window") == "remote":
            self.state["candidates"] = list(c)
            self.state["gen"] += 1

    def press(self, page, usage, slot=0):
        self.out_events += [{"slot": slot, "page": page, "usage": usage, "down": True}, {"slot": slot, "page": page, "usage": usage, "down": False}]


@pytest.fixture
async def bt(site):
    f = FakeIntegration(site)
    await f.start()
    yield f
    await f.stop()


async def shot(page, name):
    if not SHOTS:
        return
    Path(SHOTS).mkdir(parents=True, exist_ok=True)
    await page.wait_for_timeout(250)
    for tag, (w, h) in (("1440", (1440, 900)), ("390", (390, 844))):
        await page.set_viewport_size({"width": w, "height": h})
        await page.wait_for_timeout(150)
        await page.screenshot(path=f"{SHOTS}/{name}-{tag}.png", full_page=True)
    await page.set_viewport_size({"width": 1440, "height": 900})


async def open_bt(browser, site, route="bluetooth"):
    problems: list[str] = []
    page = await open_editor(browser, site, problems, viewport={"width": 1440, "height": 900})
    await page.evaluate(f"location.hash = '#/{route}'")
    return page, problems


async def draft(site):
    _, got = await api(site, "GET", "/api/v1/config")
    return (got["draft"] or {}).get("config", {})


async def draft_has(site, pred, tries=40):
    for _ in range(tries):
        cfg = await draft(site)
        if pred(cfg):
            return cfg
        await asyncio.sleep(0.15)
    raise AssertionError(f"draft never matched: {await draft(site)}")


def clean(problems):
    return [p for p in problems if "Failed to load resource" not in p]


async def test_pair_a_host_name_it_pick_commands_and_save(browser, bt, site):
    page, problems = await open_bt(browser, site)
    await page.get_by_role("heading", name="Bluetooth", exact=True).wait_for()
    await page.get_by_text("Ready", exact=True).first.wait_for()
    await page.get_by_text("No device is paired yet").wait_for()
    await shot(page, "01-panel-empty")

    await page.get_by_role("link", name="Add Bluetooth device").click()
    await page.get_by_role("radio", name="Any (recommended)").wait_for()
    await shot(page, "02-host-start")
    await page.get_by_role("button", name="Start pairing").click()
    await page.get_by_role("timer").wait_for()
    await page.get_by_text("pair with “Harmony …”").wait_for()
    assert await page.get_by_role("timer").inner_text() != ""
    assert any(c["op"] == "pair" and c["transport"] == "any" for c in bt.ran)
    await shot(page, "03-host-waiting")

    bt.add_host()  # the TV box pairs
    await page.get_by_role("heading", name="Name it and choose its commands").wait_for()
    focused = await page.evaluate("document.querySelector('hh-app').shadowRoot.querySelector('hh-bluetooth').shadowRoot.querySelector('hh-bt-pair').shadowRoot.activeElement?.id")
    assert focused == "step-title"  # focus moved into the new step
    await page.get_by_text(f"{len(DEFAULT_COMMANDS)} commands chosen").wait_for()  # the default set is preselected
    await page.get_by_label("Name", exact=True).fill("Living room box")
    await page.get_by_label("Search commands").fill("F1")
    await page.get_by_role("checkbox", name=re.compile(r"^F1 ")).check()
    await page.get_by_label("Command name").fill("Netflix")
    await page.get_by_label("HID code").fill("HID:0C:0223")
    await page.get_by_role("button", name="Add code").click()
    await page.get_by_label("Command name").fill("Bad")
    await page.get_by_label("HID code").fill("HID:09:0001")
    await page.get_by_role("button", name="Add code").click()
    await page.get_by_text("The page must be 07").wait_for()
    await shot(page, "04-host-setup")
    await page.get_by_role("button", name="Add device").click()
    await page.wait_for_function("location.hash.startsWith('#/devices/living_room_box')")
    cfg = await draft_has(site, lambda c: any(d.get("kind") == "bt" for d in c.get("devices", [])))
    dev = cfg["devices"][0]
    assert dev["address"] == BOX and dev["name"] == "Living room box" and dev["transport"] == "auto"
    assert dev["commands"]["F1"] == "F1" and dev["commands"]["Netflix"] == "HID:0C:0223" and "Home" in dev["commands"]
    # the device page has no IR parts
    await page.get_by_role("heading", name="Connection").wait_for()
    assert await page.get_by_text("Power control").count() == 0
    await shot(page, "05-device-page")
    assert clean(problems) == []


async def test_a_host_already_paired_but_not_in_the_config_is_added(browser, bt, site):
    bt.add_host(configured=False, result=False)
    bt.add_remote(configured=False)
    page, _ = await open_bt(browser, site)
    await page.get_by_text("Living room box").first.wait_for()
    assert await page.get_by_text("Not in your config").count() == 2
    await shot(page, "06-panel-paired")
    await page.get_by_role("link", name="Add to config").first.click()
    await page.get_by_role("heading", name="Name it and choose its commands").wait_for()
    await page.get_by_role("button", name="Add device").click()
    cfg = await draft_has(site, lambda c: bool(c.get("devices")))
    assert cfg["devices"][0]["address"] == BOX
    # back in the panel the host now shows as part of the config
    await page.evaluate("location.hash = '#/bluetooth'")
    await page.get_by_text("In your config").first.wait_for()


async def test_pair_a_remote_from_candidates_and_learn_a_key(browser, bt, site):
    bt.candidates({"addr": REMOTE, "name": "Homatics Remote", "rssi": -48}, {"addr": "11:22:33:44:55:02", "rssi": -80})
    page, problems = await open_bt(browser, site)
    await page.get_by_role("link", name="Add Bluetooth remote").click()
    await shot(page, "07-remote-start")
    await page.get_by_role("button", name="Scan for remotes").click()
    await page.get_by_role("list", name="Remotes found").wait_for()
    assert await page.get_by_role("img", name="Signal strong").count() == 1
    assert await page.get_by_role("img", name="Signal weak").count() == 1
    await page.get_by_text("Unnamed remote").wait_for()
    await shot(page, "08-remote-candidates")
    await page.get_by_role("button", name="Pair with Homatics Remote").click()
    await page.get_by_role("heading", name="Choose what its buttons do").wait_for()
    await page.get_by_label("Key for Home", exact=True).wait_for()
    # learn mode: press a button on the "real" remote
    await page.get_by_role("button", name="Learn a button").click()
    await page.get_by_text("Listening. Press any button").wait_for()
    await page.wait_for_function("true")
    for _ in range(40):
        if bt.listen:
            break
        await asyncio.sleep(0.1)
    assert bt.listen
    bt.press(12, 0x223)
    await page.get_by_text("You pressed").wait_for()
    await page.get_by_text("→ acts as").wait_for()  # Home is a default usage
    await page.get_by_label("Key for Home", exact=True).last.fill("menu")
    bt.press(12, 0x9999 % 0x400)  # a usage nobody mapped
    await page.get_by_text("Not mapped").first.wait_for()
    await shot(page, "09-remote-learn")
    await page.get_by_role("button", name="Stop listening").click()
    await page.get_by_role("button", name="Add remote").click()
    cfg = await draft_has(site, lambda c: bool(c.get("bt_remotes")))
    r = cfg["bt_remotes"][0]
    assert r["address"] == REMOTE and r["name"] == "Homatics Remote" and r["keys"] == {"Home": "menu"}
    assert any(c["op"] == "pick" and c["addr"] == REMOTE for c in bt.ran)
    # the panel offers the key editor of a configured remote
    await page.get_by_role("link", name="Edit buttons").first.click()
    await page.get_by_role("table", name="Remote buttons and their keys").wait_for()
    await shot(page, "10-remote-keys")
    assert clean(problems) == []


async def test_forget_asks_in_the_page_first_and_test_fire_sends_a_key(browser, bt, site):
    bt.add_host(result=False)
    page, _ = await open_bt(browser, site)
    await page.get_by_text("Living room box").first.wait_for()
    await page.get_by_role("button", name="Send", exact=True).click()
    await page.get_by_text("Sent DirectionUp.").wait_for()  # the first command of the default set
    key = next(c for c in bt.ran if c["op"] == "key")
    assert (key["page"], key["usage"], key["action"], key["slot"]) == (7, 0x52, "press", 0)
    await page.get_by_role("button", name="Forget Living room box").click()
    dialog = page.get_by_role("dialog")
    await dialog.get_by_text("Forget Living room box?").wait_for()
    await shot(page, "11-forget-confirm")
    await page.get_by_role("button", name="Cancel", exact=True).click()
    await dialog.wait_for(state="hidden")
    assert not any(c["op"] == "forget" for c in bt.ran)
    await page.get_by_role("button", name="Forget Living room box").click()
    await page.get_by_role("button", name="Forget", exact=True).click()
    await page.get_by_text("No device is paired yet").wait_for()
    assert any(c["op"] == "forget" and c["kind"] == "host" for c in bt.ran)


async def test_unsupported_offline_and_unknown_say_what_to_do(browser, site):
    # unknown: nothing has reported yet
    page, _ = await open_bt(browser, site)
    await page.get_by_text("Home Assistant has not reported Bluetooth yet").wait_for()
    assert await page.get_by_role("link", name="Add Bluetooth device").count() == 0
    await shot(page, "12-state-unknown")
    f = FakeIntegration(site)
    f.supported = False
    await f.start()
    try:
        await page.get_by_text("This hub's firmware has no Bluetooth yet").wait_for()
        await shot(page, "13-state-unsupported")
        f.supported, f.online = True, False
        await page.get_by_text("The hub is offline").wait_for(timeout=8000)
        await shot(page, "14-state-offline")
        f.online = True
        f.state["state"] = "no_adapter"
        await page.get_by_text("No adapter", exact=True).first.wait_for(timeout=8000)
        await page.get_by_text("cannot find a Bluetooth adapter").wait_for()
        assert await page.get_by_role("button", name="Add Bluetooth device").is_disabled()
        await shot(page, "15-state-no-adapter")
    finally:
        await f.stop()


async def test_errors_are_sentences_and_pairing_results_say_what_next(browser, bt, site):
    page, _ = await open_bt(browser, site, "bluetooth/host")
    bt.refuse["pair"] = ("bt_not_ready", "adapter busy")
    await page.get_by_role("button", name="Start pairing").click()
    await page.get_by_text("still starting").wait_for()
    assert await page.get_by_text("bt_not_ready").count() == 0
    await shot(page, "16-error-not-ready")
    del bt.refuse["pair"]
    await page.get_by_role("button", name="Start pairing").click()
    await page.get_by_role("timer").wait_for()
    # the device refuses the pairing
    bt.close_window()
    bt.state["pairing"] = {"result": "refused", "addr": BOX, "kind": "host"}
    bt.state["gen"] += 1
    await page.get_by_role("alert").get_by_text("The device refused to pair.").wait_for()
    await page.get_by_text("remove any old \"Harmony\" entry").wait_for()
    await shot(page, "17-result-refused")
    await page.get_by_role("button", name="Try again").click()
    await page.get_by_role("button", name="Start pairing").click()
    await page.get_by_role("timer").wait_for()
    bt.close_window()  # the window closes with nothing paired
    bt.state["pairing"] = {"result": "failed", "kind": "host"}
    await page.get_by_role("alert").get_by_text("Pairing did not finish.").wait_for()
    await shot(page, "18-result-failed")


async def test_bt_commands_work_in_the_palette_and_key_assignment(browser, bt, site):
    await seed(
        site,
        """
devices:
  - id: shield
    kind: bt
    name: Shield
    address: "AA:BB:CC:DD:EE:01"
    commands: {Home: Home, Vol up: VolumeUp}
  - id: tvbox
    kind: bt
    name: TV box
activities:
  - id: watch
    name: Watch
    devices: [{device: shield}]
    keys: {select: {device: shield, command: Home}}
""",
    )
    page, problems = await open_bt(browser, site, "remote")
    await page.get_by_text("Shield").first.wait_for()
    await page.get_by_text("Vol up").first.wait_for()  # chips of the explicit set
    await page.get_by_text("DirectionUp").first.wait_for()  # the default set of "TV box" (it lists no commands)
    await shot(page, "19-palette-bt-commands")
    await page.evaluate("location.hash = '#/devices'")
    await page.get_by_text("Bluetooth", exact=True).first.wait_for()
    await shot(page, "20-devices-gallery")
    await page.evaluate("location.hash = '#/devices/tvbox'")
    await page.get_by_role("heading", name="Commands").wait_for()
    await page.get_by_text("Not paired yet").first.wait_for()
    assert clean(problems) == []
