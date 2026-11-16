"""Python mirror of hub/rf/framing.c (RF fragmentation and reassembly)."""

HDR_LEN = 4
MAX_FRAGS = 15
FRAME_MAX = 32


def split(msg_type: int, seq: int, payload: bytes, frame_cap: int) -> list[bytes]:
    """Split into frames of at most frame_cap bytes. Raises ValueError, never truncates."""
    if not HDR_LEN < frame_cap <= FRAME_MAX:
        raise ValueError(f"invalid frame_cap {frame_cap}")
    if not 0 <= msg_type <= 0xFF or not 0 <= seq <= 0xFF:
        raise ValueError("type/seq out of range")
    per = frame_cap - HDR_LEN
    n = max(1, -(-len(payload) // per))
    if n > MAX_FRAGS:
        raise ValueError(f"payload of {len(payload)} bytes needs {n} fragments (max {MAX_FRAGS})")
    frames = []
    for i in range(n):
        chunk = payload[i * per : (i + 1) * per]
        frames.append(bytes([msg_type, seq, (i << 4) | n, len(chunk)]) + chunk)
    return frames


class Reassembler:
    def __init__(self) -> None:
        self._key: tuple[int, int] | None = None
        self._total = 0
        self._parts: dict[int, bytes] = {}

    def push(self, frame: bytes) -> tuple[int, bytes] | None:
        """Return (type, payload) when complete, None if more needed; ValueError if malformed."""
        if len(frame) < HDR_LEN:
            raise ValueError("frame shorter than header")
        mtype, seq, frag, flen = frame[0], frame[1], frame[2], frame[3]
        index, total = frag >> 4, frag & 0x0F
        if total == 0 or index >= total:
            raise ValueError("bad fragment index/total")
        if flen > FRAME_MAX - HDR_LEN or flen > len(frame) - HDR_LEN:
            raise ValueError("fragment len exceeds frame")
        key = (mtype, seq)
        if key == self._key and total != self._total:
            raise ValueError("mismatched fragment total")
        if key != self._key:
            self._key, self._total, self._parts = key, total, {}
        self._parts.setdefault(index, bytes(frame[HDR_LEN : HDR_LEN + flen]))
        if len(self._parts) < total:
            return None
        payload = b"".join(self._parts[i] for i in range(total))
        self._key, self._total, self._parts = None, 0, {}
        return mtype, payload
