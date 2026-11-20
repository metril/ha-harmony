# ruff: noqa: F811
"""The remote drawing's key labels: readable names, and every label sits inside its key, centred, at one size per key class.
Structural (no pixel positions), so a wider CI font passes. HH_REMOTE_SHOT=<png> also writes a screenshot of the remote.
    HH_CHROMIUM=/usr/bin/google-chrome uv run pytest tests/py/test_addon_e2e_remote.py -n0
"""

import os

import pytest
from test_addon_e2e import DIST, STUDIO_YAML, browser, open_editor, site  # noqa: F401

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
    page = await open_editor(browser, site, viewport={"width": 1440, "height": 1000}, device_scale_factor=1.5)
    await page.get_by_text("Set up your first hub config").wait_for()
    await page.get_by_role("button", name="Import YAML", exact=True).click()
    await page.locator("textarea").fill(STUDIO_YAML)
    await page.get_by_role("button", name="Import as draft").click()
    await page.locator("hh-remote svg.scr").wait_for()
    await page.evaluate("() => document.fonts.ready")
    await page.wait_for_timeout(300)
    return page


async def test_labels_fit_and_centre_on_their_keys(browser, site):
    page = await open_remote(browser, site)
    shot = os.environ.get("HH_REMOTE_SHOT")
    if shot:
        await page.screenshot(path=shot)
    got = await page.evaluate(PROBE)
    assert len(got) >= 10
    for t in got:
        where = f"{t['key']} '{t['text']}'"
        assert t["over"] <= 0.5, f"{where}: text overflows its key by {t['over']:.1f}px"
        assert abs(t["cx"]) <= max(1.5, 0.1 * t["cw"]), f"{where}: not centred horizontally ({t['cx']:.1f}px)"
        assert abs(t["cy"]) <= max(2.5, 0.1 * t["ch"]), f"{where}: not centred vertically ({t['cy']:.1f}px)"
    # one font size per key class
    sizes: dict[str, set[float]] = {}
    for t in got:
        grp = next(c for c in t["cls"].split() if c.startswith("g-"))
        sizes.setdefault(grp, set()).add(round(t["size"], 1))
    for grp, s in sizes.items():
        assert len(s) == 1, f"{grp}: mixed label sizes {s}"


CHIPS = """() => {
  const find = (n) => { for (const el of n.querySelectorAll('*')) { if (el.tagName === 'HH-INSPECTOR') return el.shadowRoot; if (el.shadowRoot) { const x = find(el.shadowRoot); if (x) return x; } } return null; };
  return [...find(document.querySelector('hh-app').shadowRoot).querySelectorAll('.free.chips button')].map((b) => ({ text: b.textContent.trim(), cut: b.scrollWidth - b.clientWidth, h: b.getBoundingClientRect().height }));
}"""


async def test_free_key_chips_are_readable_names_that_are_not_cut(browser, site):
    page = await open_remote(browser, site)
    chips = await page.evaluate(CHIPS)
    names = {c["text"] for c in chips}
    assert {"Previous channel", "Upper bulb", "Lower socket", "Volume down", "Fast forward", "DVR"} <= names
    for c in chips:
        assert "_" not in c["text"], c
        assert c["cut"] <= 0, f"{c['text']} is cut"
        assert c["h"] < 40, f"{c['text']} wraps"
