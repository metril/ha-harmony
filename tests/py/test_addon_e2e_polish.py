# ruff: noqa: F811
"""Editor polish end-to-end, on the large synthetic fixture (5 devices, ~96 commands, 14 pages -> 19
wire pages, a Home Assistant device, 300 fake entities): accessibility (axe-core, light and dark, every
route), interaction timings, the 1024x768 layout, late-failure spot checks, keyboard-only tasks and
touch emulation. Skipped like test_addon_e2e.py (needs playwright, a Chromium and a built frontend);
axe tests also need `npm ci` in addon/frontend.
    HH_CHROMIUM=/usr/bin/google-chrome uv run --with playwright python -m pytest tests/py/test_addon_e2e_polish.py -n0
"""

import json
import os
import re
from pathlib import Path

import pytest
from test_addon_e2e import (  # noqa: F401
    DIST,
    INGRESS_HEADERS,
    PREFIX,
    TOKEN,
    H,
    api,
    browser,
    open_editor,
    proxy_ws,
)

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
        doms = [
            "light",
            "switch",
            "fan",
            "cover",
            "climate",
            "media_player",
            "scene",
            "script",
            "sensor",
            "input_boolean",
        ]
        self.states = [
            {
                "entity_id": f"{doms[i % 10]}.room_{i // 10}_{doms[i % 10]}",
                "state": "on" if i % 3 else "off",
                "attributes": {"friendly_name": f"Room {i // 10} {doms[i % 10]}"},
            }
            for i in range(n)
        ]
        self.services = [
            {
                "domain": d,
                "services": {
                    "toggle": {"fields": {}},
                    "turn_on": {"fields": {"brightness_pct": {}}},
                },
            }
            for d in ("light", "switch", "fan", "cover", "media_player", "scene", "script")
        ]

    async def get(self, path):
        return {"/states": self.states, "/services": self.services}[path]


@pytest.fixture
async def large(aiohttp_server, tmp_path):
    app = create_app(
        tmp_path,
        options=Options("info"),
        auth=NoAuth(),
        static_dir=DIST,
        bundle_token=TOKEN,
        bundle_networks=None,
        ha_api=FakeHa(),
    )
    server = await aiohttp_server(app)
    origin = f"http://127.0.0.1:{server.port}"
    status, _ = await api(origin, "PUT", "/api/v1/config/draft", json={"config": LARGE}, headers=H)
    assert status == 200
    return origin


async def open_large(browser, origin, width=1440, height=900, **kw):
    ctx = await browser.new_context(
        viewport={"width": width, "height": height}, bypass_csp=True, **kw
    )
    page = await ctx.new_page()

    async def to_server(route):
        await route.continue_(
            url=route.request.url.replace(PREFIX, "", 1),
            headers={**route.request.headers, **INGRESS_HEADERS},
        )

    await page.route(f"{origin}{PREFIX}/**", to_server)
    await proxy_ws(page, origin)
    await page.goto(f"{origin}{PREFIX}/#/remote")
    await page.locator("hh-remote [data-key]").first.wait_for()
    await _settle(page)
    return page


ROUTES = [
    "overview",
    "remote",
    "activities",
    "activities/watch_tv",
    "themes",
    "devices",
    "devices/tv",
    "devices/shield",
    "devices/new",
    "history",
]


@pytest.mark.skipif(
    not AXE.is_file(), reason="axe-core is not installed (npm ci in addon/frontend)"
)
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
    assert bad == [], "\n".join(bad)


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
    # the drop is measured as main-thread CPU time from pointerup until the key's label changed and a frame was painted
    setup = """
    (name) => { window.__dt = null;
      window.addEventListener('pointerup', () => {
        const t0 = performance.now();
        const key = () => STUDIO_ROOT.querySelector('hh-remote').shadowRoot.querySelector('[data-key=' + name + ']');
        const before = key().getAttribute('aria-label');
        const tick = () => key().getAttribute('aria-label') !== before
          ? requestAnimationFrame(() => { window.__dt = performance.now() - t0; }) : requestAnimationFrame(tick);
        tick();
      }, { capture: true, once: true }); }
    """.replace("STUDIO_ROOT", STUDIO)
    cdp = await page.context.new_cdp_session(page)
    await cdp.send("Performance.enable", {"timeDomain": "threadTicks"})

    async def main_cpu_ms():  # main-thread CPU time: unaffected by the machine being busy
        m = (await cdp.send("Performance.getMetrics"))["metrics"]
        return next(x["value"] for x in m if x["name"] == "ThreadTime") * 1000

    times = []
    for key, cmd in (("play", "num_3"), ("pause", "num_4"), ("stop", "num_5")):
        # best of three drops per key (CPU time, so a busy machine does not inflate it)
        tries = []
        for _ in range(3):
            await page.evaluate(
                "(k) => { " + STORE + ".edit(c => { delete c.keys[k]; }); }",
                key,
            )
            await page.wait_for_timeout(900)
            chip = page.locator("hh-palette .chip2", has_text=re.compile(rf"^\s*{cmd[-1]}\s*$")).first
            await chip.scroll_into_view_if_needed()
            cb = await chip.bounding_box()
            kb = await page.locator(f"hh-remote [data-key={key}]").bounding_box()
            await page.evaluate(setup, key)
            await page.mouse.move(cb["x"] + 10, cb["y"] + 10)
            await page.mouse.down()
            await page.mouse.move(cb["x"] + 50, cb["y"] + 30, steps=4)
            await page.mouse.move(kb["x"] + kb["width"] / 2, kb["y"] + kb["height"] / 2, steps=12)
            cpu0 = await main_cpu_ms()
            await page.mouse.up()
            await page.wait_for_function("window.__dt !== null")
            tries.append(await main_cpu_ms() - cpu0)
        times.append(min(tries))
    print("drop -> re-rendered key (CPU ms):", [round(t, 1) for t in times])
    assert max(times) < 100, times
    # the Home Assistant tab: 300 entities, only the open groups draw rows
    await page.get_by_role("tab", name="Home Assistant", exact=True).click()
    summary = page.locator("hh-palette [role=status][aria-live]")
    await summary.get_by_text("300 entities").wait_for()
    assert await page.locator("hh-palette .ent").count() == 0  # collapsed by default


async def test_five_late_failures_are_prevented_or_shown_before_apply(browser, large):
    page = await open_large(browser, large)
    # 1. a 40-character non-ASCII device name is cut at 31 BYTES, not characters
    await page.goto(f"{large}{PREFIX}/#/devices/tv")
    box = page.get_by_label("Device Name")
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
    assert await insp.get_by_role("button", name="Set Action").is_disabled()
    # 4. the 33rd widget on a page is refused in the UI (N on the screen = "add a widget")
    await page.goto(f"{large}{PREFIX}/#/remote")
    pi = await page.evaluate(
        f"() => {{ const s = {STORE}; const i = s.config.pages.findIndex(p => !p.list); "
        "s.edit(c => { const p = c.pages[i]; p.cols = 16; p.rows = 16; "
        "p.widgets = Array.from({length: 32}, (_, k) => ({type: 'label', id: 'f' + k, x: k % 16, y: Math.floor(k / 16), label: 'L'})); }); return i; }"
    )
    await page.evaluate(
        f"() => document.querySelector('hh-app').shadowRoot.querySelector('hh-studio').state0.set({{mode: 'screen', page: {pi}}})"
    )
    screen = page.locator("hh-remote [role=application]")
    await screen.dispatch_event("keydown", {"key": "n"})
    await page.get_by_text("A page holds at most 32 widgets.").first.wait_for()
    assert await page.evaluate(f"() => {STORE}.config.pages[{pi}].widgets.length") == 32
    # 5. "Next page" is disabled in the picker of a button on the last page
    last = await page.evaluate(
        f"() => {{ const s = {STORE}; const i = s.config.pages.length - 1; "
        "s.edit(c => { c.pages[i].widgets.push({type: 'button', id: 'lastnav', x: 0, y: 0, label: 'N', action: {none: true}}); }); "
        "return [i, s.config.pages[i].widgets.length - 1]; }"
    )
    await page.evaluate(
        f"() => document.querySelector('hh-app').shadowRoot.querySelector('hh-studio').state0.set("
        f"{{mode: 'screen', page: {last[0]}, sel: {{kind: 'widget', page: {last[0]}, index: {last[1]}}}}})"
    )
    nxt = page.locator("hh-inspector option[value=next]")
    await nxt.wait_for(state="attached")
    assert await nxt.is_disabled()
    assert "last page" in (await nxt.text_content())


async def test_keyboard_only_assign_a_command_to_a_key_then_apply(browser, large):
    page = await open_large(browser, large)
    await page.evaluate(f"() => {STORE}.edit(c => {{ delete c.keys['play']; }})")
    await page.wait_for_timeout(800)
    await page.locator("hh-palette .chip2", has_text=re.compile(r"^\s*3\s*$")).first.focus()
    await page.keyboard.press("Space")  # pick it up
    await page.locator(".banner2", has_text="Click a key").wait_for()
    await page.locator("hh-remote [data-key=play]").focus()
    await page.keyboard.press("Enter")  # drop it
    await page.wait_for_function(
        f"() => {STUDIO}.querySelector('hh-remote').shadowRoot.querySelector('[data-key=play]').getAttribute('aria-label').includes('tv: 3')"
    )
    # review and apply with the keyboard
    await page.get_by_role("button", name="Review & Apply").focus()
    await page.keyboard.press("Enter")
    await page.get_by_role("button", name="Apply to Hub").focus()
    await page.keyboard.press("Enter")
    await page.get_by_text("Saved as rev 1").wait_for()


async def test_tap_then_tap_works_with_touch_emulation_at_400px(browser, large):
    """Touch emulation (hasTouch), not a real device: tap a chip, tap a key."""
    page = await open_large(browser, large, 400, 800, has_touch=True, is_mobile=True)
    await page.evaluate(f"() => {STORE}.edit(c => {{ delete c.keys['play']; }})")
    await page.wait_for_timeout(800)
    await page.get_by_role("button", name="Palette").tap()
    await page.locator("hh-palette .chip2", has_text=re.compile(r"^\s*3\s*$")).first.tap()
    await page.locator(".banner2", has_text="Click a key").wait_for()
    await page.locator("hh-remote [data-key=play]").tap()
    await page.wait_for_function(
        f"() => {STUDIO}.querySelector('hh-remote').shadowRoot.querySelector('[data-key=play]').getAttribute('aria-label').includes('tv: 3')"
    )
    # nothing scrolls the outer page during that
    assert await page.evaluate("window.scrollY") == 0


async def test_long_press_drag_with_touch_emulation_does_not_scroll_the_page(browser, large):
    """CDP touch events (Playwright's touchscreen can only tap): hold a chip 400 ms, drag it onto a key."""
    page = await open_large(browser, large, 400, 800, has_touch=True, is_mobile=True)
    await page.evaluate(f"() => {STORE}.edit(c => {{ delete c.keys['pause']; }})")
    await page.wait_for_timeout(800)
    await page.get_by_role("button", name="Palette").tap()
    chip = page.locator("hh-palette .chip2", has_text=re.compile(r"^\s*4\s*$")).first
    await chip.scroll_into_view_if_needed()
    cb = await chip.bounding_box()
    cdp = await page.context.new_cdp_session(page)
    cx, cy = cb["x"] + cb["width"] / 2, cb["y"] + cb["height"] / 2
    await cdp.send(
        "Input.dispatchTouchEvent", {"type": "touchStart", "touchPoints": [{"x": cx, "y": cy}]}
    )
    await page.wait_for_timeout(450)  # long press: the drag begins and the palette sheet closes
    kb = await page.locator("hh-remote [data-key=pause]").bounding_box()
    tx, ty = kb["x"] + kb["width"] / 2, kb["y"] + kb["height"] / 2
    for i in range(1, 9):
        await cdp.send(
            "Input.dispatchTouchEvent",
            {
                "type": "touchMove",
                "touchPoints": [{"x": cx + (tx - cx) * i / 8, "y": cy + (ty - cy) * i / 8}],
            },
        )
    await cdp.send("Input.dispatchTouchEvent", {"type": "touchEnd", "touchPoints": []})
    await page.wait_for_function(
        f"() => {STUDIO}.querySelector('hh-remote').shadowRoot.querySelector('[data-key=pause]').getAttribute('aria-label').includes('tv: 4')"
    )
    assert await page.evaluate("window.scrollY") == 0


def _studio(js):
    return f"() => document.querySelector('hh-app').shadowRoot.querySelector('hh-studio').state0.{js}"


async def test_ha_actions_have_no_test_fire_and_read_as_name_dot_command(browser, large):
    page = await open_large(browser, large)
    # a Home Assistant action on a key (friendly name from the entity, repeat marker) and a long device name
    await page.evaluate(
        f"() => {STORE}.edit(c => {{ c.activities[1].keys.left = {{ha: {{service: 'light.toggle', target: {{entity_id: 'light.room_0_light'}}, repeat: true}}}}; "
        "c.devices[4].name = 'Living room ceiling shield'; })"
    )
    await page.evaluate(_studio("setLayer('stream')"))
    await page.get_by_role("button", name="X-ray").click()
    remote = page.locator("hh-remote")
    await page.wait_for_timeout(2000)
    texts = await remote.locator("text.xl").evaluate_all(
        "els => els.map(e => [...e.childNodes].filter(n => n.nodeType === 3).map(n => n.textContent).join(''))"
    )
    assert any(t.startswith("Room 0 light") and t.endswith("toggle ↻") for t in texts), [t for t in texts if "toggle" in t or "light" in t]
    long = [t for t in texts if t.startswith("Living room ceiling")]
    assert long and long[0].endswith("…") and len(long[0]) <= 26, texts
    titles = await remote.locator("text.xl title").all_text_contents()
    assert any(t == "Living room ceiling shield · Play/Pause" for t in titles), titles
    if SHOTS:
        Path(SHOTS).mkdir(parents=True, exist_ok=True)
        await page.screenshot(path=f"{SHOTS}/xray-ha.png")
    # the key popover / inspector: an HA badge, never a Send IR button
    await remote.locator("[data-key=play]").click()
    insp = page.locator("hh-inspector")
    await insp.locator("hh-send-ir .ha").first.wait_for()
    assert await insp.locator("hh-send-ir button").count() == 0
    # the palette: HA device chips carry the badge
    await page.get_by_role("tab", name="Devices").click()
    assert await page.locator("hh-palette .chip2 .hab").count() >= 10
    # an IR command still has its Send IR button
    await page.get_by_role("button", name="Close").first.click()
    await remote.locator("[data-key=volume_up]").click()
    await insp.locator("hh-send-ir button").first.wait_for()


async def test_permissions_panel_groups_20_entities_and_scrolls_inside_its_card(browser, large):
    page = await open_large(browser, large)
    await page.evaluate(
        f"() => {STORE}.edit(c => {{ c.pages[0].widgets = Array.from({{length: 20}}, (_, i) => ({{type: 'tile', id: 't' + i, x: i % 4, y: Math.floor(i / 4), "
        "entity: (i % 2 ? 'light.perm_' : 'switch.perm_') + i, tap: {ha: {service: (i % 2 ? 'light.toggle' : 'switch.turn_on'), target: {entity_id: (i % 2 ? 'light.perm_' : 'switch.perm_') + i}}}})); })"
    )
    await page.goto(f"{large}{PREFIX}/#/overview")
    box = page.locator("hh-overview .scrollbox[aria-label='Entities by domain']")
    await box.wait_for()
    heads = await box.locator("h3").all_text_contents()
    assert "Light (19)" in heads and "Switch (10)" in heads, heads
    assert [h.split(" ")[0] for h in heads] == sorted(h.split(" ")[0] for h in heads), heads
    m = await box.evaluate("e => ({sh: e.scrollHeight, ch: e.clientHeight})")
    assert m["sh"] > m["ch"] and m["ch"] <= 300, m
    assert await box.locator(".svc", has_text="toggle").count() >= 10
    card = await page.locator("hh-overview section[aria-labelledby=hp]").bounding_box()
    assert card["height"] < 700, card
    if SHOTS:
        await page.locator("hh-overview section[aria-labelledby=hp]").screenshot(path=f"{SHOTS}/permissions.png")


async def test_layer_pills_fit_at_1024_and_overflow_into_a_menu_by_measured_width(browser, large):
    page = await open_large(browser, large, 1024, 768)
    await page.evaluate("() => document.fonts.ready")
    # measured geometry: [no pill squeezed or ellipsised and no scroll, pills shown, layers in the "More" menu]
    state = (
        "() => { const r = " + STUDIO + ".querySelector('.layers'); const bs = [...r.querySelectorAll('button')];"
        " const m = " + STUDIO + ".querySelector('select[aria-label=\"More layers\"]');"
        " return [r.scrollWidth <= r.clientWidth + 1 && bs.every(b => b.scrollWidth <= b.clientWidth + 1), bs.length, m ? m.options.length - 1 : 0]; }"
    )

    async def settled(total):
        # the pill count converges after a few renders; wait for it, then assert on the final geometry
        await page.wait_for_function(
            "([s, n]) => { const [ok, p, m] = eval('(' + s + ')')(); return ok && p + m === n; }", arg=[state, total]
        )
        ok, pills, menu = await page.evaluate(state)
        assert ok and pills + menu == total and pills >= 1, (ok, pills, menu)
        return pills, menu

    await settled(4)  # Default + 3 activities
    await page.evaluate(
        f"() => {STORE}.edit(c => {{ c.activities.push({{id: 'a4', name: 'Music', devices: [{{device: 'avr'}}]}}); }})"
    )
    await settled(5)
    await page.evaluate(
        f"() => {STORE}.edit(c => {{ for (const n of ['a5', 'a6', 'a7']) c.activities.push({{id: n, name: 'Extra ' + n, devices: [{{device: 'avr'}}]}}); }})"
    )
    _, menu = await settled(8)
    assert menu >= 1  # eight layers never fit in one row at 1024
    more = page.locator("hh-studio").locator("select[aria-label='More layers']")
    await more.select_option("a7")
    await page.locator("hh-studio .layers button[aria-selected=true]", has_text="Extra a7").wait_for()
    await settled(8)
    # a wide window shows five pills with no menu, and four pills plus the menu past five
    await page.set_viewport_size({"width": 1600, "height": 900})
    await page.evaluate(f"() => {STORE}.edit(c => {{ c.activities.length = 3; }})")
    await page.wait_for_function("([s]) => eval('(' + s + ')')()[1] === 4", arg=[state])
    await page.evaluate(
        f"() => {STORE}.edit(c => {{ for (const n of ['a4', 'a5', 'a6']) c.activities.push({{id: n, name: 'More ' + n, devices: [{{device: 'avr'}}]}}); }})"
    )
    assert await settled(7) == (4, 3)


# ---------------------------------------------------------------- studio: palette list, selection, key contrast
REMOTE = f"{STUDIO}.querySelector('hh-remote').shadowRoot"
PALETTE = f"{STUDIO}.querySelector('hh-palette').shadowRoot"
STUDIO_EL = "document.querySelector('hh-app').shadowRoot.querySelector('hh-studio')"


@pytest.mark.parametrize("size", [(1440, 900), (1280, 720), (420, 800)])
async def test_palette_chips_form_an_aligned_grid(browser, large, size):
    page = await open_large(browser, large, *size)
    for tab in ("Devices", "Activities", "Home Assistant", "Widgets"):
        await page.evaluate(
            f"() => [...{PALETTE}.querySelectorAll('[role=tab]')].find(b => b.getAttribute('aria-label') == '{tab}').click()"
        )
        await page.wait_for_timeout(250)
        bad = await page.evaluate(
            f"""() => {{
          const body = {PALETTE}.querySelector('.body'), bb = body.getBoundingClientRect(), out = [];
          if (body.scrollWidth > body.clientWidth) out.push('horizontal overflow');
          for (const grp of {PALETTE}.querySelectorAll('.cells')) {{
            const cs = [...grp.children].flatMap(c => c.matches('.pair[data-span=stack]') ? [...c.children] : [c]).map(c => c.getBoundingClientRect());
            if (!cs.length) continue;
            const lefts = new Set(cs.map(r => Math.round(r.left))), hs = new Set(cs.map(r => Math.round(r.height)));
            if (lefts.size > 3) out.push('ragged left edges ' + [...lefts]);
            if (hs.size > 1) out.push('unequal heights ' + [...hs]);
            if (cs.some(r => r.right > bb.right + 0.5 || r.left < bb.left - 0.5)) out.push('chip outside the list');
          }}
          return out;
        }}"""
        )
        assert bad == [], (tab, size, bad)
    await page.context.close()


async def _settle(page):
    """Two frames, and every running CSS transition finished: layout and colours are final."""
    await page.evaluate(
        """async () => {
      for (let i = 0; i < 2; i++) {
        await new Promise((r) => requestAnimationFrame(r));
        await Promise.all(document.getAnimations().map((a) => a.finished.catch(() => {})));
      }
    }"""
    )


async def _sel(page):
    return await page.evaluate(f"() => JSON.stringify({STUDIO_EL}.st.sel)")


async def _key_centre(page, name="select"):
    return await page.evaluate(
        f"() => {{ const r = {REMOTE}.querySelector('[data-key={name}] .cap').getBoundingClientRect(); return [r.x + r.width / 2, r.y + r.height / 2]; }}"
    )


@pytest.mark.parametrize("how", ["reclick", "body", "canvas", "escape", "layer"])
async def test_selection_can_be_cleared(browser, large, how):
    page = await open_large(browser, large)
    x, y = await _key_centre(page)
    pressed = f"() => {REMOTE}.querySelector('[data-key=select]').getAttribute('aria-pressed')"
    selected = f"() => JSON.stringify({STUDIO_EL}.st.sel).includes('\"key\"') && ({pressed})() === 'true'"
    # the layout can still shift once (late chunks, scrollbar) on a loaded CPU: re-measure and click again
    for attempt in range(3):
        await page.mouse.click(x, y)
        try:
            # selection state and the re-rendered aria-pressed land asynchronously (Lit update): wait, never sleep
            await page.wait_for_function(selected, timeout=10000 if attempt == 2 else 3000)
            break
        except Exception:
            if attempt == 2:
                raise
            x, y = await _key_centre(page)
    if how in ("reclick", "body"):  # the popover may sit over the key: dismiss it first, the key stays selected
        await page.keyboard.press("Escape")
        await page.wait_for_function(f"() => !{STUDIO}.querySelector('.pop')", timeout=10000)
        assert '"key"' in await _sel(page)
    if how == "reclick":
        await page.mouse.click(x, y)
    elif how == "body":  # the dark body below every key
        r = await page.evaluate(
            f"() => {{ const r = {REMOTE}.querySelector('.body').getBoundingClientRect(); return [r.x + r.width * 0.5, r.y + r.height * 0.985]; }}"
        )
        await page.mouse.click(*r)
    elif how == "canvas":
        r = await page.evaluate(
            f"() => {{ const r = {STUDIO}.querySelector('.canvas').getBoundingClientRect(); return [r.x + 12, r.y + r.height / 2]; }}"
        )
        await page.mouse.click(*r)
    elif how == "escape":
        await page.keyboard.press("Escape")  # the first Escape closes the popover
        await page.wait_for_function(f"() => !{STUDIO}.querySelector('.pop')", timeout=10000)
        await page.keyboard.press("Escape")
    else:
        await page.evaluate(f"() => {STUDIO_EL}.st.setLayer('watch_tv')")
    await page.wait_for_function(f"() => JSON.stringify({STUDIO_EL}.st.sel) === 'null'", timeout=10000)
    await page.wait_for_function(f"() => ({pressed})() === 'false'", timeout=10000)
    assert await page.evaluate(f"() => !{STUDIO}.querySelector('.pop')")
    await page.context.close()


async def test_widget_selection_clears_on_reclick(browser, large):
    page = await open_large(browser, large)
    await page.evaluate(f"() => {STUDIO_EL}.st.set({{mode: 'screen'}})")
    await page.locator("hh-remote [data-widget]").first.wait_for()
    await _settle(page)
    box = await page.evaluate(
        f"() => {{ const r = {REMOTE}.querySelector('[data-widget]').getBoundingClientRect(); return [r.x + r.width / 2, r.y + r.height / 2]; }}"
    )
    await page.mouse.click(*box)
    await page.wait_for_function(f"() => JSON.stringify({STUDIO_EL}.st.sel).includes('\"widget\"')", timeout=10000)
    await page.mouse.click(*box)
    await page.wait_for_function(f"() => JSON.stringify({STUDIO_EL}.st.sel) === 'null'", timeout=10000)
    await page.context.close()


def _lum(c):
    def f(v):
        v /= 255
        return v / 12.92 if v <= 0.03928 else ((v + 0.055) / 1.055) ** 2.4

    return 0.2126 * f(c[0]) + 0.7152 * f(c[1]) + 0.0722 * f(c[2])


def _ratio(a, b):
    la, lb = sorted((_lum(a), _lum(b)), reverse=True)
    return (la + 0.05) / (lb + 0.05)


PROBE = """
async (args) => {
  const img = await createImageBitmap(await (await fetch('data:image/png;base64,' + args.shot)).blob());
  const cv = new OffscreenCanvas(img.width, img.height), cx = cv.getContext('2d', { willReadFrequently: true });
  cx.drawImage(img, 0, 0);
  return args.boxes.map((b) => {
    const d = cx.getImageData(b.x, b.y, b.w, b.h).data, px = [];
    for (let i = 0; i < d.length; i += 4) px.push([d[i], d[i + 1], d[i + 2]]);
    return px;
  });
}
"""


async def _measure(page, scale=2):
    """Foreground colour and effective background (pixels with the legends hidden) of every visible label and glyph."""
    items = await page.evaluate(
        f"""() => {{
      const root = {REMOTE};
      return [...root.querySelectorAll('.key .lab, .key .gs, .key .gl, .pr, .prl')].filter((e) => getComputedStyle(e).opacity > 0.5 && getComputedStyle(e).display !== 'none').map((e) => {{
        const cs = getComputedStyle(e), r = e.getBoundingClientRect();
        const c = e.matches('.gs, .prl') ? cs.stroke : cs.fill;
        return {{ key: e.closest('.key')?.dataset.key ?? 'printed:' + e.textContent, text: e.matches('.lab, .pr'), col: c.match(/\\d+/g).slice(0, 3).map(Number), x: r.x, y: r.y, w: r.width, h: r.height }};
      }}).filter((i) => i.w > 0 && i.h > 0);
    }}"""
    )
    await page.evaluate(
        f"() => {{ const s = document.createElement('style'); s.id = 'hide'; s.textContent = '.lab,.pr,.prl,.gs,.gl{{visibility:hidden}}'; {REMOTE}.appendChild(s); }}"
    )
    import base64

    shot = base64.b64encode(await page.screenshot()).decode()
    await page.evaluate(f"() => {REMOTE}.getElementById('hide').remove()")
    boxes = [
        {"x": int(i["x"] * scale), "y": int(i["y"] * scale), "w": max(1, int(i["w"] * scale)), "h": max(1, int(i["h"] * scale))}
        for i in items
    ]
    px = await page.evaluate(PROBE, {"shot": shot, "boxes": boxes})
    out = []
    for i, p in zip(items, px, strict=True):
        p.sort(key=_lum)
        bg = p[int(len(p) * 0.75)]  # the light end of the backdrop; a hairline key outline does not count
        out.append((i["key"], i["text"], i["col"], bg, _ratio(i["col"], bg)))
    return out


async def test_every_key_label_and_glyph_is_readable_when_selected(browser, large):
    page = await open_large(browser, large, 1440, 900, device_scale_factor=2)
    names = await page.evaluate(f"() => [...{REMOTE}.querySelectorAll('[data-key]')].map((e) => e.dataset.key)")
    assert len(names) >= 35
    bad = []
    for state in ("idle", "hover", "selected", "selected+hover", "drop-target"):
        for name in names:
            sel = "null" if state in ("idle", "hover", "drop-target") else f"{{kind: 'key', name: '{name}'}}"
            await page.evaluate(f"() => {STUDIO_EL}.st.set({{sel: {sel}}})")
            if state == "drop-target":  # a picked-up source with the keyboard cursor on this key
                await page.evaluate(
                    f"() => {{ const a = {STUDIO_EL}.assigner; a.cancel(); a.pick({{kind: 'alloff'}}); a.setCursor({{kind: 'key', name: '{name}'}}); }}"
                )
            hover = state.endswith("hover")
            await page.mouse.move(*(await _key_centre(page, name))) if hover else await page.mouse.move(0, 0)
            await _settle(page)
            for key, is_text, col, bg, ratio in await _measure(page):
                if ratio < (4.5 if is_text else 3.0):
                    bad.append(f"{state} {name}: {key} {col} on {bg} = {ratio:.2f}")
    assert bad == [], "\n".join(bad)
    await page.context.close()


@pytest.mark.parametrize("size", [(1440, 900), (1280, 720)])
async def test_inspector_free_keys_form_an_aligned_grid(browser, large, size):
    page = await open_large(browser, large, *size)
    await page.evaluate(f"() => {STUDIO_EL}.st.set({{sel: null}})")
    await page.wait_for_timeout(250)
    bad = await page.evaluate(
        f"""() => {{
      const root = {STUDIO}.querySelector('hh-inspector').shadowRoot, out = [];
      const ir = root.host.getBoundingClientRect();
      for (const grp of root.querySelectorAll('.cells')) {{
        const cs = [...grp.children].flatMap(c => c.matches('.pair[data-span=stack]') ? [...c.children] : [c]).map(c => c.getBoundingClientRect());
        if (new Set(cs.map(r => Math.round(r.height))).size > 1) out.push('unequal heights');
        if (new Set(cs.map(r => Math.round(r.left))).size > 3) out.push('ragged left edges');
        if (cs.some(r => r.right > ir.right + 0.5)) out.push('overflow');
      }}
      return [out, root.querySelectorAll('.cells').length];
    }}"""
    )
    assert bad[1] >= 1 and bad[0] == [], bad
    await page.context.close()
