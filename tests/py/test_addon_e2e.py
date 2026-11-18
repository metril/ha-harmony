"""Headless-browser end-to-end: the real aiohttp app + the real built bundle, under an ingress prefix.

Skipped unless playwright is importable, a Chromium can be launched (HH_CHROMIUM=/path/to/chrome
overrides the lookup) and addon/frontend/dist exists (`npm run build`). CI does not install
playwright, so this runs on developer machines:
    HH_CHROMIUM=/usr/bin/google-chrome uv run --with playwright python -m pytest tests/py/test_addon_e2e.py -n0
Set HH_SHOTS=<dir> to also write light/dark screenshots at 1280 and 400 px.
"""

import json
import os
from pathlib import Path

import aiohttp
import pytest
from test_addon import SMALL

from harmony_addon.app import create_app
from harmony_addon.auth import NoAuth
from harmony_addon.options import Options

pw = pytest.importorskip("playwright.async_api")
DIST = Path(__file__).resolve().parents[2] / "addon" / "frontend" / "dist"
pytestmark = pytest.mark.skipif(not (DIST / "index.html").is_file(), reason="frontend not built")

PREFIX = "/api/hassio_ingress/e2eTok_en-1"
TOKEN = "ab" * 32
H = {"X-HH-CSRF": "1"}
YAML = """
devices:
  - id: tv
    name: TV
    power: {on: {proto: nec, addr: 4, cmd: 2}, off: {proto: nec, addr: 4, cmd: 3}}
    commands: {menu: {proto: nec, addr: 4, cmd: 20}}
"""
SHOTS = os.environ.get("HH_SHOTS")
# what the Supervisor ingress proxy sends with every request
INGRESS_HEADERS = {"X-Remote-User-Id": "test-user", "X-Ingress-Path": PREFIX}


@pytest.fixture
async def site(aiohttp_server, tmp_path):
    app = create_app(
        tmp_path,
        options=Options("info"),
        auth=NoAuth(),
        static_dir=DIST,
        bundle_token=TOKEN,
        bundle_networks=None,
        wait_seconds=1.0,
    )
    server = await aiohttp_server(app)
    return f"http://127.0.0.1:{server.port}"


@pytest.fixture
async def browser():
    async with pw.async_playwright() as p:
        try:
            b = await p.chromium.launch(executable_path=os.environ.get("HH_CHROMIUM"))
        except pw.Error as exc:  # no browser binary on this machine
            pytest.skip(f"cannot launch chromium: {exc}")
        yield b
        await b.close()


async def open_editor(browser, origin, problems=None, **kw):
    page = await browser.new_page(**kw)
    if problems is not None:
        page.on("console", lambda m: problems.append(m.text) if m.type == "error" else None)
        page.on("pageerror", lambda e: problems.append(str(e)))

    async def to_real_server(
        route,
    ):  # the ingress proxy: strip the prefix, add what Supervisor adds
        await route.continue_(
            url=route.request.url.replace(PREFIX, "", 1),
            headers={**route.request.headers, **INGRESS_HEADERS},
        )

    await page.route(f"{origin}{PREFIX}/**", to_real_server)
    await page.goto(f"{origin}{PREFIX}/")
    return page


async def api(origin, method, path, **kw):
    if "/bundle" not in path:  # bearer routes are not ingress routes
        kw["headers"] = {**INGRESS_HEADERS, **kw.get("headers", {})}
    async with aiohttp.ClientSession() as s, s.request(method, origin + path, **kw) as r:
        return r.status, await r.json()


async def ack(origin, result="ok", detail=None):
    """What the integration does after pulling the bundle: GET it, then POST /bundle/ack."""
    auth = {"Authorization": f"Bearer {TOKEN}"}
    status, bundle = await api(origin, "GET", "/api/v1/bundle?hub=default", headers=auth)
    assert status == 200
    body = {"hub": "default", "rev": bundle["rev"], "result": result}
    if detail:
        body["detail"] = detail
    status, _ = await api(origin, "POST", "/api/v1/bundle/ack", json=body, headers=auth)
    assert status == 200


async def seed(origin, *configs):
    """Apply each config in turn through the API (so history fills)."""
    from harmony_core.config_model import load_source

    for text in configs:
        cfg = load_source(text)
        assert (await api(origin, "PUT", "/api/v1/config/draft", json={"config": cfg}, headers=H))[
            0
        ] == 200
        assert (await api(origin, "POST", "/api/v1/config/apply", json={}, headers=H))[0] == 200


async def test_import_preview_apply_ack_under_an_ingress_prefix(browser, site):
    problems: list[str] = []
    page = await open_editor(browser, site, problems)
    await page.get_by_text("Set up your first hub config").wait_for()

    await page.get_by_role("button", name="Import YAML", exact=True).click()
    await page.locator("textarea").fill(YAML)
    await page.get_by_role("button", name="Import as draft").click()
    bar = page.get_by_role("region", name="Draft")
    await bar.get_by_text("1 change not applied").wait_for()
    await bar.get_by_text("Valid").wait_for()  # preview reported no errors
    await bar.get_by_role("group", name="Config", exact=False).wait_for()  # meter fed by usage
    assert "KiB" in await bar.locator(".meter").first.inner_text()

    await bar.get_by_role("button", name="Review & apply").click()
    await page.get_by_role("button", name="Apply to hub").click()
    await page.get_by_text("Saved as rev 1").wait_for()
    await page.get_by_text("Waiting for Home Assistant").wait_for()
    assert await page.get_by_text("(pending)").count() == 1
    if SHOTS:
        await shots(page, "apply-waiting")

    await ack(site)  # the integration pulled the bundle and applied it
    await page.get_by_text("(pending)").wait_for(state="detached", timeout=8000)
    await page.get_by_text("Remote pages updated").wait_for()
    await page.get_by_role("button", name="Done").click()
    await bar.get_by_text("No changes since rev 1").wait_for()
    # 404s for the favicon are fine; CSP violations and script errors are not
    assert [p for p in problems if "Failed to load resource" not in p] == []


async def test_a_second_tab_editing_the_draft_shows_the_conflict_banner(browser, site):
    await seed(site, SMALL)
    a = await open_editor(browser, site)
    b = await open_editor(browser, site)
    for p in (a, b):
        await p.get_by_text("No changes since rev 1").wait_for()
    edit = "document.querySelector('hh-app').store.edit(c => { c.devices[0].name = '%s'; })"
    await b.evaluate(edit % "From tab B")
    await b.get_by_text("saved just now").wait_for()
    await a.evaluate(edit % "From tab A")  # tab A still holds the old draft rev
    banner = a.get_by_role("alert").filter(has_text="Changed in another tab")
    await banner.wait_for()
    if SHOTS:
        await shots(a, "conflict")
    await banner.get_by_role("button", name="Reload").click()
    await banner.wait_for(state="detached")
    _, got = await api(site, "GET", "/api/v1/config")
    assert got["draft"]["config"]["devices"][0]["name"] == "From tab B"


async def test_waiting_message_after_30s_is_reachable_and_late_ack_still_completes(browser, site):
    await seed(site, SMALL)
    page = await open_editor(browser, site)
    await page.get_by_text("No changes since rev 1").wait_for()
    # an edit through the store, then apply it from the UI with a shortened wait
    await page.evaluate(
        "document.querySelector('hh-app').store.edit(c => { c.devices[0].settle_ms = 250; })"
    )
    await page.get_by_text("saved just now").wait_for()
    await page.evaluate(
        "document.querySelector('hh-app').shadowRoot.querySelector('hh-apply-dialog').waitMs = 500"
    )
    await page.get_by_role("button", name="Review & apply").click()
    await page.get_by_role("button", name="Apply to hub").click()
    await page.get_by_text("Home Assistant has not picked it up yet").wait_for(timeout=8000)
    await ack(site)
    await page.get_by_text("Remote pages updated").wait_for(timeout=8000)


async def shots(page, name):
    Path(SHOTS).mkdir(parents=True, exist_ok=True)
    for scheme in ("light", "dark"):
        await page.emulate_media(color_scheme=scheme)
        await page.wait_for_timeout(300)  # let colour transitions settle
        for width, height in ((1280, 800), (400, 800)):
            await page.set_viewport_size({"width": width, "height": height})
            await page.screenshot(path=f"{SHOTS}/{name}-{scheme}-{width}.png")
    await page.emulate_media(color_scheme="light")
    await page.set_viewport_size({"width": 1280, "height": 800})


async def test_screenshots(browser, site):
    if not SHOTS:
        pytest.skip("set HH_SHOTS to write screenshots")
    await seed(site, YAML, SMALL)
    page = await open_editor(browser, site)
    await page.get_by_text("No changes since rev 2").wait_for()
    await shots(page, "overview")
    await page.goto(f"{site}{PREFIX}/#/history")
    await page.get_by_text("Revert to rev 1").wait_for()
    await page.get_by_role("button", name="Compare").click()
    await page.get_by_text("Device").first.wait_for()
    await shots(page, "history")
    await page.goto(f"{site}{PREFIX}/#/devices/tv")
    await page.locator("hh-send-ir").first.wait_for()
    await shots(page, "devices")


STUDIO_YAML = """
devices:
  - id: tv
    name: Living room TV
    power: {on: {proto: nec, addr: 4, cmd: 2}, off: {proto: nec, addr: 4, cmd: 3}}
    inputs: {hdmi1: {proto: nec, addr: 4, cmd: 9}, hdmi2: {proto: nec, addr: 4, cmd: 10}}
    commands:
      menu: {proto: nec, addr: 4, cmd: 20}
      ok: {proto: nec, addr: 4, cmd: 34}
  - id: avr
    name: AV receiver
    power: {toggle: {proto: rc5, addr: 16, cmd: 12}}
    inputs: {stream: {proto: rc5, addr: 16, cmd: 57}}
    commands:
      vol_up: {proto: rc5, addr: 16, cmd: 16}
      vol_down: {proto: rc5, addr: 16, cmd: 17}
      mute: {proto: rc5, addr: 16, cmd: 13}
  - id: box
    name: Streaming box
    commands:
      play: {proto: sirc12, addr: 1, cmd: 26}
      pause: {proto: sirc12, addr: 1, cmd: 27}
activities:
  - id: watch_tv
    name: Watch TV
    devices: [{device: tv, input: hdmi1}, {device: avr}]
  - id: stream
    name: Stream
    devices: [{device: tv, input: hdmi2}, {device: avr, input: stream}, {device: box}]
keys:
  volume_up: {device: avr, command: vol_up}
  menu: {device: tv, command: menu}
pages:
  - id: home
    kind: home
    title: Activities
    cols: 4
    rows: 6
    widgets:
      - {type: button, id: a_tv, x: 0, y: 0, w: 4, h: 1, label: Watch TV, action: {activity: watch_tv}}
      - {type: button, id: a_st, x: 0, y: 1, w: 4, h: 1, label: Stream, action: {activity: stream}}
  - id: stream_ctl
    title: Stream
    activity: stream
    back: true
    cols: 4
    rows: 6
    widgets:
      - {type: button, id: play, x: 0, y: 0, w: 2, h: 1, label: Play, action: {device: box, command: play}}
"""


async def test_studio_import_assign_drag_inspect_apply(browser, site):
    problems: list[str] = []
    page = await open_editor(browser, site, problems)
    await page.get_by_text("Set up your first hub config").wait_for()
    await page.get_by_role("button", name="Import YAML", exact=True).click()
    await page.locator("textarea").fill(STUDIO_YAML)
    await page.get_by_role("button", name="Import as draft").click()

    # the studio is the default route: the remote with its home page on the screen
    screen = page.locator("hh-remote svg.scr")
    await screen.wait_for()
    await page.locator("hh-remote svg.scr text", has_text="Watch TV").first.wait_for()
    assert await page.locator("hh-remote [data-key]").count() == 35
    await page.get_by_role("tab", name="Default", exact=True).wait_for()

    # click-click: pick a command, then a hard key of the Stream layer
    await page.get_by_role("tab", name="Stream", exact=True).click()
    await page.locator("hh-palette .chip2", has_text="vol_down").first.click()
    await page.locator(".banner2", has_text="Click a key").wait_for()
    await page.locator("hh-remote [data-key=volume_down]").click()
    key = page.locator("hh-remote [data-key=volume_down]")
    await page.wait_for_function(
        "() => document.querySelector('hh-app').shadowRoot.querySelector('hh-studio').shadowRoot"
        ".querySelector('hh-remote').shadowRoot.querySelector('[data-key=volume_down]')"
        ".getAttribute('aria-label').includes('override')"
    )
    assert "avr: vol_down" in (await key.get_attribute("aria-label"))

    # X-ray labels every key with its action
    await page.get_by_role("button", name="X-ray").click()
    await page.locator("hh-remote text.xl", has_text="avr: vol_down").wait_for()
    await page.get_by_role("button", name="X-ray").click()

    # drag a command from the palette onto the screen: a button appears, the byte meter moves
    meter = page.locator("hh-page-strip .pg[aria-current=true]")
    before = await meter.get_attribute("aria-label")
    widgets = page.locator("hh-remote [data-widget]")
    n = await widgets.count()
    chip = page.locator("hh-palette .chip2", has_text="mute").first
    cb = await chip.bounding_box()
    sb = await screen.bounding_box()
    await page.mouse.move(cb["x"] + 10, cb["y"] + 10)
    await page.mouse.down()
    await page.mouse.move(cb["x"] + 60, cb["y"] + 40, steps=5)
    await page.mouse.move(sb["x"] + sb["width"] / 2, sb["y"] + sb["height"] * 0.75, steps=10)
    if SHOTS:
        await shots(page, "studio-mid-drag")
    await page.mouse.up()
    await page.wait_for_function(
        "n => document.querySelector('hh-app').shadowRoot.querySelector('hh-studio').shadowRoot"
        ".querySelector('hh-remote').shadowRoot.querySelectorAll('[data-widget]').length > n",
        arg=n,
    )
    await page.wait_for_function(
        "b => document.querySelector('hh-app').shadowRoot.querySelector('hh-studio').shadowRoot"
        ".querySelector('hh-page-strip').shadowRoot.querySelector('.pg[aria-current=true]').getAttribute('aria-label') !== b",
        arg=before,
    )

    # the new widget is selected: change its role and icon in the inspector, the preview re-renders
    insp = page.locator("hh-inspector")
    await insp.get_by_role("button", name="primary").click()
    await insp.get_by_role("button", name="vol_down", exact=True).click()
    await page.locator("hh-remote svg.scr g[transform]").first.wait_for()
    if SHOTS:
        await shots(page, "studio-inspector")
    await page.wait_for_timeout(900)  # autosave
    _, got = await api(site, "GET", "/api/v1/config")
    wid = got["draft"]["config"]["pages"][1]["widgets"][-1]
    assert wid["role"] == "primary" and wid["icon"] == "vol_down"
    assert wid["action"] == {"device": "avr", "command": "mute"}
    assert got["draft"]["config"]["activities"][1]["keys"]["volume_down"] == {
        "device": "avr",
        "command": "vol_down",
    }

    # undo is one step per action
    await page.locator("hh-studio button[aria-label=Undo]").click()
    await page.wait_for_timeout(200)

    await page.get_by_role("button", name="Review & apply").click()
    await page.get_by_role("button", name="Apply to hub").click()
    await page.get_by_text("Saved as rev").wait_for()
    assert [p for p in problems if "Failed to load resource" not in p] == []


@pytest.fixture
async def site_ir(aiohttp_server, tmp_path):
    """Like `site`, with the fixture IR archive (no network) and a recording IR sender."""
    from irdb_fake import FakeSession

    from harmony_irdb.archive import IrdbArchive

    class Sender:
        async def send(self, hub, action):
            return None

    app = create_app(
        tmp_path / "d",
        options=Options("info"),
        auth=NoAuth(),
        static_dir=DIST,
        bundle_token=TOKEN,
        bundle_networks=None,
        wait_seconds=1.0,
        irdb=IrdbArchive(FakeSession(), tmp_path / "irc"),
        ir_sender=Sender(),
    )
    server = await aiohttp_server(app)
    return f"http://127.0.0.1:{server.port}"


async def test_wizard_to_activity_to_xray_and_apply(browser, site_ir):
    site = site_ir
    problems: list[str] = []
    from harmony_core.config_model import load_source

    avr = (
        "devices:\n  - id: avr\n    name: AV receiver\n    power: {toggle: {proto: rc5, addr: 16, cmd: 12}}\n"
        "    commands: {vol_up: {proto: rc5, addr: 16, cmd: 16}, vol_down: {proto: rc5, addr: 16, cmd: 17}, mute: {proto: rc5, addr: 16, cmd: 13}}\n"
    )
    assert (
        await api(site, "PUT", "/api/v1/config/draft", json={"config": load_source(avr)}, headers=H)
    )[0] == 200
    page = await open_editor(browser, site, problems)
    await page.goto(f"{site}{PREFIX}/#/devices/new")
    await page.get_by_role("button", name="IR archive").click()
    await page.locator(".brand", has_text="Sony").first.click()
    await page.get_by_role("button", name="TV", exact=False).first.click()
    await page.get_by_text("Code 1 of 2").wait_for()
    await page.get_by_role("button", name="No, next code").click()
    await page.get_by_text("Code 2 of 2").wait_for()
    await page.get_by_role("button", name="Yes").click()
    await page.get_by_text("Likely matches").wait_for()
    await page.locator(".model", has_text="KDL-46V2501").first.click()
    await page.get_by_text("Review Sony KDL-46V2501").wait_for()
    await page.get_by_role("button", name="Add device").click()
    # in the gallery, and in the studio palette
    await page.get_by_text("Sony KDL-46V2501").first.wait_for()
    await page.goto(f"{site}{PREFIX}/#/devices")
    await page.locator(".dcard", has_text="Sony KDL-46V2501").wait_for()
    await page.goto(f"{site}{PREFIX}/#/remote")
    await page.locator("hh-palette").get_by_text("Sony KDL-46V2501").first.wait_for()
    # an activity with it; who handles volume fills the keys
    await page.goto(f"{site}{PREFIX}/#/activities")
    await page.get_by_role("button", name="New activity").click()
    await page.get_by_label("Activity name").fill("Watch TV")
    await page.get_by_label("Sony KDL-46V2501").check()
    await page.get_by_label("AV receiver").check()
    await page.get_by_role("button", name="Create").click()
    await page.get_by_text("Who handles it").wait_for()
    await page.get_by_role("button", name="Who handles volume").click()
    await page.get_by_role("menuitemradio", name="AV receiver").click()
    await page.get_by_text("Filled:").first.wait_for()
    # X-ray in the studio shows an activity key from the archive device
    await page.get_by_role("link", name="Open on remote").click()
    await page.get_by_role("button", name="X-ray").click()
    await page.locator("hh-remote").get_by_text("vol_up", exact=False).first.wait_for()
    bar = page.get_by_role("region", name="Draft")
    await bar.get_by_text("Valid").wait_for()
    await bar.get_by_role("button", name="Review & apply").click()
    await page.get_by_role("button", name="Apply to hub").click()
    await page.get_by_text("Saved as rev 1").wait_for()
    assert [p for p in problems if "Failed to load resource" not in p] == []


async def test_draft_survives_a_page_reload(browser, site):
    from harmony_core.config_model import load_source

    status, _ = await api(
        site, "PUT", "/api/v1/config/draft", json={"config": load_source(YAML)}, headers=H
    )
    assert status == 200
    page = await open_editor(browser, site)
    await page.goto(f"{site}{PREFIX}/#/devices/tv")
    name = page.get_by_label("Device name")
    await name.fill("Den TV")
    await name.dispatch_event("change")
    await page.wait_for_function(
        "() => document.querySelector('hh-app').store.saveState === 'saved'", timeout=8000
    )
    await page.reload()
    await page.get_by_label("Device name").wait_for()
    assert await page.get_by_label("Device name").input_value() == "Den TV"
    assert await page.evaluate("() => document.querySelector('hh-app').store.hasDraft") is True
    _, got = await api(site, "GET", "/api/v1/config")
    assert got["draft"]["config"]["devices"][0]["name"] == "Den TV"


async def test_wizard_likely_model_goes_candidates_to_model_files(
    browser, site_ir
):
    page = await open_editor(browser, site_ir)
    seen: list[str] = []
    page.on("request", lambda r: seen.append(r.url.split("/api/v1/")[-1]))
    await page.goto(f"{site_ir}{PREFIX}/#/devices/new")
    await page.get_by_role("button", name="IR archive").click()
    await page.locator(".brand", has_text="Sony").first.click()
    await page.get_by_role("button", name="TV", exact=False).first.click()
    await page.get_by_text("Code 1 of 2").wait_for()
    await page.get_by_role("button", name="No, next code").click()
    await page.get_by_role("button", name="Yes").click()
    await page.get_by_text("Likely matches").wait_for()
    cand = next(u for u in seen if u.startswith("ir/candidates"))
    assert "brand=" in cand and "type=" in cand
    # the candidate answer carried model_files: choosing a likely model asks for that file directly
    await page.locator(".model", has_text="KDL-46V2501").first.click()
    await page.get_by_text("Review Sony KDL-46V2501").wait_for()
    dev = [u for u in seen if u.startswith("ir/device")]
    assert len(dev) == 1 and "model=" in dev[0], (dev, seen)
    assert dev[0].endswith("model=KDL-46V2501.json"), dev  # the archive file from model_files
    assert seen.index(cand) < seen.index(dev[0])


HA_STATES = [
    {"entity_id": "light.kitchen", "state": "on", "attributes": {"friendly_name": "Kitchen"}},
    {"entity_id": "light.attic", "state": "off", "attributes": {"friendly_name": "Attic lamp"}},
    {"entity_id": "switch.fan", "state": "off", "attributes": {"friendly_name": "Fan"}},
]
HA_SERVICES = [
    {
        "domain": "light",
        "services": {
            "toggle": {"name": "Toggle", "fields": {"brightness_pct": {}}},
            "turn_on": {"name": "Turn on", "fields": {"brightness_pct": {}}},
            "turn_off": {"name": "Turn off", "fields": {}},
        },
    },
    {"domain": "switch", "services": {"toggle": {}, "turn_on": {}, "turn_off": {}}},
]


class FakeHaApi:
    """The e2e seam for Home Assistant: a few entities and services, GET only like the real client."""

    async def get(self, path):
        return {"/states": HA_STATES, "/services": HA_SERVICES}[path]


@pytest.fixture
async def site_ha(aiohttp_server, tmp_path):
    app = create_app(
        tmp_path,
        options=Options("info"),
        auth=NoAuth(),
        static_dir=DIST,
        bundle_token=TOKEN,
        bundle_networks=None,
        wait_seconds=1.0,
        ha_api=FakeHaApi(),
    )
    server = await aiohttp_server(app)
    return f"http://127.0.0.1:{server.port}"


FLOW_FIXTURE = Path(__file__).parent / "fixtures" / "ha_action_flow.json"


async def test_ha_action_key_and_tile_flow_reaches_the_bundle(browser, site_ha):
    site = site_ha
    problems: list[str] = []
    page = await open_editor(browser, site, problems)
    await page.get_by_text("Set up your first hub config").wait_for()
    await page.get_by_role("button", name="Import YAML", exact=True).click()
    await page.locator("textarea").fill(STUDIO_YAML)
    await page.get_by_role("button", name="Import as draft").click()
    await page.locator("hh-remote svg.scr").wait_for()

    # a hard key of the Stream layer -> Home Assistant action -> light.toggle on light.kitchen
    await page.get_by_role("tab", name="Stream", exact=True).click()
    await page.locator("hh-remote [data-key=channel_up]").click()
    insp = page.locator("hh-inspector")
    await insp.get_by_role("button", name="Home Assistant action").click()
    await insp.locator("label", has_text="Service").locator("select").select_option("light.toggle")
    await insp.get_by_placeholder("Search entities").fill("light.kitchen")
    await insp.get_by_role("button", name="Set action").click()
    await page.get_by_role("button", name="X-ray").click()
    await page.locator("hh-remote text.xl", has_text="Kitchen · toggle").wait_for()
    await page.get_by_role("button", name="X-ray").click()

    # drag a light from the palette's Home tab onto the screen: a tile with the automatic tap
    await page.get_by_role("tab", name="Home", exact=True).click()
    ent = page.locator("hh-palette .ent", has_text="light.kitchen").first
    await ent.wait_for()
    eb = await ent.bounding_box()
    sb = await page.locator("hh-remote svg.scr").bounding_box()
    await page.mouse.move(eb["x"] + 10, eb["y"] + 10)
    await page.mouse.down()
    await page.mouse.move(eb["x"] + 60, eb["y"] + 40, steps=5)
    await page.mouse.move(sb["x"] + sb["width"] / 2, sb["y"] + sb["height"] * 0.8, steps=10)
    await page.mouse.up()
    await insp.get_by_text("Automatic: a tap runs light.toggle").wait_for()
    await page.wait_for_timeout(900)  # autosave
    _, got = await api(site, "GET", "/api/v1/config")
    cfg = got["draft"]["config"]
    tiles = [w for p in cfg["pages"] for w in p["widgets"] if w["type"] == "tile"]
    assert [t["entity"] for t in tiles] == ["light.kitchen"] and "tap" not in tiles[0]
    assert cfg["activities"][1]["keys"]["channel_up"] == {
        "ha": {"service": "light.toggle", "target": {"entity_id": "light.kitchen"}}
    }
    await page.locator("hh-remote svg.scr text", has_text="Kitchen").first.wait_for()  # preview

    bar = page.get_by_role("region", name="Draft")
    await bar.get_by_text("Valid").wait_for()
    await bar.get_by_role("button", name="Review & apply").click()
    await page.get_by_role("button", name="Apply to hub").click()
    await page.get_by_text("Saved as rev 1").wait_for()

    auth = {"Authorization": f"Bearer {TOKEN}"}
    status, bundle = await api(site, "GET", "/api/v1/bundle?hub=default", headers=auth)
    assert status == 200 and "editor" not in bundle["config"]
    assert bundle["config"]["activities"][1]["keys"]["channel_up"]["ha"]["service"] == "light.toggle"
    assert [w for p in bundle["config"]["pages"] for w in p["widgets"] if w["type"] == "tile"]
    if os.environ.get("HH_WRITE_FIXTURE"):
        FLOW_FIXTURE.parent.mkdir(exist_ok=True)
        FLOW_FIXTURE.write_text(json.dumps(got["draft"]["config"], indent=1, sort_keys=True) + "\n")
    assert [p for p in problems if "Failed to load resource" not in p] == []
