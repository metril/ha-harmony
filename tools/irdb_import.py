#!/usr/bin/env -S uv run
"""Search the Logitech Harmony IR archive and import a device as a config block.

    tools/irdb_import.py brands sony
    tools/irdb_import.py models Sony "bdp s590"
    tools/irdb_import.py candidates Sony --type "DVD Player" --limit 40
    tools/irdb_import.py import Sony BDP-S590-231542.json --out sony_bd.yaml

`import` writes a `devices:` YAML fragment (docs/ha-config.md format, with a `source:` key that the
compiler ignores) to --out or stdout, and a conversion report to stderr. Files are cached under
--cache-dir (default: $XDG_CACHE_HOME/harmony-irdb); --offline uses the cache only.
"""

from __future__ import annotations

import argparse
import asyncio
import datetime
import json
import os
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from harmony_irdb.archive import (
    ArchiveError,
    Brand,
    IrdbArchive,
    Model,
    fold,
    search_models,
)
from harmony_irdb.candidates import power_candidates
from harmony_irdb.device import build_device, to_yaml


def default_cache() -> Path:
    base = os.environ.get("XDG_CACHE_HOME") or str(Path.home() / ".cache")
    return Path(base) / "harmony-irdb"


async def find_brand(arc: IrdbArchive, text: str) -> Brand:
    hits = await arc.search_brands(text, 10)
    exact = [b for b in hits if fold(b.name) == fold(text)]
    if exact:
        return exact[0]
    if len(hits) == 1:
        return hits[0]
    names = ", ".join(b.name for b in hits) or "none"
    raise ArchiveError(f"brand {text!r} is not unique; matches: {names}")


async def find_model(arc: IrdbArchive, brand: Brand, text: str) -> Model:
    models = await arc.models(brand.slug)
    for m in models:
        if m.file == text or m.file == text + ".json":
            return m
    hits = search_models(models, text, 10)
    exact = [m for m in hits if fold(m.name) == fold(text)]
    if len(exact) == 1 or len(hits) == 1:
        return (exact or hits)[0]
    names = ", ".join(f"{m.name} [{m.file}]" for m in hits) or "none"
    raise ArchiveError(f"model {text!r} is not unique; matches: {names}")


async def run(args: argparse.Namespace) -> int:
    import aiohttp

    async with aiohttp.ClientSession() as session:
        arc = IrdbArchive(session, args.cache_dir, offline=args.offline)
        if args.cmd == "brands":
            for b in await arc.search_brands(args.query, args.limit):
                print(f"{b.name}\t{b.count} devices")
            return 0
        brand = await find_brand(arc, args.brand)
        if args.cmd == "models":
            for m in search_models(await arc.models(brand.slug), args.query, args.limit):
                print(f"{m.name}\t{m.file}")
            return 0
        if args.cmd == "candidates":
            res = await power_candidates(arc, brand.slug, args.type, limit=args.limit)
            print(
                f"# {res.matched} of {res.examined} examined models are of this type",
                file=sys.stderr,
            )
            for c in res.candidates:
                print(
                    f"{c.count:4d} models  {c.kind:6s} {c.form:6s} {json.dumps(c.action)[:100]}  e.g. {', '.join(c.models[:3])}"
                )
            for e in res.errors:
                print(f"# error: {e}", file=sys.stderr)
            return 0
        model = await find_model(arc, brand, args.model)
        dev = await arc.device(brand.slug, model.file)
        cs_path = dev.get("codeset")
        cs = await arc.codeset(cs_path)
        stamp = datetime.datetime.now(datetime.UTC).strftime("%Y-%m-%dT%H:%M:%SZ")
        built = build_device(
            dev, cs, mfr=brand.name, model_file=model.file, codeset_path=cs_path,
            imported_at=stamp, dev_id=args.id, name=args.name,
        )  # fmt: skip
    text = to_yaml([built.device])
    try:
        from custom_components.ha_harmony.config_model import parse_model

        parse_model(text, "<import>")
    except ImportError:
        print("# note: config_model not importable here, output not validated", file=sys.stderr)
    if args.out:
        Path(args.out).write_text(text, encoding="utf-8")
    else:
        sys.stdout.write(text)
    forms = Counter(built.forms.values())
    print(
        f"# {len(built.forms)} commands imported ({forms['proto']} proto, {forms['pronto']} pronto),"
        f" {len(built.skipped)} skipped",
        file=sys.stderr,
    )
    for name, why in built.skipped:
        print(f"#   skipped {name}: {why}", file=sys.stderr)
    for note in built.notes:
        print(f"#   {note}", file=sys.stderr)
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--cache-dir", type=Path, default=default_cache())
    ap.add_argument("--offline", action="store_true", help="use the cache only")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("brands")
    p.add_argument("query", nargs="?", default="")
    p.add_argument("--limit", type=int, default=30)
    p = sub.add_parser("models")
    p.add_argument("brand")
    p.add_argument("query", nargs="?", default="")
    p.add_argument("--limit", type=int, default=30)
    p = sub.add_parser("candidates")
    p.add_argument("brand")
    p.add_argument("--type", required=True, help='archive deviceType number or name, e.g. "TV"')
    p.add_argument("--limit", type=int, default=60, help="max model files to fetch")
    p = sub.add_parser("import")
    p.add_argument("brand")
    p.add_argument("model", help="model file name or (unique) model name")
    p.add_argument("--id")
    p.add_argument("--name")
    p.add_argument("--out")
    args = ap.parse_args(argv)
    try:
        return asyncio.run(run(args))
    except ArchiveError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
