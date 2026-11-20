# ruff: noqa: F811
"""Layout under a wide fallback font (what a CI runner or another OS picks for system-ui): the palette tabs, the
inspector, the Bluetooth panel and both wizards must not spill sideways, wrap their one-line rows or cut a label
without a way to read it. Skipped like test_addon_e2e.py. HH_SHOTS=<dir> writes a screenshot of every state.
    HH_CHROMIUM=/usr/bin/google-chrome uv run pytest tests/py/test_addon_e2e_layout.py -n0
"""

import os
from pathlib import Path

import pytest
from test_addon_bt_e2e import REMOTE, bt, open_bt, site  # noqa: F401
from test_addon_e2e import DIST, WIDE_CSS, browser, open_editor  # noqa: F401
from test_addon_e2e_polish import large, open_large  # noqa: F401

pytestmark = pytest.mark.skipif(not (DIST / "index.html").is_file(), reason="frontend not built")
SHOTS = os.environ.get("HH_SHOTS")
SIZES = [(1440, 900), (1024, 700)]

# spills: an element whose content is wider than its box while it does not scroll or clip (ignores the tab tooltips, which hang below
# their button on purpose); rows: direct children of a grid or flex row that should be one line but are taller than 1.7 lines
AUDIT = """() => {
  const out = { spill: [], page: document.documentElement.scrollWidth - window.innerWidth };
  const walk = (root) => {
    for (const el of root.querySelectorAll('*')) {
      if (el.shadowRoot) walk(el.shadowRoot);
      const cs = getComputedStyle(el);
      if (cs.display === 'inline' || cs.display === 'none' || el.matches('.tabs button, .tabs button *, .tw, .tw *')) continue;
      if (el.scrollWidth > el.clientWidth + 1 && cs.overflowX === 'visible' && el.clientWidth > 0)
        out.spill.push((el.className || el.tagName) + ':' + (el.textContent || '').trim().slice(0, 30) + ' ' + el.scrollWidth + '>' + el.clientWidth);
    }
  };
  walk(document.querySelector('hh-app').shadowRoot);
  return out;
}"""
# an element that text-overflow clips (ellipsis) while the text is not otherwise readable: reported, never asserted
CLIPPED = """() => {
  const out = [];
  const walk = (root) => { for (const el of root.querySelectorAll('*')) { if (el.shadowRoot) walk(el.shadowRoot);
    if (getComputedStyle(el).textOverflow === 'ellipsis' && el.scrollWidth > el.clientWidth + 1 && !el.title && !el.getAttribute('aria-label')) out.push((el.className || el.tagName) + ':' + el.textContent.trim().slice(0, 30)); } };
  walk(document.querySelector('hh-app').shadowRoot); return out; }"""


async def wide(page, size, name):
    await page.set_viewport_size({"width": size[0], "height": size[1]})
    await page.evaluate("(css) => { const s = new CSSStyleSheet(); s.replaceSync(css); document.adoptedStyleSheets = [...document.adoptedStyleSheets, s]; }", WIDE_CSS)
    await page.evaluate("() => document.fonts.ready")
    await page.wait_for_timeout(200)
    if SHOTS:
        Path(SHOTS).mkdir(parents=True, exist_ok=True)
        await page.screenshot(path=f"{SHOTS}/{name}-{size[0]}.png")


async def audit(page, where):
    got = await page.evaluate(AUDIT)
    assert got["page"] <= 0, f"{where}: the page scrolls sideways by {got['page']}"
    assert got["spill"] == [], f"{where}: content spills out of its box: {got['spill']}"


# every row of a .cells grid but the last is filled edge to edge: no empty cell sits before a later item
HOLES = """() => {
  const bad = [];
  const walk = (root) => { for (const g of root.querySelectorAll('.cells')) {
    const kids = [...g.children].filter((k) => k.getBoundingClientRect().height > 0), W = g.getBoundingClientRect().width;
    const rows = new Map(); for (const k of kids) { const r = k.getBoundingClientRect(); const t = Math.round(r.top); rows.set(t, (rows.get(t) || 0) + r.width); }
    const tops = [...rows.keys()].sort((a, b) => a - b);
    tops.slice(0, -1).forEach((t) => { const used = rows.get(t); if (W - used > used / 20 + 40) bad.push(g.getAttribute('aria-label') + ': row at ' + t + ' fills ' + Math.round(used) + ' of ' + Math.round(W)); });
  } for (const el of root.querySelectorAll('*')) if (el.shadowRoot) walk(el.shadowRoot); };
  walk(document.querySelector('hh-app').shadowRoot); return bad; }"""


@pytest.mark.parametrize("size", SIZES)
async def test_palette_tabs_inspector_and_tab_strip_under_a_wide_font(browser, large, size):
    page = await open_large(browser, large, *size)
    await wide(page, size, "20-remote")
    await audit(page, "remote")
    for i, tab in enumerate(("Devices", "Activities", "Home Assistant", "Widgets")):
        await page.get_by_role("tab", name=tab, exact=True).click()
        await wide(page, size, f"21-palette-{i}-{tab.split()[0].lower()}")
        await audit(page, tab)
        # the label is whole or absent
        st = await page.locator("hh-palette .tn").evaluate("e => [getComputedStyle(e).visibility === 'hidden', e.scrollWidth <= e.clientWidth]")
        assert st[0] or st[1], tab
    # the chips of the Devices/Activities/Widgets tabs: equal rows, one line each
    for tab in ("Devices", "Activities", "Widgets"):
        await page.get_by_role("tab", name=tab, exact=True).click()
        heights = await page.locator("hh-palette .chip2").evaluate_all("els => [...new Set(els.map(e => Math.round(e.getBoundingClientRect().height)))]")
        assert len(heights) <= 1, (tab, heights)
        assert await page.evaluate(HOLES) == [], tab
    # the inspector's free-keys list: equal rows
    heights = await page.locator("hh-inspector .free button, hh-inspector .chip2").evaluate_all(
        "els => [...new Set(els.map(e => Math.round(e.getBoundingClientRect().height)))]"
    )
    assert len(heights) <= 1, heights
    assert await page.evaluate(HOLES) == []


@pytest.mark.parametrize("size", SIZES)
async def test_bluetooth_panel_wizards_and_key_map_under_a_wide_font(browser, bt, site, size):
    bt.add_host(configured=False, result=False)
    bt.add_remote(configured=False)
    bt.candidates({"addr": "11:22:33:44:55:09", "name": "Homatics Remote", "rssi": -48}, {"addr": "11:22:33:44:55:02", "rssi": -80})
    page, _ = await open_bt(browser, site)
    await page.get_by_text("Living room box").first.wait_for()
    await wide(page, size, "30-bt-panel")
    await audit(page, "panel")
    await page.get_by_role("link", name="Add Bluetooth device").click()
    await page.get_by_role("radio", name="Automatic (recommended)").wait_for()
    await wide(page, size, "31-bt-host-start")
    await audit(page, "host wizard")
    await page.evaluate("location.hash = '#/bluetooth'")
    await page.get_by_role("link", name="Add to config").first.click()
    await page.get_by_role("heading", name="Name it and choose its commands").wait_for()
    await wide(page, size, "32-bt-host-setup")
    await audit(page, "host setup")
    await page.evaluate("location.hash = '#/bluetooth'")
    await page.get_by_role("link", name="Add Bluetooth remote").click()
    await page.get_by_role("button", name="Scan for remotes").click()
    await page.get_by_role("list", name="Remotes found").wait_for()
    await wide(page, size, "33-bt-remote-candidates")
    await audit(page, "remote wizard")
    await page.get_by_role("button", name="Pair with Homatics Remote").click()
    await page.get_by_role("heading", name="Choose what its buttons do").wait_for()
    await wide(page, size, "34-bt-remote-keys-step")
    await audit(page, "remote key step")
    # the key map: every button name is shown whole, one value picker per row
    cut = await page.locator(".kr b").evaluate_all("els => els.filter(e => e.scrollWidth > e.clientWidth + 1).map(e => e.textContent)")
    assert cut == [], cut
