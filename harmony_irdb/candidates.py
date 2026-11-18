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


async def power_candidates(
    archive: IrdbArchive,
    slug: str,
    device_type: object,
    *,
    models: list | None = None,
    limit: int = 60,
    concurrency: int = 8,
    examples: int = 5,
) -> CandidateResult:
    """`limit` caps how many device files are fetched (brands can have 20k models)."""
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
    paths = sorted({d["codeset"] for _, d in matched if isinstance(d.get("codeset"), str)})

    async def load_cs(p):
        async with sem:
            try:
                return p, await archive.codeset(p)
            except OfflineError:
                raise
            except ArchiveError as exc:
                errors.append(f"{p}: {exc}")
                return p, None

    codesets = dict(await asyncio.gather(*(load_cs(p) for p in paths)))
    groups: dict[tuple[str, str], Candidate] = {}
    seen_cs: dict[tuple[str, str], set[str]] = {}
    for m, d in matched:
        cs = codesets.get(d.get("codeset"))
        power = d.get("power")
        if not cs or not isinstance(power, dict):
            continue
        by_name = {c.get("name"): c for c in cs["commands"] if isinstance(c, dict)}
        for kind in _KIND_ORDER:
            lst = power.get(kind)
            first = (
                next((x for x in lst if isinstance(x, str)), None)
                if isinstance(lst, list)
                else None
            )
            res = convert_command(by_name.get(first)) if first in by_name else None
            if not isinstance(res, Converted):
                continue
            key = (kind, json.dumps(res.action, sort_keys=True))
            c = groups.setdefault(key, Candidate(kind, res.action, res.form, 0))
            c.count += 1
            if len(c.models) < examples:
                c.models.append(m.name)
            seen_cs.setdefault(key, set()).add(d["codeset"])
    for key, c in groups.items():
        c.codesets = len(seen_cs[key])
    ranked = sorted(groups.items(), key=lambda kv: (-kv[1].count, _KIND_ORDER[kv[0][0]], kv[0][1]))
    return CandidateResult([c for _, c in ranked], len(loaded), len(matched), sorted(errors))
