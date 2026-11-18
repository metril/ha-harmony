"""Which power codes should I try for brand X + device type Y?

Models are grouped by codeset (many models share one); the distinct power codes found are ranked by
how many models use them.
"""

from __future__ import annotations

import asyncio
import json
from dataclasses import dataclass, field

from .archive import ArchiveError, IrdbArchive, OfflineError
from .convert import Converted, convert_command
from .device import device_type_id

_KIND_ORDER = {"on": 0, "off": 1, "toggle": 2}


@dataclass
class Candidate:
    kind: str  # on | off | toggle
    action: dict
    form: str
    count: int  # models using this code
    models: list[str] = field(default_factory=list)  # a few example model names
    codesets: int = 0


@dataclass
class CandidateResult:
    candidates: list[Candidate]
    examined: int  # device files looked at
    matched: int  # of the requested type
    errors: list[str] = field(default_factory=list)
    truncated: bool = False  # the byte budget ran out: later codesets were not looked at


BUDGET_BYTES = 16 * 1024 * 1024  # codeset text processed per request


def _cost(cs: dict) -> int:
    n = 0
    for c in cs["commands"]:
        if isinstance(c, dict):
            for k in ("name", "pronto", "prontoRepeat"):
                v = c.get(k)
                if isinstance(v, str):
                    n += len(v)
    return n


def scan_codeset(cs: dict, entries: list[tuple[str, dict]]) -> list[tuple[str, str, dict, str, str]]:
    """CPU part, run in a worker thread: (kind, key, action, form, model name) for each power code
    the given (model name, device power block) pairs use. `by_name` is built once per codeset."""
    by_name = {c.get("name"): c for c in cs["commands"] if isinstance(c, dict)}
    out = []
    for name, power in entries:
        for kind in _KIND_ORDER:
            lst = power.get(kind)
            first = (
                next((x for x in lst if isinstance(x, str)), None)
                if isinstance(lst, list)
                else None
            )
            res = convert_command(by_name.get(first)) if first in by_name else None
            if isinstance(res, Converted):
                out.append((kind, json.dumps(res.action, sort_keys=True), res.action, res.form, name))
    return out


async def power_candidates(
    archive: IrdbArchive,
    slug: str,
    device_type: object,
    *,
    models: list | None = None,
    limit: int = 60,
    concurrency: int = 8,
    examples: int = 5,
    budget: int | None = None,
) -> CandidateResult:
    """`limit` caps how many device files are fetched (brands can have 20k models). Codesets are
    loaded a few at a time and dropped after scanning, so memory stays bounded, and `budget` caps
    the codeset text processed (the result is then `truncated`)."""
    budget = BUDGET_BYTES if budget is None else budget
    want = device_type_id(device_type)
    if want is None:
        raise ArchiveError(f"unknown device type {device_type!r}")
    pool = (models if models is not None else await archive.models(slug))[:limit]
    sem = asyncio.Semaphore(concurrency)
    errors: list[str] = []

    async def load(m):
        async with sem:
            try:
                return m, await archive.device(slug, m.file)
            except OfflineError:
                raise
            except ArchiveError as exc:
                errors.append(f"{m.file}: {exc}")
                return m, None

    loaded = await asyncio.gather(*(load(m) for m in pool))
    matched = [(m, d) for m, d in loaded if d and d.get("deviceType") == want]
    by_cs: dict[str, list[tuple[str, dict]]] = {}
    for m, d in matched:
        if isinstance(d.get("codeset"), str) and isinstance(d.get("power"), dict):
            by_cs.setdefault(d["codeset"], []).append((m.name, d["power"]))
    paths = sorted(by_cs)

    async def load_cs(p):
        async with sem:
            try:
                return p, await archive.codeset(p)
            except OfflineError:
                raise
            except ArchiveError as exc:
                errors.append(f"{p}: {exc}")
                return p, None

    groups: dict[tuple[str, str], Candidate] = {}
    seen_cs: dict[tuple[str, str], set[str]] = {}
    spent, truncated = 0, False
    for i in range(0, len(paths), concurrency):
        if spent >= budget:
            truncated = True
            break
        for p, cs in await asyncio.gather(*(load_cs(p) for p in paths[i : i + concurrency])):
            if not cs:
                continue
            spent += await asyncio.to_thread(_cost, cs)
            if spent > budget:
                truncated = True
                break
            for kind, key, action, form, mname in await asyncio.to_thread(
                scan_codeset, cs, by_cs[p]
            ):
                k = (kind, key)
                c = groups.setdefault(k, Candidate(kind, action, form, 0))
                c.count += 1
                if len(c.models) < examples:
                    c.models.append(mname)
                seen_cs.setdefault(k, set()).add(p)
        if truncated:
            break
    for key, c in groups.items():
        c.codesets = len(seen_cs[key])
    ranked = sorted(groups.items(), key=lambda kv: (-kv[1].count, _KIND_ORDER[kv[0][0]], kv[0][1]))
    return CandidateResult(
        [c for _, c in ranked], len(loaded), len(matched), sorted(errors), truncated
    )
