"""Python mirror of hub/rf/framing.c: RF object framing in hub mode (30-byte frames).

Raw-frame helpers for tests that play a remote by hand: frames are sent without waiting for acks;
the C link (hub/rf/link.c) owns stop-and-wait. Vectors: tests/vectors/rf/frames.json.
"""

CHUNK = 25
BODY_MAX = 254
INNER_LEN = 28
HUB_FRAME_LEN = 30
KIND_OBJECT, KIND_ACK, KIND_CONT = 0x20, 0x40, 0x60
FIRST = 0x10


def split(msg_type: int, seq: int, body: bytes, slot: int = 1) -> list[bytes]:
    """One object as hub-mode frames (0x12, slot, inner[28]). Raises ValueError, never truncates."""
    if not 0 <= msg_type <= 0xFF or not 0 <= slot <= 0xFF:
        raise ValueError("type/slot out of range")
    if len(body) > BODY_MAX:
        raise ValueError(f"body of {len(body)} bytes exceeds {BODY_MAX}")
    obj = bytes([len(body)]) + body
    frames = []
    for i in range(0, len(obj), CHUNK):
        hdr = (KIND_OBJECT | FIRST if i == 0 else KIND_CONT) | (seq & 0x0F)
        inner = bytes([hdr, msg_type, i // CHUNK]) + obj[i : i + CHUNK].ljust(CHUNK, b"\0")
        frames.append(bytes([0x12, slot]) + inner)
    return frames


class Reassembler:
    """Reassembles hub-mode frames; acks and non-data frames yield None."""

    def __init__(self) -> None:
        self._key: tuple[int, int] | None = None
        self._need = 0
        self._next = 0
        self._buf = b""
        self._last: int | None = None

    def push(self, frame: bytes) -> tuple[int, bytes] | None:
        """(type, body) when an object completes, None otherwise; ValueError if malformed."""
        if len(frame) != HUB_FRAME_LEN or frame[0] != 0x12:
            raise ValueError("not a hub-mode data frame")
        hdr, mtype, idx, data = frame[2], frame[3], frame[4], frame[5:]
        kind, seq = hdr & 0xE0, hdr & 0x0F
        if kind == KIND_ACK:
            return None
        if kind == KIND_OBJECT and hdr & FIRST and data[0] <= BODY_MAX:
            self._key, self._need, self._next, self._buf = (mtype, seq), data[0] + 1, 1, b""
        elif (
            kind == KIND_CONT
            and not hdr & FIRST
            and self._key == (mtype, seq)
            and idx == self._next
        ):
            self._next += 1
        else:
            self._key = None
            raise ValueError("frame does not belong to the current object")
        self._buf += data[: self._need - len(self._buf)]
        if len(self._buf) < self._need:
            return None
        body = self._buf[1:]
        self._key = None
        if self._last == seq:
            return None  # a retried object whose ack was lost: re-acked, not redelivered
        self._last = seq
        return mtype, body
