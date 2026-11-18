"""Entry point: python -m harmony_addon [--data-dir /data] [--port 8099]."""

from __future__ import annotations

import argparse
import logging
from pathlib import Path

from aiohttp import web

from .app import create_app
from .auth import load_or_create_token
from .ha_client import default_ha_api
from .ir_tester import default_ir_sender
from .options import load_options
from .supervisor import Discovery, default_supervisor

SHUTDOWN_TIMEOUT = 3.0  # waiters are woken at shutdown; never sit out a ~55 s long poll


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--data-dir", default="/data")
    ap.add_argument("--port", type=int, default=8099)
    ap.add_argument("--static-dir", default="/opt/app/frontend")
    a = ap.parse_args()
    opts = load_options(Path(a.data_dir) / "options.json")
    logging.basicConfig(level=opts.log_level.upper(), format="%(levelname)s %(name)s: %(message)s")
    token = load_or_create_token(a.data_dir)
    web.run_app(
        create_app(
            a.data_dir,
            options=opts,
            bundle_token=token,
            discovery=Discovery(default_supervisor(), a.port, token),
            ha_api=default_ha_api(),
            ir_sender=default_ir_sender(),
            static_dir=a.static_dir,
        ),
        host="0.0.0.0",
        port=a.port,
        print=None,
        shutdown_timeout=SHUTDOWN_TIMEOUT,
        access_log=logging.getLogger("access"),
    )


if __name__ == "__main__":
    main()
