"""TCP keepalive for liveness without application ping timers."""

import socket

KEEPIDLE = 15
KEEPINTVL = 5
KEEPCNT = 3
USER_TIMEOUT_MS = 20000  # unacked data aborts the connection after this long


def set_keepalive(sock: socket.socket | None) -> bool:
    """Enable SO_KEEPALIVE (15 s idle, 5 s interval, 3 probes). False when sock is None/unusable."""
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
