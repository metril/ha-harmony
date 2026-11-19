"""Read-only view of Home Assistant for the editor's pickers.

The add-on has `homeassistant_api: true`, so its SUPERVISOR_TOKEN can call the Core API through the
Supervisor proxy. This module only ever issues GET requests (`/api/states`, `/api/services`); it
never calls a service. Without SUPERVISOR_TOKEN (plain docker) there is no client and the routes
answer 503, so the UI falls back to typed entity ids.
"""

from __future__ import annotations

import asyncio
import hashlib
import json
import logging
import os
from collections.abc import Callable
from typing import Any, Protocol

import aiohttp

from .ha_ws import HaRegistryApi, RegistryUnavailable

log = logging.getLogger(__name__)

CORE_API_URL = "http://supervisor/core/api"
MAX_BODY = 8 * 1024 * 1024  # a large install's /api/states is a few MB
MAX_ENTITIES = 20000  # ~130 B a row compact: a 20,000-entity install is ~2.6 MB, past that the answer says `truncated`
MAX_STR = 200
CACHE_SECONDS = 5.0
TIMEOUT_SECONDS = 20  # a few-MB /states on a busy install
REGISTRY_SECONDS = 60.0
FAILED_RETRY_SECONDS = 15.0

ACTION_DOMAINS = frozenset(
    {
        "light", "switch", "scene", "script", "media_player", "remote", "fan", "cover",
        "climate", "input_boolean", "input_button", "button", "number", "select",
    }
)  # fmt: skip
ENTITY_DOMAINS = ACTION_DOMAINS | {"sensor", "binary_sensor"}


class HaUnavailable(Exception):
    """Home Assistant cannot be read right now (no token, network error, bad answer).

    `reason` is a short machine-readable cause (`status_503`, `too_large`, `not_json`,
    `unexpected_shape`, an exception type name); it never holds response contents.
    """

    def __init__(self, reason: str) -> None:
        super().__init__(reason)
        self.reason = reason


class HaApi(Protocol):
    async def get(self, path: str) -> Any: ...


class HttpHaApi:
    """GET-only client for the Core API. There is deliberately no method that sends a body."""

    def __init__(self, token: str, url: str = CORE_API_URL) -> None:
        self._token, self._url = token, url

    def __repr__(self) -> str:  # the token must never reach a log line
        return f"HttpHaApi(url={self._url!r})"

    async def get(self, path: str) -> Any:
        timeout = aiohttp.ClientTimeout(total=TIMEOUT_SECONDS)
        headers = {"Authorization": f"Bearer {self._token}"}
        try:
            async with (
                aiohttp.ClientSession(timeout=timeout, headers=headers) as s,
                s.get(f"{self._url}{path}", allow_redirects=False) as r,
            ):
                if r.status != 200:
                    raise HaUnavailable(f"status_{r.status}")
                raw = bytearray()
                async for chunk in r.content.iter_chunked(65536):
                    raw += chunk
                    if len(raw) > MAX_BODY:
                        raise HaUnavailable("too_large")
                try:
                    return json.loads(raw)
                except ValueError:
                    raise HaUnavailable("not_json") from None
        except (aiohttp.ClientError, OSError, TimeoutError) as exc:
            raise HaUnavailable(type(exc).__name__) from None


def default_ha_api() -> HaApi | None:
    token = os.environ.get("SUPERVISOR_TOKEN")
    if not token:
        log.info("SUPERVISOR_TOKEN is not set: Home Assistant entity pickers are off")
        return None
    return HttpHaApi(token)


def _text(v: Any) -> str | None:
    return v[:MAX_STR] if isinstance(v, str) and v else None


def _ids(v: Any) -> list[str]:
    """A registry field that should be a list of ids: anything else is an empty list."""
    return [x[:MAX_STR] for x in v if isinstance(x, str) and x][:16] if isinstance(v, list) else []


def _dict_rows(v: Any) -> list[dict[str, Any]]:
    return [r for r in v if isinstance(r, dict)] if isinstance(v, list) else []


MEMO_MAX = 64


def _haystack(row: dict[str, Any], reg: dict[str, Any] | None) -> str:
    """The lower-cased text a `q` word must be found in: names (never ids of areas and devices)."""
    parts = [row["name"], row["entity_id"], row.get("platform", "")]
    if reg is not None:
        if a := row.get("area_id"):
            parts.append(reg["areas"][a][0])
        if d := row.get("device_id"):
            parts.append(reg["devices"][d][0])
        parts += [reg["labels"][lid] for lid in row.get("label_ids", [])]
    return " ".join(parts).casefold()


class Catalog:
    """Entities (with area/device from the registries when readable) and services, filtered to the allow-listed domains and cached for a few seconds."""

    def __init__(
        self,
        api: HaApi | None,
        clock: Callable[[], float],
        registry: HaRegistryApi | None = None,
    ) -> None:
        self._api, self._clock, self._registry = api, clock, registry
        self._reg_cache: tuple[float, dict[str, Any] | None] | None = None
        self._cache: dict[str, tuple[float, Any]] = {}
        self._failing: dict[str, str] = {}  # path -> last logged reason
        self._inflight: dict[str, asyncio.Future[Any]] = {}
        self._memo_src: tuple[Any, Any] | None = None
        self._memo: dict[tuple[Any, ...], tuple[str, str]] = {}

    def _failed(self, path: str, reason: str) -> None:
        """One WARNING on the first failure and on a change of cause, never one per poll."""
        if self._failing.get(path) != reason:
            self._failing[path] = reason
            what = "registries" if path == "registry" else path
            log.warning("Home Assistant %s cannot be read: %s", what, reason)

    async def _once(self, key: str, make: Callable[[], Any]) -> Any:
        """Concurrent callers of one resource share a single in-flight fetch."""
        fut = self._inflight.get(key)
        if fut is None:
            fut = asyncio.ensure_future(make())
            self._inflight[key] = fut

            def done(f: asyncio.Future[Any]) -> None:
                self._inflight.pop(key, None)
                if not f.cancelled():
                    f.exception()  # retrieved: the callers that wait see it, nobody else logs it

            fut.add_done_callback(done)
        return await asyncio.shield(fut)

    async def _get(self, path: str) -> Any:
        try:
            data = await self._fetch(path)
        except HaUnavailable as exc:
            self._failed(path, exc.reason)
            raise
        if self._failing.pop(path, None) is not None:
            log.info("Home Assistant %s can be read again", path)
        return data

    async def _fetch(self, path: str) -> Any:
        if self._api is None:
            raise HaUnavailable("no_access")
        hit = self._cache.get(path)
        if hit and self._clock() - hit[0] < CACHE_SECONDS:
            return hit[1]
        return await self._once(path, lambda: self._load(path))

    async def _load(self, path: str) -> Any:
        assert self._api is not None
        now = self._clock()
        data = await self._api.get(path)
        if not isinstance(data, list):
            raise HaUnavailable("unexpected_shape")
        self._cache[path] = (now, data)
        return data

    async def _registries(self) -> dict[str, Any] | None:
        """Area, device, entity, floor and label registries merged into lookups, cached ~60 s.

        None when they cannot be read (no WebSocket access, refused, too slow): the catalog then
        carries no areas, devices or flags. A failed refresh keeps serving the last good copy and is
        not retried for a few seconds, so a dead socket does not cost a timeout per request.
        """
        if self._registry is None:
            return None
        hit = self._reg_cache
        if hit and self._clock() - hit[0] < (REGISTRY_SECONDS if hit[1] is not None else FAILED_RETRY_SECONDS):
            return hit[1]
        return await self._once("registry", self._load_registries)

    async def _load_registries(self) -> dict[str, Any] | None:
        assert self._registry is not None
        now = self._clock()
        old = self._reg_cache[1] if self._reg_cache else None
        try:
            raw = await self._registry.lists()
        except RegistryUnavailable as exc:
            self._failed("registry", exc.reason)
            self._reg_cache = (now, old)
            return old
        if self._failing.pop("registry", None) is not None:
            log.info("Home Assistant registries can be read again")
        floors = {
            fid: _text(f.get("name"))
            for f in _dict_rows(raw.get("config/floor_registry/list"))
            if (fid := _text(f.get("floor_id")))
        }
        areas = {}
        for a in _dict_rows(raw.get("config/area_registry/list")):
            if aid := _text(a.get("area_id")):
                fid = _text(a.get("floor_id"))
                areas[aid] = (_text(a.get("name")) or aid, fid if fid in floors else None, floors.get(fid))
        labels = {
            lid: _text(lb.get("name")) or lid
            for lb in _dict_rows(raw.get("config/label_registry/list"))
            if (lid := _text(lb.get("label_id")))
        }
        devices = {}
        for d in _dict_rows(raw.get("config/device_registry/list")):
            if did := _text(d.get("id")):
                devices[did] = (
                    _text(d.get("name_by_user")) or _text(d.get("name")),
                    _text(d.get("area_id")),
                    _ids(d.get("labels")),
                )
        entities = {
            eid: e for e in _dict_rows(raw.get("config/entity_registry/list")) if (eid := _text(e.get("entity_id")))
        }
        data = {"areas": areas, "devices": devices, "entities": entities, "labels": labels}
        self._reg_cache = (now, data)
        return data

    async def _sources(self) -> tuple[list[Any], dict[str, Any] | None]:
        states = await self._get("/states")
        return states, await self._registries()

    async def catalog(
        self,
        domain: str | None = None,
        q: str | None = None,
        area: str | None = None,
        diagnostic: bool = False,
        state: bool = False,
    ) -> dict[str, Any]:
        """Entities in a compact shape, with the areas, devices and labels they refer to by id.

        Disabled and hidden entities are left out; so are diagnostic and config ones unless
        `diagnostic`. `registries` says whether area/device/integration/label data could be read.
        Above MAX_ENTITIES the first ones (domain, name order) are returned with `truncated`.
        """
        states, reg = await self._sources()
        return self._build(states, reg, domain, q, area, diagnostic, state)

    async def catalog_body(
        self,
        domain: str | None = None,
        q: str | None = None,
        area: str | None = None,
        diagnostic: bool = False,
        state: bool = False,
    ) -> tuple[str, str]:
        """The serialised catalog and its ETag, memoised per query until the sources change."""
        states, reg = await self._sources()
        if self._memo_src is None or self._memo_src[0] is not states or self._memo_src[1] is not reg:
            self._memo_src, self._memo = (states, reg), {}
        key = (domain, q, area, diagnostic, state)
        hit = self._memo.get(key)
        if hit is None:
            text = json.dumps(
                self._build(states, reg, domain, q, area, diagnostic, state), separators=(",", ":")
            )
            hit = (text, '"' + hashlib.sha256(text.encode()).hexdigest()[:16] + '"')
            if len(self._memo) >= MEMO_MAX:
                self._memo.clear()
            self._memo[key] = hit
        return hit

    @staticmethod
    def _build(
        states: list[Any],
        reg: dict[str, Any] | None,
        domain: str | None,
        q: str | None,
        area: str | None,
        diagnostic: bool,
        state: bool,
    ) -> dict[str, Any]:
        words = (q or "").casefold().split()
        out: list[dict[str, Any]] = []
        for st in states:
            if not isinstance(st, dict):
                continue
            eid = st.get("entity_id")
            if not isinstance(eid, str) or "." not in eid or len(eid) > MAX_STR:
                continue
            dom = eid.split(".", 1)[0]
            if dom not in ENTITY_DOMAINS or (domain and dom != domain):
                continue
            attrs = st.get("attributes") if isinstance(st.get("attributes"), dict) else {}
            row: dict[str, Any] = {
                "entity_id": eid,
                "name": _text(attrs.get("friendly_name")) or eid,
                "domain": dom,
            }
            if state and (val := _text(st.get("state"))):
                row["state"] = val
            if val := _text(attrs.get("device_class")):
                row["device_class"] = val
            if reg is not None and (ent := reg["entities"].get(eid)):
                if ent.get("disabled_by") or ent.get("hidden_by"):
                    continue
                if ent.get("entity_category") in ("diagnostic", "config"):
                    if not diagnostic:
                        continue
                    row["diag"] = True
                dev_id = _text(ent.get("device_id"))
                _, dev_area, dev_labels = reg["devices"].get(dev_id, (None, None, []))
                area_id = _text(ent.get("area_id")) or dev_area
                if area_id in reg["areas"]:
                    row["area_id"] = area_id
                if dev_id in reg["devices"] and reg["devices"][dev_id][0]:
                    row["device_id"] = dev_id
                if plat := _text(ent.get("platform")):
                    row["platform"] = plat
                lids = sorted(
                    {i for i in [*_ids(ent.get("labels")), *dev_labels] if i in reg["labels"]}
                )
                if lids:
                    row["label_ids"] = lids
            if area and area != row.get("area_id"):
                continue
            if words:
                hay = _haystack(row, reg)
                if not all(w in hay for w in words):
                    continue
            out.append(row)
        out.sort(key=lambda r: (r["domain"], r["name"].casefold()))
        total = len(out)
        out = out[:MAX_ENTITIES]
        areas, devices, labels = {}, {}, {}
        if reg is not None:
            for r in out:
                if (a := r.get("area_id")) and a not in areas:
                    name, fid, floor = reg["areas"][a]
                    areas[a] = {"id": a, "name": name, **({"floor_id": fid, "floor": floor} if fid else {})}
                if (d := r.get("device_id")) and d not in devices:
                    devices[d] = {"id": d, "name": reg["devices"][d][0]}
                for lid in r.get("label_ids", []):
                    labels.setdefault(lid, {"id": lid, "name": reg["labels"][lid]})
        body: dict[str, Any] = {
            "entities": out,
            "areas": sorted(areas.values(), key=lambda a: a["name"].casefold()),
            "devices": sorted(devices.values(), key=lambda d: d["name"].casefold()),
            "labels": sorted(labels.values(), key=lambda lb: lb["name"].casefold()),
            "registries": reg is not None,
            "total": total,
        }
        if total > len(out):
            body["truncated"] = True
        return body

    async def entities(self, domain: str | None, q: str | None) -> list[dict[str, Any]]:
        return (await self.catalog(domain, q))["entities"]

    async def services(self, domain: str | None) -> list[dict[str, Any]]:
        data = await self._get("/services")
        out: list[dict[str, Any]] = []
        for entry in data:
            if not isinstance(entry, dict):
                continue
            dom = entry.get("domain")
            if dom not in ACTION_DOMAINS or (domain and dom != domain):
                continue
            services = entry.get("services")
            if not isinstance(services, dict):
                continue
            for name, spec in sorted(services.items()):
                if not isinstance(name, str) or len(name) > MAX_STR:
                    continue
                spec = spec if isinstance(spec, dict) else {}
                fields = spec.get("fields") if isinstance(spec.get("fields"), dict) else {}
                out.append(
                    {
                        "domain": dom,
                        "service": name,
                        "name": _text(spec.get("name")) or name,
                        "fields": sorted(k for k in fields if isinstance(k, str))[:64],
                    }
                )
        return out
