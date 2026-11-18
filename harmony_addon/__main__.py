"""Entry point: python -m harmony_addon [--data-dir /data] [--port 8099]."""

from __future__ import annotations

import argparse
import logging
from pathlib import Path

from aiohttp import web

from .app import create_app
from .options import load_options


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--data-dir", default="/data")
    ap.add_argument("--port", type=int, default=8099)
    a = ap.parse_args()
    opts = load_options(Path(a.data_dir) / "options.json")
    logging.basicConfig(level=opts.log_level.upper(), format="%(levelname)s %(name)s: %(message)s")
    web.run_app(
        create_app(a.data_dir, options=opts),
        host="0.0.0.0",
        port=a.port,
        print=None,
        access_log=logging.getLogger("access"),
    )


if __name__ == "__main__":
    main()
