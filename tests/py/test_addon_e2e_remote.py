# ruff: noqa: F811
"""The remote drawing's key labels: readable names, and every label sits inside its key, centred, at one size per key class.
Structural (no pixel positions), so a wider CI font passes. HH_REMOTE_SHOT=<png> also writes a screenshot of the remote.
    HH_CHROMIUM=/usr/bin/google-chrome uv run pytest tests/py/test_addon_e2e_remote.py -n0
"""

import os

import pytest
from test_addon_e2e import DIST, STUDIO_YAML, browser, open_editor, site  # noqa: F401
from test_addon_e2e_polish import large, open_large  # noqa: F401

pytestmark = pytest.mark.skipif(not (DIST / "index.html").is_file(), reason="frontend not built")

# per key: its label text boxes and the cap box, in the page's pixels
PROBE = """() => {
  const r = document.querySelector('hh-app').shadowRoot.querySelector('hh-remote') || null;
  const root = (function f(n) { for (const el of n.querySelectorAll('*')) { if (el.tagName === 'HH-REMOTE') return el.shadowRoot; if (el.shadowRoot) { const x = f(el.shadowRoot); if (x) return x; } } return null; })(document.querySelector('hh-app').shadowRoot);
  const out = [];
  for (const k of root.querySelectorAll('[data-key]')) {
    const cap = k.querySelector('.cap').getBoundingClientRect();
    for (const t of k.querySelectorAll('text')) {
      const b = t.getBoundingClientRect();
      if (!b.width) continue;
      out.push({ key: k.dataset.key, cls: k.getAttribute('class'), text: t.textContent, size: parseFloat(getComputedStyle(t).fontSize),
        tw: b.width, th: b.height, cx: b.x + b.width / 2 - (cap.x + cap.width / 2), cy: b.y + b.height / 2 - (cap.y + cap.height / 2),
        cw: cap.width, ch: cap.height, over: Math.max(cap.x - b.x, b.right - cap.right, cap.y - b.y, b.bottom - cap.bottom) });
    }
  }
  return out;
}"""


async def open_remote(browser, site):
    page = await open_editor(
        browser, site, viewport={"width": 1440, "height": 1000}, device_scale_factor=1.5
    )
    await page.get_by_text("Set up your first hub config").wait_for()
    await page.get_by_role("button", name="Import YAML", exact=True).click()
    await page.locator("textarea").fill(STUDIO_YAML)
    await page.get_by_role("button", name="Import As Draft").click()
    await page.locator("hh-remote svg.scr").wait_for()
    await page.evaluate("() => document.fonts.ready")
    await page.wait_for_timeout(300)
    return page


async def test_labels_fit_and_centre_on_their_keys(browser, site):
    page = await open_remote(browser, site)
    shot = os.environ.get("HH_REMOTE_SHOT")
    if shot:
        await page.screenshot(path=shot)
        await page.set_viewport_size({"width": 1280, "height": 800})
        await page.wait_for_timeout(300)
        await page.screenshot(path=shot.replace(".png", "-1280.png"))
        await page.set_viewport_size({"width": 1440, "height": 1000})
    got = await page.evaluate(PROBE)
    assert len(got) >= 10
    for t in got:
        where = f"{t['key']} '{t['text']}'"
        assert t["over"] <= 0.5, f"{where}: text overflows its key by {t['over']:.1f}px"
        assert abs(t["cx"]) <= max(1.5, 0.1 * t["cw"]), (
            f"{where}: not centred horizontally ({t['cx']:.1f}px)"
        )
        assert abs(t["cy"]) <= max(2.5, 0.1 * t["ch"]), (
            f"{where}: not centred vertically ({t['cy']:.1f}px)"
        )
    # one font size per key class
    sizes: dict[str, set[float]] = {}
    for t in got:
        grp = next(c for c in t["cls"].split() if c.startswith("g-"))
        sizes.setdefault(grp, set()).add(round(t["size"], 1))
    for grp, s in sizes.items():
        assert len(s) == 1, f"{grp}: mixed label sizes {s}"


CHIPS = """() => {
  const find = (n) => { for (const el of n.querySelectorAll('*')) { if (el.tagName === 'HH-INSPECTOR') return el.shadowRoot; if (el.shadowRoot) { const x = find(el.shadowRoot); if (x) return x; } } return null; };
  return [...find(document.querySelector('hh-app').shadowRoot).querySelectorAll('.free.grid button')].map((b) => ({ text: b.textContent.trim(), cut: b.scrollWidth - b.clientWidth, x: Math.round(b.getBoundingClientRect().x), w: b.getBoundingClientRect().width, h: b.getBoundingClientRect().height }));
}"""


async def test_free_key_chips_are_readable_names_that_are_not_cut(browser, site):
    page = await open_remote(browser, site)
    chips = await page.evaluate(CHIPS)
    names = {c["text"] for c in chips}
    assert {
        "Previous Channel",
        "Upper Bulb",
        "Lower Socket",
        "Volume Down",
        "Fast Forward",
        "DVR",
    } <= names
    for c in chips:
        assert "_" not in c["text"], c
        assert c["cut"] <= 0, f"{c['text']} is cut"
        assert c["h"] < 40, f"{c['text']} wraps"
    # short labels share one cell width; a long one spans exactly two cells (rows stay aligned)
    one = min(round(c["w"]) for c in chips)
    assert all(
        round(c["w"]) in (one, round(2 * one + 6)) or abs(c["w"] - 2 * one) < 12 for c in chips
    ), "free-key cells are not on one grid"
    short = [c for c in chips if len(c["text"]) <= 9]
    assert len({c["x"] for c in short}) >= 2, "short free keys sit in one column"
    assert len({round(c["h"]) for c in chips}) == 1, "uneven free-key heights"


async def test_page_tour_screenshots(browser, large):
    """HH_PAGES_SHOTS=<dir> writes pages-<route>-<width>.png for every page; skipped otherwise."""
    out = os.environ.get("HH_PAGES_SHOTS")
    if not out:
        pytest.skip("set HH_PAGES_SHOTS")
    from test_addon_e2e import PREFIX
    from test_addon_e2e_polish import ROUTES

    for width in (1600, 1280):
        page = await open_large(browser, large, width, 1000)
        for route in [*ROUTES, "bluetooth"]:
            await page.goto(f"{large}{PREFIX}/#/{route}")
            await page.wait_for_timeout(700)
            await page.screenshot(path=f"{out}/pages-{route.replace('/', '-')}-{width}.png")
        await page.context.close()


async def test_device_icon_picker_sets_the_icon_in_the_draft(browser, large):
    from test_addon_e2e import PREFIX

    page = await open_large(browser, large, 1440, 900)
    await page.goto(f"{large}{PREFIX}/#/devices/tv")
    await page.locator("details.iconpick summary").wait_for()
    await page.locator("details.iconpick summary").click()
    picker = page.locator("hh-icon-picker")
    await picker.locator("input[type=search]").fill("music")
    await picker.get_by_role("button", name="Music", exact=True).click()
    icon = await page.evaluate(
        "document.querySelector('hh-app').store.config.devices.find(d => d.id === 'tv').icon"
    )
    assert icon == "music"
    await picker.get_by_role("button", name="Use the Default Icon").click()
    assert (
        await page.evaluate(
            "document.querySelector('hh-app').store.config.devices.find(d => d.id === 'tv').icon"
        )
        is None
    )
    await page.context.close()


PALETTE = """() => {
  const find = (n) => { for (const el of n.querySelectorAll('*')) { if (el.tagName === 'HH-PALETTE') return el.shadowRoot; if (el.shadowRoot) { const x = find(el.shadowRoot); if (x) return x; } } return null; };
  return [...find(document.querySelector('hh-app').shadowRoot).querySelectorAll('.cells')].map((g) => [...g.querySelectorAll('.chip2')].map((b) => {
    const r = b.getBoundingClientRect(), s = b.querySelector('span');
    return { text: s.textContent.trim(), x: Math.round(r.x), w: r.width, h: r.height, cut: s.scrollWidth - s.clientWidth, cx: s.getBoundingClientRect().x + s.getBoundingClientRect().width / 2 - (r.x + r.width / 2) };
  }));
}"""


async def test_palette_commands_are_readable_and_sit_on_a_uniform_grid(browser, site):
    page = await open_remote(browser, site)
    groups = await page.evaluate(PALETTE)
    texts = {c["text"] for g in groups for c in g}
    assert {"Power On", "Volume Up", "Volume Down", "Mute", "HDMI 1", "OK", "Menu"} <= texts
    await page.set_viewport_size({"width": 1600, "height": 1000})
    await page.wait_for_timeout(300)
    groups = await page.evaluate(PALETTE)
    wide = [g for g in groups if len(g) >= 2 and max(len(c["text"]) for c in g) <= 9]
    assert wide and all(len({c["x"] for c in g}) >= 2 for g in wide), (
        "short palette labels sit in one column at 1600px"
    )
    for g in groups:
        one = min(round(c["w"]) for c in g)
        assert (
            all(round(c["w"]) in (one, 2 * one + 6) for c in g)
            and len({round(c["h"]) for c in g}) == 1
        ), f"uneven cells: {g}"
        for c in g:
            assert "_" not in c["text"], c
            assert c["cut"] <= 0, f"{c['text']} is cut"
