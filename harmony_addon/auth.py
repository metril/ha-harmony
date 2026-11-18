"""Pluggable request admission. Runs before routing, so unknown paths are refused too.

Two disjoint rules: ingress routes are ingress-only (Supervisor gateway peer); the bundle routes
(`/api/v1/bundle*`, used by the integration) are bearer-only and never reachable via ingress.
"""

from __future__ import annotations

import hashlib
import hmac
import ipaddress
import os
import re
import secrets
import time
from collections import deque
from collections.abc import Callable, Collection
from pathlib import Path
from typing import Protocol

from aiohttp import web

# Supervisor's ingress gateway (documented in the HA app security/presentation guides).
INGRESS_GATEWAY = "172.30.32.2"
INGRESS_HEADER = "X-Ingress-Path"
HASSIO_NETWORK = "172.30.32.0/23"
BUNDLE_PREFIX = "/api/v1/bundle"
TOKEN_FILE = "bundle_token"


class Auth(Protocol):
    def check(self, request: web.Request) -> None:
        """Raise web.HTTPForbidden / HTTPUnauthorized / HTTPTooManyRequests to refuse."""


def _is_api(request: web.Request) -> bool:
    return request.path.startswith("/api/")


def _peer_ip(request: web.Request) -> str | None:
    peer = request.transport.get_extra_info("peername") if request.transport else None
    return peer[0] if peer else None


class IngressAuth:
    """Only the Supervisor ingress gateway may talk to us; UI routes also need X-Ingress-Path."""

    def __init__(self, peers: Collection[str] = (INGRESS_GATEWAY,)) -> None:
        self.peers = frozenset(peers)

    def check(self, request: web.Request) -> None:
        peer = _peer_ip(request)
        if not peer or peer not in self.peers:
            raise web.HTTPForbidden(reason="forbidden")
        if not _is_api(request) and not request.headers.get(INGRESS_HEADER):
            raise web.HTTPForbidden(reason="forbidden")


class NoAuth:
    """Test only."""

    def check(self, request: web.Request) -> None:
        return None


def load_or_create_token(data_dir: Path | str) -> str:
    """The bundle bearer token: 32 random bytes (hex), mode 0600, under the data dir."""
    path = Path(data_dir) / TOKEN_FILE
    try:
        tok = path.read_text(encoding="ascii").strip()
        if re.fullmatch(r"[0-9a-f]{64}", tok):
            path.chmod(0o600)
            return tok
    except OSError, ValueError:
        pass
    tok = secrets.token_hex(32)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(f".{path.name}.tmp")
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w", encoding="ascii") as f:
        f.write(tok)
    os.replace(tmp, path)
    return tok


class BundleAuth:
    """Bearer token + hassio-network peer, and never via ingress. One opaque 401 for every miss;
    repeated misses from one peer are answered 429 for a while. Only failures count: the token is
    checked first, so a valid caller is never throttled."""

    MAX_FAILS = 10
    WINDOW = 60.0

    def __init__(
        self,
        token: str,
        networks: Collection[str] | None = (HASSIO_NETWORK,),
        clock: Callable[[], float] = time.monotonic,
    ) -> None:
        self._digest = hashlib.sha256(token.encode()).digest()
        self._nets = None if networks is None else [ipaddress.ip_network(n) for n in networks]
        self._clock = clock
        self._fails: dict[str, deque[float]] = {}

    def _in_network(self, ip: str) -> bool:
        if self._nets is None:
            return True
        try:
            addr = ipaddress.ip_address(ip)
        except ValueError:
            return False
        return any(addr in n for n in self._nets)

    def _limited(self, ip: str) -> bool:
        q = self._fails.get(ip)
        if q is None:
            return False
        now = self._clock()
        while q and now - q[0] > self.WINDOW:
            q.popleft()
        if not q:
            del self._fails[ip]
            return False
        return len(q) >= self.MAX_FAILS

    def check(self, request: web.Request) -> None:
        ip = _peer_ip(request) or "?"
        scheme, _, given = request.headers.get("Authorization", "").partition(" ")
        ok = scheme == "Bearer" and hmac.compare_digest(
            hashlib.sha256(given.encode()).digest(), self._digest
        )
        if ok and self._in_network(ip) and INGRESS_HEADER not in request.headers:
            return  # a valid token is never throttled (Core and add-ons share one source IP)
        if self._limited(ip):
            raise web.HTTPTooManyRequests(reason="rate limited", headers={"Retry-After": "60"})
        self._fails.setdefault(ip, deque()).append(self._clock())
        raise web.HTTPUnauthorized(reason="unauthorized")


class RouteAuth:
    """Bundle routes are bearer-only; every other route keeps the ingress-only rule."""

    def __init__(self, ingress: Auth, bundle: BundleAuth) -> None:
        self.ingress, self.bundle = ingress, bundle

    def check(self, request: web.Request) -> None:
        path = request.path
        if path == BUNDLE_PREFIX or path.startswith(BUNDLE_PREFIX + "/"):
            self.bundle.check(request)
        else:
            self.ingress.check(request)
