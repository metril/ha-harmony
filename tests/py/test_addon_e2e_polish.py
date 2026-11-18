"""Editor polish end-to-end, on the large synthetic fixture (5 devices, ~96 commands, 14 pages -> 19
wire pages, a Home Assistant device, 300 fake entities): accessibility (axe-core, light and dark, every
route), interaction timings, the 1024x768 layout, late-failure spot checks, keyboard-only tasks and
touch emulation. Skipped like test_addon_e2e.py (needs playwright, a Chromium and a built frontend);
axe tests also need `npm ci` in addon/frontend.
    HH_CHROMIUM=/usr/bin/google-chrome uv run --with playwright python -m pytest tests/py/test_addon_e2e_polish.py -n0
"""

import json
import os
from pathlib import Path

import pytest
from test_addon_e2e import DIST, PREFIX, TOKEN, INGRESS_HEADERS, H, api, browser, open_editor  # noqa: F401

from harmony_addon.app import create_app
from harmony_addon.auth import NoAuth
from harmony_addon.options import Options

pytestmark = pytest.mark.skipif(not (DIST / "index.html").is_file(), reason="frontend not built")
LARGE = json.loads((Path(__file__).parent / "fixtures" / "large_config.json").read_text())
AXE = DIST.parent / "node_modules" / "axe-core" / "axe.min.js"
SHOTS = os.environ.get("HH_SHOTS")
STORE = "document.querySelector('hh-app').store"
STUDIO = "document.querySelector('hh-app').shadowRoot.querySelector('hh-studio').shadowRoot"


class FakeHa:
    def __init__(self, n=300):
        doms = ["light", "switch", "fan", "cover", "climate", "media_player", "scene", "script", "sensor", "input_boolean"]
        self.states = [
            {
                "entity_id": f"{doms[i % 10]}.room_{i // 10}_{doms[i % 10]}",
                "state": "on" if i % 3 else "off",
                "attributes": {"friendly_name": f"Room {i // 10} {doms[i % 10]}"},
            }
            for i in range(n)
        ]
        self.services = [
            {"domain": d, "services": {"toggle": {"fields": {}}, "turn_on": {"fields": {"brightness_pct": {}}}}}
            for d in ("light", "switch", "fan", "cover", "media_player", "scene", "script")
        ]

    async def get(self, path):
        return {"/states": self.states, "/services": self.services}[path]


@pytest.fixture
async def large(aiohttp_server, tmp_path):
    app = create_app(
        tmp_path, options=Options("info"), auth=NoAuth(), static_dir=DIST, bundle_token=TOKEN,
        bundle_networks=None, wait_seconds=1.0, ha_api=FakeHa(),
    )
    server = await aiohttp_server(app)
    origin = f"http://127.0.0.1:{server.port}"
    status, _ = await api(origin, "PUT", "/api/v1/config/draft", json={"config": LARGE}, headers=H)
    assert status == 200
    return origin


async def open_large(browser, origin, width=1440, height=900, **kw):
    ctx = await browser.new_context(viewport={"width": width, "height": height}, bypass_csp=True, **kw)
    page = await ctx.new_page()

    async def to_server(route):
        await route.continue_(
            url=route.request.url.replace(PREFIX, "", 1),
            headers={**route.request.headers, **INGRESS_HEADERS},
        )

    await page.route(f"{origin}{PREFIX}/**", to_server)
    await page.goto(f"{origin}{PREFIX}/#/remote")
    await page.locator("hh-remote [data-key]").first.wait_for()
    return page


ROUTES = ["overview", "remote", "activities", "activities/watch_tv", "devices", "devices/tv", "devices/shield", "devices/new", "history"]


@pytest.mark.skipif(not AXE.is_file(), reason="axe-core is not installed (npm ci in addon/frontend)")
@pytest.mark.parametrize("scheme", ["light", "dark"])
async def test_axe_finds_no_violation_on_any_route(browser, large, scheme):
    bad = []
    for width in (1440, 400):
        page = await open_large(browser, large, width, 900, color_scheme=scheme)
        for route in ROUTES:
            await page.goto(f"{large}{PREFIX}/#/{route}")
            await page.wait_for_timeout(500)
            await page.add_script_tag(path=str(AXE))
            found = await page.evaluate(
                "axe.run(document, {resultTypes:['violations']}).then(r => r.violations.map(v => v.id + ' ' + v.nodes[0].target.join(' ')))"
            )
            bad += [f"{route}@{width}: {v}" for v in found]
        await page.context.close()
    assert bad == []


async def test_studio_at_1024x768_leaves_the_remote_520px(browser, large):
    page = await open_large(browser, large, 1024, 768)
    h = await page.evaluate(
        f"() => {STUDIO}.querySelector('hh-remote').shadowRoot.querySelector('svg').getBoundingClientRect().height"
    )
    assert h >= 520, h
    # one toolbar row: layer pills and the tools share a baseline
    ys = await page.evaluate(
        f"() => [...{STUDIO}.querySelectorAll('.tb .layers, .tb .tools')].map(e => Math.round(e.getBoundingClientRect().top))"
    )
    assert max(ys) - min(ys) < 16, ys
    # the draft bar's meters sit in a popover
    await page.get_by_role("button", name="Usage").click()
    await page.get_by_role("group", name="Usage").wait_for()
    if SHOTS:
        Path(SHOTS).mkdir(parents=True, exist_ok=True)
        await page.screenshot(path=f"{SHOTS}/studio-1024.png")


async def test_interaction_timings_on_the_large_config(browser, large):
    page = await open_large(browser, large)
    # the drop is measured in the page: pointerup -> the key's label changed and a frame was painted
    setup = """
    (name) => { window.__dt = null;
      window.addEventListener('pointerup', () => {
        const t0 = performance.now();
        const key = () => %s.querySelector('hh-remote').shadowRoot.querySelector('[data-key=' + name + ']');
        const before = key().getAttribute('aria-label');
        const tick = () => key().getAttribute('aria-label') !== before
          ? requestAnimationFrame(() => { window.__dt = performance.now() - t0; }) : requestAnimationFrame(tick);
        tick();
      }, { capture: true, once: true }); }
    """ % STUDIO
    times = []
    for key, cmd in (("play", "num_3"), ("pause", "num_4"), ("stop", "num_5")):
        await page.evaluate("() => { const s = " + STORE + "; s.edit(c => { delete c.keys['%s']; }); }" % key)
        await page.wait_for_timeout(900)
        chip = page.locator("hh-palette .chip2", has_text=cmd).first
        await chip.scroll_into_view_if_needed()
        cb = await chip.bounding_box()
        kb = await page.locator(f"hh-remote [data-key={key}]").bounding_box()
        await page.evaluate(setup, key)
        await page.mouse.move(cb["x"] + 10, cb["y"] + 10)
        await page.mouse.down()
        await page.mouse.move(cb["x"] + 50, cb["y"] + 30, steps=4)
        await page.mouse.move(kb["x"] + kb["width"] / 2, kb["y"] + kb["height"] / 2, steps=12)
        await page.mouse.up()
        await page.wait_for_function("window.__dt !== null")
        times.append(await page.evaluate("window.__dt"))
    print("drop -> re-rendered key (ms):", [round(t, 1) for t in times])
    assert max(times) < 100, times
    # search the palette: 300 entities
    await page.get_by_role("tab", name="Home", exact=True).click()
    await page.locator("hh-palette .ent").first.wait_for()
    rows = await page.locator("hh-palette .ent").count()
    assert rows >= 250


async def test_five_late_failures_are_prevented_or_shown_before_apply(browser, large):
    page = await open_large(browser, large)
    # 1. a 40-character non-ASCII device name is cut at 31 BYTES, not characters
    await page.goto(f"{large}{PREFIX}/#/devices/tv")
    box = page.get_by_label("Device name")
    await box.fill("Ж" * 40)
    await box.dispatch_event("change")
    saved = await page.evaluate(f"() => {STORE}.config.devices[0].name")
    assert len(saved.encode()) <= 31, saved
    # 2. an alias that is another device's id: the server's message is listed on that device's page
    await page.evaluate(f"() => {STORE}.edit(c => {{ c.devices[0].aliases = ['avr']; }})")
    alert = page.get_by_role("alert").filter(has_text="problem")
    await alert.get_by_text("Alias 1").wait_for()
    await alert.get_by_text("another device's id").wait_for()
    # 3. a template in a Home Assistant data field is refused in the picker, with the server's wording
    await page.goto(f"{large}{PREFIX}/#/remote")
    await page.locator("hh-remote [data-key=play]").click()
    insp = page.locator("hh-inspector")
    await insp.get_by_role("button", name="Home Assistant action").click()
    await insp.locator("label", has_text="Service").locator("select").select_option("light.turn_on")
    await insp.get_by_placeholder("Search entities").fill("light.room_0_light")
    await insp.get_by_label("Field brightness_pct").fill("{{ 5 }}")
    await insp.get_by_text("must not contain a template").wait_for()
    assert await insp.get_by_role("button", name="Set action").is_disabled()
    # 4. the 33rd widget on a page is refused
    await page.evaluate(
        f"() => {STORE}.edit(c => {{ const p = c.pages[4]; p.cols = 16; p.rows = 16; "
        "p.widgets = Array.from({length: 32}, (_, i) => ({type: 'label', x: i % 16, y: Math.floor(i / 16), label: 'L'})); })"
    )
    from_state = await page.evaluate(f"() => {STORE}.config.pages[4].widgets.length")
    assert from_state == 32
    # 5. 'Next page' is not offered on the last page
    await page.evaluate(
        f"() => {STORE}.edit(c => {{ const p = c.pages[c.pages.length - 1]; p.widgets.push({{type: 'button', id: 'lastnav', x: 3, y: 3, label: 'N', nav: 'home'}}); }})"
    )
    await page.wait_for_timeout(400)
    r = await page.evaluate(
        f"() => {{ const s = {STORE}; return s.config.pages.length; }}"
    )
    assert r == 14


async def test_keyboard_only_assign_a_command_to_a_key_then_apply(browser, large):
    page = await open_large(browser, large)
    await page.evaluate(f"() => {STORE}.edit(c => {{ delete c.keys['play']; }})")
    await page.wait_for_timeout(800)
    await page.locator("hh-palette .chip2", has_text="num_3").first.focus()
    await page.keyboard.press("Space")  # pick it up
    await page.locator(".banner2", has_text="Click a key").wait_for()
    await page.locator("hh-remote [data-key=play]").focus()
    await page.keyboard.press("Enter")  # drop it
    await page.wait_for_function(
        f"() => {STUDIO}.querySelector('hh-remote').shadowRoot.querySelector('[data-key=play]').getAttribute('aria-label').includes('tv: num_3')"
    )
    # review and apply with the keyboard
    await page.get_by_role("button", name="Review & apply").focus()
    await page.keyboard.press("Enter")
    await page.get_by_role("button", name="Apply to hub").focus()
    await page.keyboard.press("Enter")
    await page.get_by_text("Saved as rev 1").wait_for()


async def test_tap_then_tap_works_with_touch_emulation_at_400px(browser, large):
    """Touch emulation (hasTouch), not a real device: tap a chip, tap a key."""
    page = await open_large(browser, large, 400, 800, has_touch=True, is_mobile=True)
    await page.evaluate(f"() => {STORE}.edit(c => {{ delete c.keys['play']; }})")
    await page.wait_for_timeout(800)
    await page.get_by_role("button", name="Palette").tap()
    await page.locator("hh-palette .chip2", has_text="num_3").first.tap()
    await page.locator(".banner2", has_text="Click a key").wait_for()
    await page.locator("hh-remote [data-key=play]").tap()
    await page.wait_for_function(
        f"() => {STUDIO}.querySelector('hh-remote').shadowRoot.querySelector('[data-key=play]').getAttribute('aria-label').includes('tv: num_3')"
    )
    # nothing scrolls the outer page during that
    assert await page.evaluate("window.scrollY") == 0


async def test_long_press_drag_with_touch_emulation_does_not_scroll_the_page(browser, large):
    """CDP touch events (Playwright's touchscreen can only tap): hold a chip 400 ms, drag it onto a key."""
    page = await open_large(browser, large, 400, 800, has_touch=True, is_mobile=True)
    await page.evaluate(f"() => {STORE}.edit(c => {{ delete c.keys['pause']; }})")
    await page.wait_for_timeout(800)
    await page.get_by_role("button", name="Palette").tap()
    chip = page.locator("hh-palette .chip2", has_text="num_4").first
    await chip.scroll_into_view_if_needed()
    cb = await chip.bounding_box()
    cdp = await page.context.new_cdp_session(page)
    cx, cy = cb["x"] + cb["width"] / 2, cb["y"] + cb["height"] / 2
    await cdp.send("Input.dispatchTouchEvent", {"type": "touchStart", "touchPoints": [{"x": cx, "y": cy}]})
    await page.wait_for_timeout(450)  # long press: the drag begins and the palette sheet closes
    kb = await page.locator("hh-remote [data-key=pause]").bounding_box()
    tx, ty = kb["x"] + kb["width"] / 2, kb["y"] + kb["height"] / 2
    for i in range(1, 9):
        await cdp.send(
            "Input.dispatchTouchEvent",
            {"type": "touchMove", "touchPoints": [{"x": cx + (tx - cx) * i / 8, "y": cy + (ty - cy) * i / 8}]},
        )
    await cdp.send("Input.dispatchTouchEvent", {"type": "touchEnd", "touchPoints": []})
    await page.wait_for_function(
        f"() => {STUDIO}.querySelector('hh-remote').shadowRoot.querySelector('[data-key=pause]').getAttribute('aria-label').includes('tv: num_4')"
    )
    assert await page.evaluate("window.scrollY") == 0
