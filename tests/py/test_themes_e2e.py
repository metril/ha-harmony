# ruff: noqa: F811
"""Themes and widget colours in the editor, in a real browser (same skips as test_addon_e2e.py):
palette colour -> preview + YAML, custom colour contrast, duplicate-a-preset flow through the apply dialog, the
compiler's over-budget message, axe on the new views. HH_SHOTS=<dir> also writes the screenshots."""

import json
import os
from pathlib import Path

import pytest
from test_addon_e2e import DIST, PREFIX, H, api, browser, link, open_editor, site  # noqa: F401

from harmony_core.config_model import load_source

pytestmark = pytest.mark.skipif(not (DIST / "index.html").is_file(), reason="frontend not built")
SHOTS = os.environ.get("HH_SHOTS")
AXE = DIST.parent / "node_modules" / "axe-core" / "axe.min.js"
BASE = """
devices:
  - id: tv
    name: Living room TV
    power: {on: {proto: nec, addr: 4, cmd: 2}, off: {proto: nec, addr: 4, cmd: 3}}
    commands: {menu: {proto: nec, addr: 4, cmd: 20}, ok: {proto: nec, addr: 4, cmd: 34}}
activities:
  - id: watch_tv
    name: Watch TV
    devices: [{device: tv}]
pages:
  - id: home
    kind: home
    title: Living room
    cols: 3
    rows: 5
    widgets:
      - {type: label, x: 0, y: 0, w: 3, h: 1, label: Evening}
      - {type: button, id: a_tv, x: 0, y: 1, label: Watch TV, icon: tv, role: primary, action: {activity: watch_tv}}
      - {type: button, id: menu, x: 1, y: 1, label: Menu, action: {device: tv, command: menu}}
      - {type: button, id: ok, x: 2, y: 1, label: OK, action: {device: tv, command: ok}}
      - {type: tile, id: lamp, x: 0, y: 2, label: Lamp, entity: light.lamp}
      - {type: tile, id: temp, x: 1, y: 2, label: Temp, entity: sensor.temp}
"""


async def put(origin, cfg):
    status, _ = await api(origin, "PUT", "/api/v1/config/draft", json={"config": cfg}, headers=H)
    assert status == 200


async def studio(browser, origin, cfg=None, width=1440, scheme="light", **kw):
    await put(origin, cfg or load_source(BASE))
    page = await open_editor(
        browser, origin, color_scheme=scheme, viewport={"width": width, "height": 900}, **kw
    )
    await page.locator("hh-remote [data-widget]").first.wait_for()
    return page


async def draft_yaml(origin):
    import aiohttp

    async with (
        aiohttp.ClientSession() as s,
        s.get(
            f"{origin}/api/v1/config/export_yaml",
            params={"hub": "default", "source": "draft"},
            headers={"X-Remote-User-Id": "t"},
        ) as r,
    ):
        return await r.text()


async def settle(page):
    await page.wait_for_timeout(900)  # autosave + preview


async def shot(page, name):
    if not SHOTS:
        return
    Path(SHOTS).mkdir(parents=True, exist_ok=True)
    await page.screenshot(path=f"{SHOTS}/{name}.png")


SCREEN_FILL = (
    "() => document.querySelector('hh-app').shadowRoot.querySelector('hh-studio').shadowRoot"
    ".querySelector('hh-remote').shadowRoot.querySelectorAll('svg.scr rect')[%d].getAttribute('fill')"
)


async def test_palette_colour_updates_the_preview_and_the_yaml(browser, site):
    page = await studio(browser, site)
    await page.locator("hh-remote [data-widget]").nth(1).click()  # Watch TV
    insp = page.locator("hh-inspector")
    await insp.get_by_role("button", name="Red", exact=True).click()
    await page.wait_for_function(
        "() => [...document.querySelector('hh-app').shadowRoot.querySelector('hh-studio').shadowRoot"
        ".querySelector('hh-remote').shadowRoot.querySelectorAll('svg.scr rect')].some(r => r.getAttribute('fill') === '#f85848')"
    )
    await settle(page)
    assert "color: red" in await draft_yaml(site)
    await insp.get_by_role("button", name="Default (role)").first.click()
    await settle(page)
    assert "color:" not in await draft_yaml(site)
    await page.context.close()


async def test_custom_colour_low_contrast_warns_then_fixes(browser, site):
    page = await studio(browser, site)
    await page.locator("hh-remote [data-widget]").nth(2).click()
    insp = page.locator("hh-inspector")
    await insp.get_by_role("button", name="Custom…").first.click()
    await insp.get_by_label("Hex colour").fill("#777777")
    await insp.get_by_text("Low contrast").wait_for()
    await shot(page, "colour-low-contrast")
    await settle(page)
    assert "#777777" in await draft_yaml(site)
    await insp.get_by_role("button", name="Use nearest readable").click()
    await insp.get_by_text("Low contrast").wait_for(state="detached")
    await insp.get_by_text("label contrast").wait_for()
    await page.context.close()


async def test_duplicate_a_preset_edit_the_accent_and_see_it_in_the_apply_dialog(browser, site):
    page = await studio(browser, site)
    await page.goto(f"{site}{PREFIX}/#/themes")
    await page.get_by_role("article", name="Cards").wait_for()
    await shot(page, "themes-gallery")
    await page.get_by_role("button", name="Duplicate Cards").click()
    await page.get_by_label("Accent hex", exact=True).fill("#FF8A3D")
    await page.get_by_label("Accent hex", exact=True).press("Tab")
    await shot(page, "theme-editor")
    await page.get_by_role("button", name="Use this theme").click()
    await settle(page)
    y = await draft_yaml(site)
    assert "theme: my_default" in y and "ACC" not in y and "acc: '#FF8A3D'" in y.replace('"', "'")
    # the mini remote and thumbnails use the new accent
    fills = await page.evaluate(
        "() => [...document.querySelector('hh-app').shadowRoot.querySelector('hh-themes').shadowRoot"
        ".querySelectorAll('svg text')].map(t => t.getAttribute('fill'))"
    )
    assert "#f88c40" in fills  # the thumbnails draw the new accent (snapped to 5-6-5)
    await page.get_by_role("button", name="Review & Apply").click()
    dlg = page.locator("hh-apply-dialog")
    await dlg.get_by_text("My Default").first.wait_for()
    assert await dlg.get_by_text("Theme", exact=False).count() >= 1
    await shot(page, "apply-theme")
    await page.context.close()


async def test_over_budget_page_says_so_in_the_inspector(browser, site):
    cfg = load_source(BASE)
    labels = [f"Label number {i:02d} padding." for i in range(8)]  # 24 bytes each
    cfg["pages"] = [
        {
            "id": "home",
            "kind": "home",
            "cols": 4,
            "rows": 4,
            "widgets": [
                {
                    "type": "button",
                    "id": f"b{i}",
                    "x": i % 4,
                    "y": i // 4,
                    "label": labels[i],
                    "action": {"none": True},
                }
                for i in range(8)
            ],
        }
    ]
    page = await studio(browser, site, cfg)
    for i in range(3):
        cfg["pages"][0]["widgets"][i]["color"] = ["#3355FF", "#44AA66", "#CC8822"][i]
    await put(site, cfg)
    await page.reload()
    await page.locator("hh-remote [data-widget]").first.wait_for()
    await page.locator("hh-remote [data-widget]").first.click()
    await page.get_by_text("1 byte over").first.wait_for()
    await shot(page, "colour-over-budget")
    await page.context.close()


@pytest.mark.skipif(not AXE.is_file(), reason="axe-core is not installed")
@pytest.mark.parametrize("scheme", ["light", "dark"])
async def test_axe_on_the_theme_views_and_the_colour_row(browser, site, scheme):
    bad = []
    for width in (1440, 400):
        page = await studio(browser, site, width=width, scheme=scheme, bypass_csp=True)
        await page.locator("hh-remote [data-widget]").nth(1).click()
        if width < 860:
            await page.get_by_role("button", name="Inspector", exact=True).click()
        await page.locator("hh-inspector").get_by_role("button", name="Custom…").first.click()
        await page.locator("hh-inspector").get_by_label("Hex colour").fill("#3355FF")
        await (
            page.locator("hh-inspector")
            .get_by_role("group", name="Custom colour")
            .scroll_into_view_if_needed()
        )
        await page.wait_for_timeout(300)
        await shot(page, f"inspector-colour-{scheme}-{width}")
        views = [("studio", None), ("gallery", "themes")]
        for name, route in views:
            if route:
                await page.goto(f"{site}{PREFIX}/#/{route}")
                await page.get_by_role("article", name="Midnight").wait_for()
                await shot(page, f"themes-gallery-{scheme}-{width}")
            await page.add_script_tag(path=str(AXE))
            found = await page.evaluate(
                "axe.run(document, {resultTypes:['violations']}).then(r => r.violations.map(v => v.id + ' ' + v.nodes[0].target.join(' ')))"
            )
            bad += [f"{name}@{width}: {v}" for v in found]
        await page.get_by_role("button", name="Duplicate Midnight").click()
        await page.get_by_label("Accent hex", exact=True).wait_for()
        await page.get_by_label("Main Text hex", exact=True).fill(
            "#404848"
        )  # a contrast problem shows too
        await page.get_by_label("Main Text hex", exact=True).press("Tab")
        await page.get_by_text("contrast problem").first.wait_for()
        await page.evaluate("() => window.scrollTo(0, 0)")
        await page.wait_for_timeout(300)
        await shot(page, f"theme-editor-{scheme}-{width}")
        await page.mouse.wheel(0, 700)
        await page.wait_for_timeout(300)
        await shot(page, f"theme-editor-scrolled-{scheme}-{width}")
        await page.add_script_tag(path=str(AXE))
        found = await page.evaluate(
            "axe.run(document, {resultTypes:['violations']}).then(r => r.violations.map(v => v.id + ' ' + v.nodes[0].target.join(' ')))"
        )
        bad += [f"editor@{width}: {v}" for v in found]
        await page.context.close()
    assert bad == [], json.dumps(bad)


async def test_a_palette_only_remote_shows_its_notice_in_the_themes_view(browser, site):

    page = await studio(browser, site)
    await page.goto(f"{site}{PREFIX}/#/themes")
    await page.get_by_role("article", name="Cards").wait_for()
    assert await page.locator("#theme-notice").count() == 0  # nothing reported: no banner
    report = [
        {
            "remote": 1,
            "caps": 7,
            "tier": "palette",
            "notices": [
                "theme 'dusk' replaced by its base preset 'midnight': the remote firmware has no custom colour support",
                "<b id='injected'>x</b><img src=x onerror='window.__pwn=1'>",
            ],
        }
    ]
    await link(site, remotes=report)  # the integration's hello, as the real one sends it
    await page.reload()
    await page.get_by_role("article", name="Cards").wait_for()
    notice = page.locator("#theme-notice")
    await notice.wait_for()
    assert "palette colours only" in await notice.inner_text()
    await notice.locator("summary").click()
    assert "<b id='injected'>" in await notice.inner_text()  # shown as text, never as markup
    assert await page.locator("#injected").count() == 0
    assert await page.evaluate("() => window.__pwn === undefined")
    await shot(page, "theme-notice-palette-remote")
    await page.context.close()
