# ruff: noqa: F811
"""The palette's Home Assistant tab on a large install, end to end: 3,000 synthetic entities, 25 areas on
4 floors, 40 integrations, labels. Collapsed by default, search, filters, group-by, favourites, "used in
this config", dragging an entity onto a key, a bounded DOM, the unreadable and no-registry states.
Skipped like test_addon_e2e.py. Set HH_SHOTS=<dir> to write screenshots of every state.
    HH_CHROMIUM=/usr/bin/google-chrome uv run pytest tests/py/test_addon_e2e_hatab.py -n0
"""

import os
import time
from itertools import pairwise
from pathlib import Path

import playwright.async_api as pw
import pytest
from test_addon_e2e import DIST, PREFIX, TOKEN, WIDE_CSS, H, _stress, api, browser  # noqa: F401
from test_addon_e2e_polish import LARGE, STORE, open_large

from harmony_addon.app import create_app
from harmony_addon.auth import NoAuth
from harmony_addon.ha_client import HaUnavailable
from harmony_addon.ha_ws import RegistryUnavailable
from harmony_addon.options import Options

pytestmark = pytest.mark.skipif(not (DIST / "index.html").is_file(), reason="frontend not built")
SHOTS = os.environ.get("HH_SHOTS")
DOMS = ["light", "switch", "sensor", "scene", "script", "media_player", "climate", "cover", "fan", "input_boolean"]
ROOMS = ["Living room", "Kitchen", "Bedroom", "Hallway", "Garage", "Office", "Bathroom", "Garden"]


def areas():
    names = ROOMS + [f"Room {i}" for i in range(len(ROOMS), 25)]
    return [
        {"area_id": f"a{i}", "name": n, "floor_id": f"f{i % 4}" if i < 22 else None}
        for i, n in enumerate(names)
    ]


def build():
    """2,999 + the lamp = 3,000 visible entities plus ones the add-on must leave out (disabled, hidden, diagnostic)."""
    states, ents, devs = [], [], []
    for d in range(800):
        devs.append({"id": f"d{d}", "name": f"Device {d}", "area_id": f"a{d % 25}"})
    for i in range(2999):
        dom = DOMS[i % len(DOMS)]
        eid = f"{dom}.item_{i}"
        states.append({"entity_id": eid, "state": "on", "attributes": {"friendly_name": f"Item {i} {dom}"}})
        ent = {"entity_id": eid, "device_id": f"d{i % 800}", "platform": f"integ{i % 40}"}
        if i % 7 == 0:
            ent["labels"] = ["cozy"] if i % 14 == 0 else ["night", "cozy"]
        ents.append(ent)
    # the one a person looks for
    states.append({"entity_id": "light.living_lamp", "state": "off", "attributes": {"friendly_name": "Living room lamp"}})
    ents.append({"entity_id": "light.living_lamp", "device_id": None, "area_id": "a0", "platform": "hue", "labels": ["cozy"]})
    for n, extra in enumerate(
        [{"disabled_by": "user"}] * 5 + [{"hidden_by": "user"}] * 5 + [{"entity_category": "diagnostic"}] * 20
    ):
        eid = f"sensor.skip_{n}"
        states.append({"entity_id": eid, "state": "1", "attributes": {}})
        ents.append({"entity_id": eid, "platform": "hue", **extra})
    lists = {
        "config/area_registry/list": areas(),
        "config/floor_registry/list": [{"floor_id": f"f{i}", "name": n} for i, n in enumerate(["Ground", "First", "Second", "Outside"])],
        "config/label_registry/list": [{"label_id": "cozy", "name": "Cozy"}, {"label_id": "night", "name": "Night"}],
        "config/device_registry/list": devs,
        "config/entity_registry/list": ents,
    }
    return states, lists


STATES, LISTS = build()
SERVICES = [
    {"domain": d, "services": {"toggle": {"fields": {}}, "turn_on": {"fields": {}}}}
    for d in ("light", "switch", "scene", "script", "media_player")
]


class BigHa:
    def __init__(self, fail=None):
        self.fail = fail

    async def get(self, path):
        if self.fail:
            raise HaUnavailable(self.fail)
        return {"/states": STATES, "/services": SERVICES}[path]


class Registries:
    def __init__(self, ok=True):
        self.ok = ok

    async def lists(self):
        if not self.ok:
            raise RegistryUnavailable("auth_failed")
        return LISTS


async def site_for(aiohttp_server, tmp_path, ha, reg):
    app = create_app(
        tmp_path,
        options=Options("info"),
        auth=NoAuth(),
        static_dir=DIST,
        bundle_token=TOKEN,
        bundle_networks=None,
        ha_api=ha,
        ha_registry=reg,
    )
    server = await aiohttp_server(app)
    origin = f"http://127.0.0.1:{server.port}"
    assert (await api(origin, "PUT", "/api/v1/config/draft", json={"config": LARGE}, headers=H))[0] == 200
    return origin


@pytest.fixture
async def big(aiohttp_server, tmp_path):
    return await site_for(aiohttp_server, tmp_path, BigHa(), Registries())


async def shot(page, name):
    if SHOTS:
        d = Path(SHOTS)
        d.mkdir(parents=True, exist_ok=True)
        await page.screenshot(path=str(d / f"{name}.png"))


P = "hh-palette"
ROWS = "hh-palette .ent"


async def open_tab(browser, origin, width=1440, height=900):
    page = await open_large(browser, origin, width, height)
    if width <= 720:  # the palette is a bottom sheet on a phone
        await page.get_by_role("button", name="Palette", exact=True).click()
    await page.get_by_role("tab", name="Home Assistant", exact=True).click()
    await page.locator(f"{P} [role=status][aria-live]").get_by_text("3,000 entities").wait_for()
    return page


async def search(page, text):
    await page.locator(P).get_by_placeholder("Search entities, areas").fill(text)


async def test_collapsed_by_default_search_filter_group_and_drag(browser, big):
    page = await open_tab(browser, big)
    # the tab says Home Assistant (never "Home") and its label is not cut
    name = page.locator("hh-palette .tabs .tn")
    assert (await name.text_content()) == "Home Assistant"
    assert await name.evaluate("e => e.scrollWidth <= e.clientWidth")
    assert await page.get_by_role("tab", name="Home", exact=True).count() == 0
    # collapsed: floors as headers, no rows; excluded entities are not counted (3,000 shown)
    assert await page.locator(ROWS).count() == 0
    heads = page.locator(f"{P} .gh")
    assert await heads.count() >= 4
    assert await heads.first.get_attribute("aria-expanded") == "false"
    await shot(page, "01-collapsed-1440")

    # search: "Living room lamp" is found and its groups are open, in well under a second
    t0 = time.perf_counter()
    await search(page, "living room lamp")
    row = page.locator(ROWS, has_text="Living room lamp")
    await row.first.wait_for()
    assert time.perf_counter() - t0 < 2.0
    assert await page.locator(ROWS).count() <= 5
    assert await page.locator(f"{P} mark").count() >= 1
    await page.locator(f"{P} [role=status][aria-live]").get_by_text("match").wait_for()
    await shot(page, "02-search-1440")
    await search(page, "")
    await page.wait_for_function("() => !document.querySelector('hh-app').shadowRoot.querySelector('hh-studio').shadowRoot.querySelector('hh-palette').shadowRoot.querySelector('hh-ha-browser').shadowRoot.querySelector('.ent')")
    assert await page.locator(ROWS).count() == 0  # the previous (collapsed) state is back

    # type chips with counts, an "n filters" state, clear
    await page.locator(P).get_by_role("button", name="Type", exact=True).click()
    await page.locator(f"{P} .opt", has_text="Lights").get_by_role("checkbox").check()
    await page.locator(P).get_by_text("1 filter", exact=True).wait_for()
    await shot(page, "03-filter-chip-1440")
    await page.locator(P).get_by_role("button", name="Clear", exact=True).click()
    assert await page.locator(f"{P} .pills").count() == 0

    # area dropdown filter (searchable) + removable pill
    await page.locator(P).get_by_role("button", name="Area", exact=True).click()
    await page.locator(P).get_by_role("checkbox").first.wait_for()
    await page.locator(P).get_by_placeholder("Search areas").fill("kitchen")
    await page.locator(P).get_by_role("checkbox").first.check()
    await page.locator(P).get_by_text("Area: Kitchen").wait_for()
    await shot(page, "04-area-filter-1440")
    await page.locator(P).get_by_role("button", name="Remove filter Area: Kitchen").click()

    # group by Type / Device / Integration / Label (all have data), collapsed, with counts
    sel = page.locator(P).get_by_role("combobox", name="Group By")
    assert await sel.locator("option").all_text_contents() == ["Area", "Type", "Device", "Integration", "Label"]
    await sel.select_option("type")
    await page.locator(f"{P} .gh", has_text="Lights").first.wait_for()
    assert await page.locator(ROWS).count() == 0
    await shot(page, "05-by-type-1440")
    await sel.select_option("label")
    await page.locator(P).get_by_role("button", name="Cozy").wait_for()
    await page.locator(P).get_by_role("button", name="No label").wait_for()
    await sel.select_option("integration")
    await page.locator(P).get_by_role("button", name="integ0", exact=False).first.wait_for()
    await sel.select_option("area")

    # open one area by keyboard (headers are buttons with aria-expanded) and walk the rows with arrows
    first = page.locator(P).get_by_role("button", name="Ground")
    await first.focus()
    await page.keyboard.press("ArrowRight")  # a floor opens...
    assert await first.get_attribute("aria-expanded") == "true"
    deep = (
        "() => { let e = document.activeElement; while (e && e.shadowRoot && e.shadowRoot.activeElement) e = e.shadowRoot.activeElement;"
        " return e ? e.textContent.trim() : ''; }"
    )
    before = await page.evaluate(deep)
    await page.keyboard.press("ArrowDown")  # ... and the arrows walk on to the first area inside it
    after = await page.evaluate(deep)
    assert after and after != before
    await page.keyboard.press("ArrowUp")
    assert await page.evaluate(deep) == before
    await page.keyboard.press("ArrowLeft")  # Left on an open header collapses it
    assert await first.get_attribute("aria-expanded") == "false"

    # "Used in This Config" and the recently used list, at the top
    await page.evaluate(
        "() => " + STORE + ".edit(c => { c.keys = c.keys || {}; c.keys.play = { ha: { service: 'light.toggle', target: { entity_id: 'light.living_lamp' } } }; })"
    )
    await page.locator(P).get_by_role("button", name="Used in This Config").wait_for()
    await page.locator(P).get_by_role("button", name="Used in This Config").click()
    used = page.locator(ROWS, has_text="Living room lamp")
    await used.first.wait_for()
    assert "toggle" in (await used.first.text_content())  # says which service the config calls
    await shot(page, "06-used-1440")

    # favourite the lamp: it appears under Favourites
    await page.locator(P).get_by_role("button", name="Add Living room lamp to favourites").first.click()
    await page.locator(f"{P} .gh", has_text="Favourites").wait_for()

    # drag a search result onto a key of the remote: toggle on that entity
    await search(page, "living room lamp")
    # the favourite row is already there while the debounced search is pending; measure once the results settle
    await page.locator(f"{P} [role=status][aria-live]").get_by_text("match").wait_for()
    ent = page.locator(ROWS, has_text="Living room lamp").first
    await ent.wait_for()
    # the list may still be re-rendering (rows are drawn in a budget) when the CPU is loaded: measure
    # until the row holds still, and redo the whole gesture if a re-render swallowed it
    landed = "() => (" + STORE + ".config.keys.stop || {}).ha?.target?.entity_id === 'light.living_lamp'"
    for attempt in range(4):
        await ent.scroll_into_view_if_needed()
        eb = await ent.bounding_box()
        for _ in range(20):
            await page.wait_for_timeout(100)
            nb = await ent.bounding_box()
            if nb == eb:
                break
            eb = nb
        kb = await page.locator("hh-remote [data-key=stop]").bounding_box()
        await page.mouse.move(eb["x"] + 20, eb["y"] + 15)
        await page.mouse.down()
        await page.mouse.move(eb["x"] + 70, eb["y"] + 45, steps=5)
        await page.mouse.move(kb["x"] + kb["width"] / 2, kb["y"] + kb["height"] / 2, steps=12)
        await page.mouse.up()
        try:
            await page.wait_for_function(landed, timeout=8000 if attempt == 3 else 2500)
            break
        except Exception:
            if attempt == 3:
                raise
    act = await page.evaluate("() => " + STORE + ".config.keys.stop.ha.service")
    assert act == "light.toggle"
    await shot(page, "07-after-drag-1440")

    # it is remembered per browser: a reload keeps group-by and favourites
    await page.reload()
    await page.locator("hh-remote [data-key]").first.wait_for()
    await page.get_by_role("tab", name="Home Assistant", exact=True).click()
    await page.locator(f"{P} .gh", has_text="Favourites").wait_for()


async def test_the_dom_stays_bounded_and_typing_stays_fast(browser, big):
    page = await open_tab(browser, big)
    await page.locator(P).get_by_role("button", name="Expand All").click()
    await page.locator(f"{P} .more").first.wait_for()
    n = await page.locator(ROWS).count()
    assert 20 < n <= 300, n  # 3,000 entities never become 3,000 rows
    assert await page.locator(f"{P} .more").count() >= 1
    # a keystroke burst over the full, expanded list stays inside a generous budget
    t0 = time.perf_counter()
    for ch in "item 12":
        await page.keyboard.type(ch) if await page.locator(f"{P} input.q:focus").count() else None
    await page.locator(P).get_by_placeholder("Search entities, areas").press_sequentially("item 12", delay=5)
    await page.locator(f"{P} [role=status][aria-live]").get_by_text("match").wait_for()
    assert time.perf_counter() - t0 < 5.0
    assert await page.locator(ROWS).count() <= 300
    await shot(page, "08-expanded-capped-1440")


async def test_narrow_width_states(browser, big):
    page = await open_tab(browser, big, 400, 800)
    tab = page.get_by_role("tab", name="Home Assistant", exact=True)
    assert await tab.is_visible()
    await shot(page, "10-collapsed-400")
    await search(page, "living room")
    await page.locator(ROWS).first.wait_for()
    await shot(page, "11-search-400")
    over = await page.evaluate(
        "() => { const b = document.querySelector('hh-app').shadowRoot.querySelector('hh-studio').shadowRoot.querySelector('hh-palette').shadowRoot.querySelector('.body'); return b.scrollWidth - b.clientWidth; }"
    )
    assert over <= 1, "the Home Assistant tab overflows sideways at 400 px"


async def test_unreadable_says_why_and_retry_recovers(aiohttp_server, tmp_path, browser):
    ha = BigHa(fail="status_503")
    origin = await site_for(aiohttp_server, tmp_path, ha, Registries())
    page = await open_large(browser, origin)
    await page.get_by_role("tab", name="Home Assistant", exact=True).click()
    alert = page.locator(P).get_by_role("alert")
    await alert.get_by_text("Home Assistant cannot be read").wait_for()
    await alert.get_by_text("HTTP 503").wait_for()
    await page.locator(P).get_by_placeholder("light.living_room").wait_for()  # typed fallback stays
    await shot(page, "09-unreadable-1440")
    ha.fail = None
    await alert.get_by_role("button", name="Retry").click()
    await page.locator(f"{P} [role=status][aria-live]").get_by_text("3,000 entities").wait_for()


async def test_without_registries_it_groups_by_type_and_says_so(aiohttp_server, tmp_path, browser):
    origin = await site_for(aiohttp_server, tmp_path, BigHa(), Registries(ok=False))
    page = await open_large(browser, origin)
    await page.get_by_role("tab", name="Home Assistant", exact=True).click()
    note = page.locator(P).get_by_role("note")
    await note.get_by_text("grouped by type only").wait_for()
    assert await page.locator(P).get_by_role("combobox", name="Group By").locator("option").all_text_contents() == ["Type"]
    await page.locator(f"{P} .gh", has_text="Lights").first.wait_for()
    await shot(page, "12-no-registries-1440")


async def test_action_picker_and_add_device_wizard_use_the_same_navigator(browser, big):
    page = await open_large(browser, big)
    # the key's action picker: browse, search, pick -> the entity field is filled
    await page.locator("hh-remote [data-key=record]").click()
    insp = page.locator("hh-inspector")
    await insp.get_by_role("button", name="Home Assistant action").click()
    await insp.get_by_role("button", name="Browse Home Assistant entities").click()
    await insp.get_by_placeholder("Search entities, areas").fill("living room lamp")
    await insp.locator("hh-ha-browser .ent", has_text="Living room lamp").first.click()
    assert await insp.get_by_placeholder("Search entities", exact=True).input_value() == "light.living_lamp"
    await shot(page, "13-action-picker-1440")
    # the wizard's Home Assistant step: only its domains, the same search and filters
    await page.goto(f"{big}{PREFIX}/#/devices/new")
    await page.get_by_role("button", name="Home Assistant entity").click()
    nav = page.locator("hh-add-device hh-ha-browser")
    await nav.get_by_placeholder("Search entities, areas").fill("living room lamp")
    row = nav.locator(".ent", has_text="Living room lamp").first
    await row.wait_for()
    assert await nav.get_by_role("button", name="Sensors").count() == 0  # not a domain a device can be made from
    await shot(page, "14-wizard-1440")
    await row.click()
    await page.get_by_role("heading", name="Pick a Home Assistant Entity").wait_for(state="detached")


# every browser-side check below is about structure (one line, nothing cut or sideways), never a pixel constant tuned to one font
SHADOW_ALL = """(sel) => { const out = []; const walk = (root) => { for (const el of root.querySelectorAll('*')) { if (el.shadowRoot) walk(el.shadowRoot); } out.push(...root.querySelectorAll(sel)); };
  walk(document.querySelector('hh-app').shadowRoot); return out; }"""


async def check_top_of_tab(page):
    box = lambda sel: page.locator(sel).first.bounding_box()
    search = await box(f"{P} input.q")
    head = await box(f"{P} .gh")
    # the list starts within a few row heights of the search box: filters and group-by are one line each
    assert head["y"] - search["y"] < 5 * head["height"], "the list starts too far below the search box"
    tops = lambda sel: page.locator(sel).evaluate_all("els => [...new Set(els.map(e => Math.round(e.getBoundingClientRect().top)))]")
    ys = await tops(f"{P} .fchips .fchip")
    assert len(ys) == 1, f"the filter row wraps: {ys}"
    centres = await page.locator(f"{P} .tools > *").evaluate_all(
        "els => els.map(e => { const r = e.getBoundingClientRect(); return Math.round(r.top + r.height / 2); })"
    )
    assert max(centres) - min(centres) <= 3, f"the group-by row wraps: {centres}"
    # nothing in the tab's own controls is wider than its box (a cut label shows as ellipsis, never a sideways scroll)
    over = await page.locator(P).evaluate("(el) => { const b = el.shadowRoot.querySelector('.body'); return b.scrollWidth - b.clientWidth; }")
    assert over <= 1, over


async def test_the_top_of_the_tab_is_compact_and_the_tab_icons_stay_put(browser, big):
    page = await open_tab(browser, big)
    await check_top_of_tab(page)
    # collapsed, the 4 floors + "no floor" + the pinned list are all in view; open one floor and its areas join them
    await page.locator(P).get_by_role("button", name="Ground").click()
    await page.locator(f"{P} .gh.d1").first.wait_for()
    heads = await page.locator(f"{P} .gh").evaluate_all(
        "(hs, h) => hs.filter(x => { const r = x.getBoundingClientRect(); return r.top >= 0 && r.bottom <= h; }).length",
        900,
    )
    assert heads >= 8, heads
    # one filter row: Type, Area, Label, Integration and the options menu on one line
    ys = await page.locator(f"{P} .fchips .fchip").evaluate_all("els => [...new Set(els.map(e => Math.round(e.getBoundingClientRect().top)))]")
    assert len(ys) == 1, ys
    # switching tabs does not move the other icons under the cursor
    tabs = page.locator("hh-palette [role=tab]")
    before = await tabs.evaluate_all("els => els.map(e => Math.round(e.getBoundingClientRect().left))")
    await tabs.nth(0).click()
    await page.locator("hh-palette [role=tab][aria-selected=true][aria-label=Devices]").wait_for()
    assert await tabs.evaluate_all("els => els.map(e => Math.round(e.getBoundingClientRect().left))") == before
    # every icon has a tooltip (its name) on hover and keyboard focus
    await tabs.nth(1).hover()
    tab = await tabs.nth(1).element_handle()
    await page.wait_for_function("e => getComputedStyle(e, '::after').opacity === '1'", arg=tab)
    assert await tab.evaluate("e => getComputedStyle(e, '::after').content") == '"Activities"'
    await shot(page, "15-tab-tooltip-1440")


TAB_NAMES = ["Devices", "Activities", "Home Assistant", "Widgets"]


async def check_tab_label(page):
    """the selected tab's name is whole or absent: never cut"""
    st = await page.locator(f"{P} .tn").evaluate(
        "e => ({ hidden: getComputedStyle(e).visibility === 'hidden', text: e.textContent, ok: e.scrollWidth <= e.clientWidth })"
    )
    assert st["hidden"] or st["ok"], f"tab label cut: {st}"
    return st


@pytest.mark.parametrize("size", [(1440, 900), (1280, 720), (1024, 700), (900, 700)])
@pytest.mark.parametrize("zoom", [1, 1.25])
async def test_the_top_of_the_tab_survives_a_wide_fallback_font(browser, big, size, zoom):
    page = await open_tab(browser, big, *size)
    await page.add_style_tag(content=WIDE_CSS + (f"html{{zoom:{zoom}}}" if zoom != 1 else ""))
    await page.evaluate("() => document.fonts.ready")
    await shot(page, f"16-wide-font-{size[0]}-{zoom}")
    await check_top_of_tab(page)
    # chips: all four show icon + word, or all four are icons alone (never a mix, never an abbreviation)
    modes = await page.locator(f"{P} .fchips .fchip .l").evaluate_all("els => els.map(e => getComputedStyle(e).display !== 'none')")
    assert len(set(modes)) == 1, modes
    for name in TAB_NAMES:
        await page.get_by_role("tab", name=name, exact=True).click()
        st = await check_tab_label(page)
        assert st["hidden"] or st["text"] == name
    await page.get_by_role("tab", name="Home Assistant", exact=True).click()
    await page.locator(P).get_by_role("button", name="Ground").click()
    await page.locator(f"{P} .gh.d1").first.wait_for()


@pytest.fixture
async def classic(browser):
    """a browser with real (classic, layout-taking) scrollbars, like Chrome on macOS with a mouse; headless hides them by default"""
    async with pw.async_playwright() as p:
        b = await p.chromium.launch(executable_path=os.environ.get("HH_CHROMIUM"), ignore_default_args=["--hide-scrollbars"])
        _stress(b)
        yield b
        await b.close()


async def test_the_filter_mode_does_not_depend_on_the_list_height(classic, big):
    """a long open list gets a scrollbar; the filter buttons' icon+word / icons mode must not change with it"""
    for width in (1440, 1280, 1024):
        page = await open_tab(classic, big, width)
        body = "e => { const b = e.shadowRoot.querySelector('.body'); return b.offsetWidth - b.clientWidth; }"
        short = await page.locator(f"{P} .fchips").evaluate("e => e.classList.contains('icons')")
        gutter = await page.locator(P).evaluate(body)
        await page.locator(f"{P} select[aria-label='Group by']").select_option("type")
        await page.locator(f"{P} .gh").first.click()
        await page.locator(ROWS).first.wait_for()
        scrolls = await page.locator(P).evaluate("e => { const b = e.shadowRoot.querySelector('.body'); return b.scrollHeight > b.clientHeight; }")
        assert scrolls, "the list should be long enough to scroll"
        assert await page.locator(P).evaluate(body) > 0, "this browser has overlay scrollbars, so the test proves nothing"
        await page.wait_for_timeout(100)
        now = await page.locator(f"{P} .fchips").evaluate("e => e.classList.contains('icons')")
        assert now == short, f"the filter mode changed when the list grew (width {width})"
        assert await page.locator(P).evaluate(body) == gutter, "the scrollbar changes the width the controls get"
        await shot(page, f"20-long-list-{width}")
        await page.close()


@pytest.mark.parametrize("size", [(1440, 900), (1280, 800), (1024, 700)])
@pytest.mark.parametrize("by", ["area", "type", "device", "integration", "label"])
async def test_the_header_keeps_its_layout_in_every_group_by_mode(classic, big, by, size):
    page = await open_tab(classic, big, *size)
    await page.locator(f"{P} select[aria-label='Group by']").select_option(by)
    await page.locator(f"{P} .gh").first.wait_for()
    await page.wait_for_timeout(150)
    await shot(page, f"21-group-{by}-{size[0]}")
    boxes = await page.locator(f"{P} hh-ha-browser").evaluate(
        """e => { const r = e.shadowRoot;
          const o = (s) => { const b = r.querySelector(s).getBoundingClientRect(); return { top: b.top, bottom: b.bottom }; };
          return { search: o('.search'), chips: o('.fchips'), tools: o('.tools'), sum: o('.sum'), list: o('.list') }; }"""
    )
    order = ["search", "chips", "tools", "sum", "list"]
    for a, b in pairwise(order):
        assert boxes[a]["bottom"] <= boxes[b]["top"] + 0.5, f"{by}: {a} overlaps {b}: {boxes}"
    first = await page.locator(f"{P} .gh").first.bounding_box()
    assert first["y"] >= boxes["chips"]["bottom"], f"{by}: the list starts above the filter row"
    assert boxes["tools"]["bottom"] - boxes["tools"]["top"] <= 40, boxes
    # the header's controls are the same size whichever way it groups and stay inside their row
    fit = await page.locator(f"{P} hh-ha-browser").evaluate(
        """e => { const t = e.shadowRoot.querySelector('.tools'), tb = t.getBoundingClientRect(), sel = t.querySelector('select').getBoundingClientRect();
          return { select: Math.round(sel.width), inside: [...t.children].every(c => c.getBoundingClientRect().right <= tb.right + 0.5 && c.getBoundingClientRect().left >= tb.left - 0.5) }; }"""
    )
    assert fit["inside"], f"{by}: the group-by row spills out of its box {fit}"
    # the four filter buttons sit inside their row (a row narrower than its buttons let them spill over the group-by row)
    chips = await page.locator(f"{P} hh-ha-browser").evaluate(
        """e => { const r = e.shadowRoot, row = r.querySelector('.fchips').getBoundingClientRect(), host = e.getBoundingClientRect();
          return [...r.querySelectorAll('.fchips .fchip')].map(c => { const b = c.getBoundingClientRect(); return b.width > 20 && b.right <= row.right + 0.5 && b.bottom <= row.bottom + 0.5 && b.top >= row.top - 0.5; }); }"""
    )
    assert chips == [True] * 4, f"{by}: a filter button is cut or pushed out: {chips}"


async def test_the_group_by_box_is_one_width_in_every_mode(classic, big):
    page = await open_tab(classic, big, 1280, 800)
    seen = set()
    for by in ("area", "type", "device", "integration", "label"):
        await page.locator(f"{P} select[aria-label='Group by']").select_option(by)
        await page.wait_for_timeout(100)
        seen.add(await page.locator(f"{P} select[aria-label='Group by']").evaluate("e => Math.round(e.getBoundingClientRect().width)"))
    assert len(seen) == 1, f"the group-by box resizes with its choice: {seen}"
