"""Headless-browser smoke test: the real aiohttp app + the real built bundle, under an ingress prefix.

Skipped unless playwright is importable, a Chromium can be launched (HH_CHROMIUM=/path/to/chrome
overrides the lookup) and addon/frontend/dist exists (`npm run build`). CI does not install
playwright, so this runs on developer machines:
    uv run --with playwright pytest tests/py/test_addon_e2e.py
"""

import os
from pathlib import Path

import pytest

from harmony_addon.app import create_app
from harmony_addon.auth import NoAuth
from harmony_addon.options import Options

pw = pytest.importorskip("playwright.async_api")
DIST = Path(__file__).resolve().parents[2] / "addon" / "frontend" / "dist"
pytestmark = pytest.mark.skipif(not (DIST / "index.html").is_file(), reason="frontend not built")

PREFIX = "/api/hassio_ingress/e2eTok_en-1"
YAML = """
devices:
  - id: tv
    name: TV
    power: {on: {proto: nec, addr: 4, cmd: 2}, off: {proto: nec, addr: 4, cmd: 3}}
    commands: {menu: {proto: nec, addr: 4, cmd: 20}}
"""


async def test_editor_loads_imports_and_applies_under_an_ingress_prefix(aiohttp_server, tmp_path):
    server = await aiohttp_server(
        create_app(tmp_path, options=Options("info"), auth=NoAuth(), static_dir=DIST)
    )
    origin = f"http://127.0.0.1:{server.port}"
    async with pw.async_playwright() as p:
        try:
            browser = await p.chromium.launch(executable_path=os.environ.get("HH_CHROMIUM"))
        except pw.Error as exc:  # no browser binary on this machine
            pytest.skip(f"cannot launch chromium: {exc}")
        page = await browser.new_page()
        problems = []
        page.on("console", lambda m: problems.append(m.text) if m.type == "error" else None)
        page.on("pageerror", lambda e: problems.append(str(e)))

        async def to_real_server(route):  # the ingress proxy: strip the prefix
            url = route.request.url.replace(PREFIX, "", 1)
            await route.continue_(url=url)

        await page.route(f"{origin}{PREFIX}/**", to_real_server)
        await page.goto(f"{origin}{PREFIX}/")
        await page.get_by_text("Set up your first hub config").wait_for()

        await page.get_by_role("button", name="Import YAML", exact=True).click()
        await page.locator("textarea").fill(YAML)
        await page.get_by_role("button", name="Import as draft").click()
        await page.get_by_text("1 change not applied").wait_for()
        await page.get_by_role("button", name="Review & apply").click()
        await page.get_by_role("button", name="Apply to hub").click()
        await page.get_by_text("Saved as rev 1").wait_for()
        # 404s for favicon / the not-yet-existing api/v1/hubs are fine; CSP violations are not
        assert [p for p in problems if "Failed to load resource" not in p] == []
        await browser.close()
