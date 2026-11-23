"""TCP keepalive for liveness without application ping timers (60 s idle, 10 s interval, 3 probes)."""

from __future__ import annotations

import socket
from typing import Any

KEEPIDLE = 60
KEEPINTVL = 10
KEEPCNT = 3
USER_TIMEOUT_MS = 90000  # unacked data aborts the connection after this long
USER_TIMEOUT_MS = 90000  # unacked data aborts the connection after this long


def set_keepalive(sock: socket.socket | None) -> bool:
    """Enable SO_KEEPALIVE with the values above. False when sock is None/unusable."""
    if sock is None:
        return False
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
    except OSError:
        return False
    for name, value in (
        ("TCP_KEEPIDLE", KEEPIDLE),
        ("TCP_KEEPINTVL", KEEPINTVL),
        ("TCP_KEEPCNT", KEEPCNT),
    ):
        opt = getattr(socket, name, None)  # macOS spells idle TCP_KEEPALIVE; Windows lacks some
        if opt is None and name == "TCP_KEEPIDLE":
            opt = getattr(socket, "TCP_KEEPALIVE", None)
        if opt is None:
            continue
        try:
            sock.setsockopt(socket.IPPROTO_TCP, opt, value)
        except OSError:
            pass
    opt = getattr(socket, "TCP_USER_TIMEOUT", None)  # Linux only
    if opt is not None:
        try:
            sock.setsockopt(socket.IPPROTO_TCP, opt, USER_TIMEOUT_MS)
        except OSError:
            pass
    return True


def keepalive_ws(ws: Any) -> bool:
    """Set keepalive on the socket under an aiohttp client WebSocket. aiohttp has no public accessor, so
    this reads the response's connection transport and never raises."""
    try:
        sock = ws._response.connection.transport.get_extra_info("socket")
    except AttributeError:
        return False
    return set_keepalive(sock)
