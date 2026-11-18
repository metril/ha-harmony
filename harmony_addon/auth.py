"""Pluggable request admission. Runs before routing, so unknown paths are refused too."""

from __future__ import annotations

from collections.abc import Collection
from typing import Protocol

from aiohttp import web

# Supervisor's ingress gateway (documented in the HA app security/presentation guides).
INGRESS_GATEWAY = "172.30.32.2"
INGRESS_HEADER = "X-Ingress-Path"


class Auth(Protocol):
    def check(self, request: web.Request) -> None:
        """Raise web.HTTPForbidden to refuse the request."""


def _is_api(request: web.Request) -> bool:
    return request.path.startswith("/api/")


class IngressAuth:
    """Only the Supervisor ingress gateway may talk to us; UI routes also need X-Ingress-Path."""

    def __init__(self, peers: Collection[str] = (INGRESS_GATEWAY,)) -> None:
        self.peers = frozenset(peers)

    def check(self, request: web.Request) -> None:
        peer = request.transport.get_extra_info("peername") if request.transport else None
        if not peer or peer[0] not in self.peers:
            raise web.HTTPForbidden(reason="forbidden")
        if not _is_api(request) and not request.headers.get(INGRESS_HEADER):
            raise web.HTTPForbidden(reason="forbidden")


class NoAuth:
    """Test only."""

    def check(self, request: web.Request) -> None:
        return None
