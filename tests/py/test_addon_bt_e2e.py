# ruff: noqa: F811
"""The editor's Bluetooth views end to end: the real add-on and the built bundle in Chromium, with a scripted fake
integration on the channel WebSocket standing in for Home Assistant and the hub. Skipped like test_addon_e2e.py.
Set HH_BT_SHOTS=<dir> to write a screenshot of every screen at 1440x900 and 390x844:
    HH_CHROMIUM=/usr/bin/google-chrome uv run pytest tests/py/test_addon_bt_e2e.py -n0
"""

import asyncio
import os
import re
from pathlib import Path

import pytest
from test_addon_e2e import DIST, TOKEN, Channel, H, api, browser, open_editor, seed  # noqa: F401

from harmony_addon.app import create_app
from harmony_addon.auth import NoAuth
from harmony_addon.options import Options
from harmony_core.bt_codes import DEFAULT_COMMANDS

pytestmark = pytest.mark.skipif(not (DIST / "index.html").is_file(), reason="frontend not built")
SHOTS = os.environ.get("HH_BT_SHOTS")
BEARER = {"Authorization": f"Bearer {TOKEN}"}
LIMITS = {
    "hosts_max": 8,
    "conn_max": 2,
    "remotes_max": 4,
    "le_bonds_max": 8,
    "pages": [7, 12],
    "le": True,
}
BOX = "AA:BB:CC:DD:EE:01"
REMOTE = "11:22:33:44:55:01"


@pytest.fixture
async def site(aiohttp_server, tmp_path):
    app = create_app(
        tmp_path,
        options=Options("info"),
        auth=NoAuth(),
        static_dir=DIST,
        bundle_token=TOKEN,
        bundle_networks=None,
    )
    server = await aiohttp_server(app)
    return f"http://127.0.0.1:{server.port}"


class FakeIntegration:
    """Plays Home Assistant: speaks the add-on's channel like the real one, runs the editor's commands on a scripted hub."""

    def __init__(self, origin):
        self.origin = origin
        self.supported = True
        self.online = True
        self.state = {"state": "ready", "discoverable": False, "gen": 1, "hosts": [], "remotes": []}
        self.refuse: dict[str, tuple[str, str]] = {}  # op -> (hub code, message)
        self.ran: list[dict] = []
        self.out_events: list[dict] = []
        self.ch: Channel | None = None
        self.listen = False
        self._task: asyncio.Task | None = None
        self._candidates: list[dict] = []

    async def start(self):
        self.ch = await Channel(self.origin).open()  # hello: the hub id is now linked
        await self.push()
        self._task = asyncio.create_task(self._loop())

    async def stop(self):
        if self._task:
            self._task.cancel()
        await self.ch.close()

    def push(self):
        """Report the hub's Bluetooth state (what the integration does on every change)."""
        frame = {"online": self.online, "supported": self.supported}
        if self.supported:
            frame |= {"state": self.state, "limits": LIMITS}
        return asyncio.ensure_future(self.ch.send("bt_state", **frame))

    async def _loop(self):
        """Run the editor's commands (`bt_cmd`) on the scripted hub and answer `bt_result`; follow `listen`."""
        while True:
            msg = await self.ch.recv(timeout=3600)
            if msg["t"] == "bt_cmd":
                res = self._run({k: v for k, v in msg.items() if k != "t"})
                await self.ch.send(
                    "bt_result", **res, **({"state": self.state} if self.supported else {})
                )
            elif msg["t"] == "listen":
                self.listen = msg["on"] is True
                for ev in self.out_events if self.listen else ():
                    await self.ch.send("bt_key", **ev)
                self.out_events = []

    def _run(self, cmd):
        self.ran.append(cmd)
        op = cmd["op"]
        if op in self.refuse:
            code, msg = self.refuse[op]
            return {"id": cmd["id"], "ok": False, "code": code, "msg": msg}
        st = self.state
        if op == "pair":
            st.pop("pairing", None)  # the hub clears the last outcome when a window opens
            st.update(
                discoverable=cmd.get("mode", "host") == "host",
                window=cmd.get("mode", "host"),
                discoverable_secs=cmd.get("secs", 120),
            )
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
        entry = {
            "slot": len(self.state["hosts"]),
            "addr": addr,
            "name": name,
            "transport": "le",
            "connected": True,
        }
        if configured is not None:
            entry["configured"] = configured
        self.state["hosts"].append(entry)
        if result:
            self.state["pairing"] = {
                "result": "ok",
                "addr": addr,
                "name": name,
                "transport": "le",
                "kind": "host",
            }
        self.close_window()
        self.state["gen"] += 1
        self.push()

    def add_remote(self, addr=REMOTE, name="Homatics Remote", configured=None):
        entry = {
            "slot": len(self.state["remotes"]),
            "addr": addr,
            "name": name,
            "connected": True,
            "battery": 82,
        }
        if configured is not None:
            entry["configured"] = configured
        self.state["remotes"].append(entry)
        self.state["pairing"] = {"result": "ok", "addr": addr, "name": name, "kind": "remote"}
        self.close_window()
        self.state["gen"] += 1
        self.push()

    def candidates(self, *c):
        self._candidates = list(c)
        if self.state.get("window") == "remote":
            self.state["candidates"] = list(c)
            self.state["gen"] += 1
            self.push()

    def press(self, page, usage, slot=0):
        for down in (True, False):
            asyncio.ensure_future(
                self.ch.send("bt_key", slot=slot, page=page, usage=usage, down=down)
            )


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
    _, got = await api(site, "GET", "/api/v1/configs/main")
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
    await page.get_by_role("radio", name="Automatic (recommended)").wait_for()
    await shot(page, "02-host-start")
    await page.get_by_role("button", name="Start Pairing").click()
    await page.get_by_role("timer").wait_for()
    await page.get_by_text("pair with “Harmony …”").wait_for()
    assert await page.get_by_role("timer").inner_text() != ""
    assert any(c["op"] == "pair" and c["transport"] == "any" for c in bt.ran)
    await shot(page, "03-host-waiting")

    bt.add_host()  # the TV box pairs
    await page.get_by_role("heading", name="Name It and Choose Its Commands").wait_for()
    focused = await page.evaluate(
        "document.querySelector('hh-app').shadowRoot.querySelector('hh-bluetooth').shadowRoot.querySelector('hh-bt-pair').shadowRoot.activeElement?.id"
    )
    assert focused == "step-title"  # focus moved into the new step
    # the paired device is in the draft already (address and the transport it paired on), before the form is finished
    saved = await draft_has(
        site,
        lambda c: any(
            d.get("kind") == "bt" and d.get("address") == BOX for d in c.get("devices", [])
        ),
    )
    assert saved["devices"][0]["transport"] == "le"
    await page.get_by_text(
        f"{len(DEFAULT_COMMANDS)} commands chosen"
    ).wait_for()  # the default set is preselected
    await page.get_by_label("Name", exact=True).fill("Living room box")
    await page.get_by_label("Search commands").fill("F1")
    f1 = page.get_by_role("button", name=re.compile(r"^F1 "))
    await f1.click()
    assert await f1.get_attribute("aria-pressed") == "true"
    await page.get_by_label("Command Name").fill("Netflix")
    await page.get_by_label("HID Code").fill("HID:0C:0223")
    await page.get_by_role("button", name="Add Code").click()
    await page.get_by_label("Command Name").fill("Bad")
    await page.get_by_label("HID Code").fill("HID:09:0001")
    await page.get_by_role("button", name="Add Code").click()
    await page.get_by_text("The page must be 07").wait_for()
    await shot(page, "04-host-setup")
    await page.get_by_role("button", name="Add Device").click()
    await page.wait_for_function("location.hash.startsWith('#/devices/living_room_box')")
    cfg = await draft_has(
        site,
        lambda c: any(
            d.get("kind") == "bt" and "Netflix" in d.get("commands", {})
            for d in c.get("devices", [])
        ),
    )
    dev = cfg["devices"][0]
    assert dev["address"] == BOX and dev["name"] == "Living room box" and dev["transport"] == "le"
    assert len(cfg["devices"]) == 1  # the form edited the entry made at pairing
    assert (
        dev["commands"]["F1"] == "F1"
        and dev["commands"]["Netflix"] == "HID:0C:0223"
        and "Home" in dev["commands"]
    )
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
    await page.get_by_role("link", name="Add to Config").first.click()
    await page.get_by_role("heading", name="Name It and Choose Its Commands").wait_for()
    await page.get_by_role("button", name="Add Device").click()
    cfg = await draft_has(site, lambda c: bool(c.get("devices")))
    assert cfg["devices"][0]["address"] == BOX
    # back in the panel the host now shows as part of the config
    await page.evaluate("location.hash = '#/bluetooth'")
    await page.get_by_text("In Your Config").first.wait_for()


async def test_pair_a_remote_from_candidates_and_learn_a_key(browser, bt, site):
    bt.candidates(
        {"addr": REMOTE, "name": "Homatics Remote", "rssi": -48},
        {"addr": "11:22:33:44:55:02", "rssi": -80},
    )
    page, problems = await open_bt(browser, site)
    await page.get_by_role("link", name="Add Bluetooth remote").click()
    await shot(page, "07-remote-start")
    await page.get_by_role("button", name="Scan for Remotes").click()
    await page.get_by_role("list", name="Remotes found").wait_for()
    assert await page.get_by_role("img", name="Signal strong").count() == 1
    assert await page.get_by_role("img", name="Signal weak").count() == 1
    await page.get_by_text("Unnamed remote").wait_for()
    await shot(page, "08-remote-candidates")
    await page.get_by_role("button", name="Pair with Homatics Remote").click()
    await page.get_by_role("heading", name="Choose What Its Buttons Do").wait_for()
    paired = await draft_has(
        site, lambda c: bool(c.get("bt_remotes"))
    )  # saved to the draft on pairing, before any key
    assert paired["bt_remotes"] == [{"name": "Homatics Remote", "address": REMOTE}]
    await page.get_by_label("Key for Home", exact=True).wait_for()
    # learn mode: press a button on the "real" remote
    await page.get_by_role("button", name="Learn a Button").click()
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
    await page.get_by_label("Key for Home", exact=True).last.select_option(label="Menu")
    bt.press(12, 0x9999 % 0x400)  # a usage nobody mapped
    await page.get_by_text("Not mapped").first.wait_for()
    await shot(page, "09-remote-learn")
    await page.get_by_role("button", name="Stop Listening").click()
    await page.get_by_role("button", name="Add Remote").click()
    cfg = await draft_has(
        site, lambda c: bool(c.get("bt_remotes") and c["bt_remotes"][0].get("keys"))
    )
    assert len(cfg["bt_remotes"]) == 1
    r = cfg["bt_remotes"][0]
    assert (
        r["address"] == REMOTE and r["name"] == "Homatics Remote" and r["keys"] == {"Home": "menu"}
    )
    assert any(c["op"] == "pick" and c["addr"] == REMOTE for c in bt.ran)
    # the panel offers the key editor of a configured remote
    await page.get_by_role("link", name="Edit Buttons").first.click()
    await page.get_by_role("list", name="Remote buttons and their keys").wait_for()
    await shot(page, "10-remote-keys")
    # phone width: a key row is two lines (name, usage and Default/Reset, then the picker) and nothing scrolls sideways
    await page.set_viewport_size({"width": 390, "height": 844})
    await page.wait_for_timeout(200)
    rows = await page.evaluate(
        """() => {
          const find = (root, sel) => { const f = root.querySelector(sel); if (f) return f;
            for (const e of root.querySelectorAll('*')) if (e.shadowRoot) { const r = find(e.shadowRoot, sel); if (r) return r; } return null; };
          const kr = find(document, '.kr');
          const tops = [...kr.children].map((c) => Math.round(c.getBoundingClientRect().top));
          return { h: kr.getBoundingClientRect().height, tops, over: document.documentElement.scrollWidth - window.innerWidth };
        }"""
    )
    assert rows["over"] <= 0
    name, use, pick, end = rows["tops"]  # DOM order: name, usage, picker, Default/Reset
    assert abs(name - use) <= 6 and abs(name - end) <= 6 and pick > name + 14, rows
    assert rows["h"] < 90, rows
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
        f.push()
        await page.get_by_text("The hub is offline").wait_for(timeout=8000)
        await shot(page, "14-state-offline")
        f.online = True
        f.state["state"] = "no_adapter"
        f.push()
        await page.get_by_text("No Adapter", exact=True).first.wait_for(timeout=8000)
        await page.get_by_text("cannot find a Bluetooth adapter").wait_for()
        assert await page.get_by_role("button", name="Add Bluetooth device").is_disabled()
        await shot(page, "15-state-no-adapter")
    finally:
        await f.stop()


async def test_errors_are_sentences_and_pairing_results_say_what_next(browser, bt, site):
    page, _ = await open_bt(browser, site, "bluetooth/host")
    bt.refuse["pair"] = ("bt_not_ready", "adapter busy")
    await page.get_by_role("button", name="Start Pairing").click()
    await page.get_by_text("still starting").wait_for()
    assert await page.get_by_text("bt_not_ready").count() == 0
    await shot(page, "16-error-not-ready")
    del bt.refuse["pair"]
    await page.get_by_role("button", name="Start Pairing").click()
    await page.get_by_role("timer").wait_for()
    # the device refuses the pairing
    bt.close_window()
    bt.state["pairing"] = {"result": "refused", "addr": BOX, "kind": "host"}
    bt.state["gen"] += 1
    bt.push()
    await page.get_by_role("alert").get_by_text("The device refused to pair.").wait_for()
    await page.get_by_text('remove any old "Harmony" entry').wait_for()
    await shot(page, "17-result-refused")
    await page.get_by_role("button", name="Try Again").click()
    await page.get_by_role("button", name="Start Pairing").click()
    await page.get_by_role("timer").wait_for()
    bt.close_window()  # the window closes with nothing paired
    bt.state["pairing"] = {"result": "failed", "kind": "host"}
    bt.state["gen"] += 1  # the hub bumps gen with every outcome
    bt.push()
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
    await page.get_by_text("Volume Up").first.wait_for()  # chips of the explicit set
    await page.get_by_text(
        "Up", exact=True
    ).first.wait_for()  # the default set of "TV box" (it lists no commands)
    await shot(page, "19-palette-bt-commands")
    await page.evaluate("location.hash = '#/devices'")
    await page.get_by_text("Bluetooth", exact=True).first.wait_for()
    await shot(page, "20-devices-gallery")
    await page.evaluate("location.hash = '#/devices/tvbox'")
    await page.get_by_role("heading", name="Commands").wait_for()
    await page.get_by_text("Not Paired yet").first.wait_for()
    assert clean(problems) == []


ACTIVE = """(() => { let e = document.activeElement; while (e && e.shadowRoot && e.shadowRoot.activeElement) e = e.shadowRoot.activeElement;
  return e ? ((e.getAttribute('aria-label') || e.innerText || e.id || e.tagName) + '').trim() : ''; })()"""


async def tab_to(page, text, limit=250):
    """Press Tab until the focused control's name contains `text`."""
    for _ in range(limit):
        await page.keyboard.press("Tab")
        if text in await page.evaluate(ACTIVE):
            return
    raise AssertionError(f"never reached {text!r} by keyboard; at {await page.evaluate(ACTIVE)!r}")


async def test_the_host_wizard_works_from_the_keyboard_alone(browser, bt, site):
    page, _ = await open_bt(browser, site)
    await page.get_by_text("Ready", exact=True).first.wait_for()
    await tab_to(page, "Add Bluetooth device")
    await page.keyboard.press("Enter")
    await page.get_by_role("button", name="Start Pairing").wait_for()
    await tab_to(page, "Start Pairing")
    await page.keyboard.press("Enter")
    await page.get_by_role("timer").wait_for()
    bt.add_host()
    await page.get_by_role("heading", name="Name It and Choose Its Commands").wait_for()
    assert (
        await page.evaluate(ACTIVE) == "Name It and Choose Its Commands"
    )  # focus moved to the new step
    await tab_to(page, "Add Device")
    await page.keyboard.press("Enter")
    cfg = await draft_has(site, lambda c: bool(c.get("devices")))
    assert cfg["devices"][0]["address"] == BOX


async def test_the_remote_wizard_works_from_the_keyboard_alone(browser, bt, site):
    bt.candidates({"addr": REMOTE, "name": "Homatics Remote", "rssi": -48})
    page, _ = await open_bt(browser, site)
    await page.get_by_text("Ready", exact=True).first.wait_for()
    await tab_to(page, "Add Bluetooth remote")
    await page.keyboard.press("Enter")
    await page.get_by_role("button", name="Scan for Remotes").wait_for()
    await tab_to(page, "Scan for Remotes")
    await page.keyboard.press("Enter")
    await page.get_by_role("list", name="Remotes found").wait_for()
    await tab_to(page, "Pair with Homatics Remote")
    await page.keyboard.press("Enter")
    await page.get_by_role("heading", name="Choose What Its Buttons Do").wait_for()
    await tab_to(page, "Key for Back")
    await page.keyboard.press("ArrowDown")  # a select: change it from the keyboard
    await tab_to(page, "Add Remote")
    await page.keyboard.press("Enter")
    cfg = await draft_has(
        site, lambda c: bool(c.get("bt_remotes") and c["bt_remotes"][0].get("keys"))
    )
    assert cfg["bt_remotes"][0]["address"] == REMOTE and "Back" in cfg["bt_remotes"][0].get(
        "keys", {}
    )


async def test_a_default_can_be_set_to_does_nothing_and_the_picker_has_human_names(
    browser, bt, site
):
    await seed(site, "bt_remotes: [{name: R}]\n")
    bt.add_remote(configured=True)
    page, _ = await open_bt(browser, site, "bluetooth/keys/0")
    sel = page.get_by_label("Key for Back", exact=True)
    await sel.wait_for()
    opts = await sel.locator("option").all_inner_texts()
    assert "Does Nothing" in opts and "Digit 0" in opts and "Volume Up" in opts and "up" not in opts
    assert await sel.locator("optgroup").evaluate_all("g => g.map(x => x.label)") == [
        "Navigation",
        "Transport",
        "Volume and channel",
        "Colour",
        "Digits",
        "Other",
    ]
    await sel.select_option(label="Does Nothing")
    cfg = await draft_has(
        site, lambda c: (c.get("bt_remotes") or [{}])[0].get("keys", {}).get("Back", 1) is None
    )
    assert cfg["bt_remotes"][0]["keys"] == {"Back": None}
    await page.get_by_role("button", name="Reset Back to Exit").click()
    await draft_has(
        site, lambda c: "keys" not in c["bt_remotes"][0] or not c["bt_remotes"][0]["keys"]
    )


async def test_nothing_overflows_at_390(browser, bt, site):
    bt.add_host(configured=False, result=False)
    bt.add_remote(configured=False)
    for route in (
        "bluetooth",
        "bluetooth/host",
        "bluetooth/remote",
        f"bluetooth/remote/{REMOTE}",
        f"bluetooth/host/{BOX}",
    ):
        page = await open_editor(browser, site, [], viewport={"width": 390, "height": 844})
        await page.evaluate(f"location.hash = '#/{route}'")
        await page.wait_for_timeout(1200)
        over = await page.evaluate("document.documentElement.scrollWidth - window.innerWidth")
        assert over <= 0, f"{route} overflows by {over}px"
        await page.close()
