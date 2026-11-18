"""IR database routes (ingress): brand/model search, converted device blocks, power candidates,
and an importer for pasted Pronto / Flipper / SmartIR text. No network here: the archive client
(`harmony_irdb`) does the fetching with its fixed host list, size caps and sanitisers."""

from __future__ import annotations

import asyncio
import json
import logging
from collections.abc import Awaitable
from datetime import UTC, datetime
from typing import Any

import voluptuous as vol
from aiohttp import web

from harmony_core.ir.flipper import parse_flipper
from harmony_core.ir.model import IrImportError
from harmony_core.ir.pronto import normalise_pronto
from harmony_core.ir.smartir import parse_smartir
from harmony_irdb.archive import (
    ArchiveError,
    ArchiveLimitError,
    IrdbArchive,
    OfflineError,
    safe_segment,
    search_models,
)
from harmony_irdb.candidates import power_candidates
from harmony_irdb.device import DEVICE_TYPES, build_device, device_type_id

from .web_util import CLOCK_KEY, error, jresp, query, read_body

log = logging.getLogger(__name__)

IRDB_KEY = web.AppKey("irdb", IrdbArchive)

MAX_BRANDS = 50
MAX_MODELS_PAGE = 200
MAX_CANDIDATES = 100
MAX_PARSED = 256
MAX_SKIPPED = 500
MAX_NOTES = 200
MAX_PARSE_TEXT = 256 * 1024
ARCHIVE_TIMEOUT = 45.0

_TEXT = vol.All(str, vol.Length(max=200))
_Q = vol.All(str, vol.Length(max=64))


def _int(lo: int, hi: int, default: int):
    return vol.All(str, vol.Match(r"^\d{1,9}$"), vol.Coerce(int), vol.Range(lo, hi))


BRANDS_QUERY = vol.Schema({vol.Optional("q", default=""): _Q}, extra=vol.PREVENT_EXTRA)
MODELS_QUERY = vol.Schema(
    {
        vol.Required("brand"): vol.All(str, vol.Length(min=1, max=200)),
        vol.Optional("q", default=""): _Q,
        vol.Optional("offset", default="0"): _int(0, 10_000_000, 0),
        vol.Optional("limit", default="50"): _int(1, MAX_MODELS_PAGE, 50),
    },
    extra=vol.PREVENT_EXTRA,
)
DEVICE_QUERY = vol.Schema(
    {
        vol.Required("brand"): vol.All(str, vol.Length(min=1, max=200)),
        vol.Required("model"): vol.All(str, vol.Length(min=1, max=200)),
    },
    extra=vol.PREVENT_EXTRA,
)
CANDIDATES_QUERY = vol.Schema(
    {
        vol.Required("brand"): vol.All(str, vol.Length(min=1, max=200)),
        vol.Required("type"): vol.All(str, vol.Length(min=1, max=32)),
        vol.Optional("limit", default="60"): _int(1, 200, 60),
    },
    extra=vol.PREVENT_EXTRA,
)
PARSE_BODY = vol.Schema(
    {
        vol.Required("kind"): vol.In(["pronto", "flipper", "smartir"]),
        vol.Required("text"): vol.All(str, vol.Length(min=1, max=MAX_PARSE_TEXT)),
    },
    extra=vol.PREVENT_EXTRA,
)


async def _archive(coro: Awaitable[Any]) -> Any:
    """Run an archive call; map its failures to stable JSON errors (never echo remote text)."""
    try:
        async with asyncio.timeout(ARCHIVE_TIMEOUT):
            return await coro
    except OfflineError:
        raise error(
            504, "archive_offline", "the IR archive is unreachable and not cached"
        ) from None
    except TimeoutError:
        raise error(504, "archive_timeout", "the IR archive took too long") from None
    except ArchiveLimitError as exc:
        log.warning("IR archive entry over a limit: %s", str(exc)[:200])
        raise error(
            502, "archive_too_large", "that archive entry is too large to process"
        ) from None
    except ArchiveError as exc:
        log.warning("IR archive error: %s", str(exc)[:200])
        raise error(502, "archive_error", "the IR archive answered something unusable") from None


def _segment(value: str, what: str) -> str:
    try:
        return safe_segment(value)
    except ArchiveError:
        raise error(400, "bad_request", f"invalid request: {what}") from None


async def _brand(request: web.Request, slug: str):
    seg = _segment(slug, "brand")
    for b in await request.app[IRDB_KEY].brands():
        if b.slug == seg:
            return b
    raise error(404, "unknown_brand", "no such brand in the IR archive")


async def brands(request: web.Request) -> web.Response:
    q = query(request, BRANDS_QUERY)
    found = await request.app[IRDB_KEY].search_brands(q["q"], MAX_BRANDS)
    return jresp(
        {"brands": [{"name": b.name, "slug": b.slug, "count": b.count} for b in found[:MAX_BRANDS]]}
    )


async def models(request: web.Request) -> web.Response:
    q = query(request, MODELS_QUERY)
    brand = await _brand(request, q["brand"])
    allm = await _archive(request.app[IRDB_KEY].models(brand.slug))
    if q["q"]:
        hits = await asyncio.to_thread(search_models, allm, q["q"], len(allm))
    else:
        hits = allm
    page = hits[q["offset"] : q["offset"] + q["limit"]]
    return jresp(
        {
            "brand": {"name": brand.name, "slug": brand.slug},
            "total": len(hits),
            "offset": q["offset"],
            "limit": q["limit"],
            "models": [{"file": m.file, "name": m.name[:120], "id": m.id} for m in page],
        }
    )


async def device(request: web.Request) -> web.Response:
    q = query(request, DEVICE_QUERY)
    brand = await _brand(request, q["brand"])
    model = _segment(q["model"], "model")
    if not model.endswith(".json") or model == "index.json":
        raise error(400, "bad_request", "invalid request: model")
    arc = request.app[IRDB_KEY]
    dev = await _archive(arc.device(brand.slug, model))
    path = dev.get("codeset")
    if not isinstance(path, str):
        raise error(502, "archive_error", "the archive entry has no codeset")
    codeset = await _archive(arc.codeset(path))
    clock = request.app[CLOCK_KEY]
    built = await asyncio.to_thread(  # CPU: off the event loop
        build_device,
        dev,
        codeset,
        mfr=brand.name,
        model_file=model,
        codeset_path=path,
        imported_at=datetime.fromtimestamp(int(clock()), UTC).strftime("%Y-%m-%dT%H:%M:%SZ"),
    )
    dtype = dev.get("deviceType")
    return jresp(
        {
            "device": built.device,
            "forms": built.forms,
            "notes": [n[:200] for n in built.notes[:MAX_NOTES]],
            "skipped": [
                {"name": str(n)[:80], "reason": str(r)[:120]}
                for n, r in built.skipped[:MAX_SKIPPED]
            ],
            "skipped_total": len(built.skipped),
            "device_type": {"id": dtype, "name": DEVICE_TYPES.get(dtype)}
            if isinstance(dtype, int) and not isinstance(dtype, bool)
            else None,
            "source": built.device["source"],
        }
    )


async def candidates(request: web.Request) -> web.Response:
    q = query(request, CANDIDATES_QUERY)
    brand = await _brand(request, q["brand"])
    if device_type_id(q["type"]) is None:
        raise error(400, "bad_request", "invalid request: type")
    res = await _archive(
        power_candidates(request.app[IRDB_KEY], brand.slug, q["type"], limit=q["limit"])
    )
    return jresp(
        {
            "candidates": [
                {
                    "kind": c.kind,
                    "action": c.action,
                    "form": c.form,
                    "count": c.count,
                    "models": [m[:120] for m in c.models[:5]],
                    "codesets": c.codesets,
                }
                for c in res.candidates[:MAX_CANDIDATES]
            ],
            "examined": res.examined,
            "matched": res.matched,
            "errors": len(res.errors),
            "truncated": res.truncated,
        }
    )


# ------------------------------------------------------------------ pasted IR text
def _bad(msg: str) -> web.HTTPException:
    return error(422, "bad_ir", msg[:200], errors=[{"path": [], "msg": msg[:200]}])


def parse_ir_text(kind: str, text: str) -> tuple[list[dict[str, Any]], list[dict[str, str]]]:
    """-> (actions, skipped). Raises IrImportError for text that is not that format at all."""
    codes: dict[str, Any]
    if kind == "pronto":
        from harmony_core.ir.model import IrCode

        codes = {"pronto": IrCode(pronto=normalise_pronto(text))}
    elif kind == "flipper":
        codes = parse_flipper(text)
    else:
        try:
            data = json.loads(text)
        except ValueError, RecursionError:
            raise IrImportError("SmartIR text is not valid JSON") from None
        codes = parse_smartir(data)
    actions: list[dict[str, Any]] = []
    skipped: list[dict[str, str]] = []
    for name, code in codes.items():
        if len(actions) >= MAX_PARSED:
            skipped.append({"name": str(name)[:80], "reason": f"over {MAX_PARSED} actions"})
            continue
        try:
            action = code.to_hub_json()
        except IrImportError as exc:
            skipped.append({"name": str(name)[:80], "reason": str(exc)[:120]})
            continue
        actions.append(
            {
                "name": str(name)[:80],
                "action": action,
                "form": "proto" if "proto" in action else "pronto" if "pronto" in action else "raw",
            }
        )
    return actions, skipped[:MAX_SKIPPED]


async def parse(request: web.Request) -> web.Response:
    body = await read_body(request, PARSE_BODY)
    try:
        actions, skipped = await asyncio.to_thread(parse_ir_text, body["kind"], body["text"])
    except IrImportError as exc:
        raise _bad(str(exc)) from None
    except ValueError, RecursionError, KeyError, TypeError:
        raise _bad(f"not a valid {body['kind']} file") from None
    if not actions:
        raise _bad("no usable IR signals found")
    return jresp({"ok": True, "actions": actions, "skipped": skipped})


def add_routes(app: web.Application, api: str) -> None:
    app.router.add_get(f"{api}/ir/brands", brands)
    app.router.add_get(f"{api}/ir/models", models)
    app.router.add_get(f"{api}/ir/device", device)
    app.router.add_get(f"{api}/ir/candidates", candidates)
    app.router.add_post(f"{api}/ir/parse", parse)
