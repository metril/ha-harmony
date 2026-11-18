"""Headless-browser end-to-end: the real aiohttp app + the real built bundle, under an ingress prefix.

Skipped unless playwright is importable, a Chromium can be launched (HH_CHROMIUM=/path/to/chrome
overrides the lookup) and addon/frontend/dist exists (`npm run build`). CI does not install
playwright, so this runs on developer machines:
    HH_CHROMIUM=/usr/bin/google-chrome uv run --with playwright python -m pytest tests/py/test_addon_e2e.py -n0
Set HH_SHOTS=<dir> to also write light/dark screenshots at 1280 and 400 px.
"""

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

    async def to_real_server(route):  # the ingress proxy: strip the prefix
        await route.continue_(url=route.request.url.replace(PREFIX, "", 1))

    await page.route(f"{origin}{PREFIX}/**", to_real_server)
    await page.goto(f"{origin}{PREFIX}/")
    return page


async def api(origin, method, path, **kw):
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
        assert (await api(origin, "PUT", "/api/v1/config/draft", json={"config": cfg}, headers=H))[0] == 200
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
    await page.evaluate("document.querySelector('hh-app').shadowRoot.querySelector('hh-apply-dialog').waitMs = 500")
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
    await page.get_by_text("Send IR").first.wait_for()
    await shots(page, "devices")
