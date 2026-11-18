"""Serve the built editor (addon/frontend/dist) on 127.0.0.1 with a fake Home Assistant, for local
browsing and browser scripts. No device, no real Home Assistant. Ctrl+C stops it.
    uv run python tools/editor_dev_server.py [--config file.json|file.yaml] [--port 8765] [--entities 300]
`--config` is applied as the draft (and as the applied config with --applied). Prints the URL.
"""

import argparse
import asyncio
import json
import tempfile
from pathlib import Path

from aiohttp import web

from harmony_addon.app import create_app
from harmony_addon.auth import NoAuth
from harmony_addon.options import Options
from harmony_core.config_model import load_source

ROOT = Path(__file__).resolve().parents[1]
DOMAINS = [
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


class FakeHaApi:
    def __init__(self, n: int) -> None:
        self.states = [
            {
                "entity_id": f"{DOMAINS[i % len(DOMAINS)]}.room_{i // len(DOMAINS)}_{DOMAINS[i % len(DOMAINS)]}",
                "state": "on" if i % 3 else "off",
                "attributes": {
                    "friendly_name": f"Room {i // len(DOMAINS)} {DOMAINS[i % len(DOMAINS)]}"
                },
            }
            for i in range(n)
        ]
        self.services = [
            {
                "domain": d,
                "services": {
                    "toggle": {"name": "Toggle", "fields": {}},
                    "turn_on": {"name": "Turn on", "fields": {"brightness_pct": {}}},
                    "turn_off": {},
                },
            }
            for d in (
                "light",
                "switch",
                "fan",
                "cover",
                "media_player",
                "scene",
                "script",
                "climate",
            )
        ]

    async def get(self, path: str):
        return {"/states": self.states, "/services": self.services}[path]


async def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--config")
    ap.add_argument(
        "--applied", action="store_true", help="apply the config instead of leaving it as a draft"
    )
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--entities", type=int, default=300)
    a = ap.parse_args()
    data = Path(tempfile.mkdtemp(prefix="hh-dev-"))
    app = create_app(
        data,
        options=Options("info"),
        auth=NoAuth(),
        static_dir=ROOT / "addon/frontend/dist",
        bundle_token="ab" * 32,
        bundle_networks=None,
        wait_seconds=1.0,
        ha_api=FakeHaApi(a.entities),
    )
    runner = web.AppRunner(app)
    await runner.setup()
    await web.TCPSite(runner, "127.0.0.1", a.port).start()
    if a.config:
        import aiohttp

        text = Path(a.config).read_text()
        cfg = json.loads(text) if a.config.endswith(".json") else load_source(text)
        h = {"X-HH-CSRF": "1", "X-Remote-User-Id": "dev"}
        async with aiohttp.ClientSession(headers=h) as s:
            base = f"http://127.0.0.1:{a.port}/api/v1/config"
            async with s.put(f"{base}/draft", json={"config": cfg}) as r:
                assert r.status == 200, await r.text()
            if a.applied:
                async with s.post(f"{base}/apply", json={}) as r:
                    assert r.status == 200, await r.text()
    print(f"http://127.0.0.1:{a.port}/", flush=True)
    await asyncio.Event().wait()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
