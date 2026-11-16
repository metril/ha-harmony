"""IrCode: one IR action in config-format-2 terms."""

from __future__ import annotations

from dataclasses import dataclass

PROTOCOLS = ("nec", "necext", "samsung32", "rc5", "rc6", "sirc12", "sirc15", "sirc20")

# Max (addr, cmd) per protocol.
_LIMITS = {
    "nec": (0xFF, 0xFF),
    "necext": (0xFFFF, 0xFF),
    "samsung32": (0xFF, 0xFF),
    "rc5": (0x1F, 0x7F),
    "rc6": (0xFF, 0xFF),
    "sirc12": (0x1F, 0x7F),
    "sirc15": (0xFF, 0x7F),
    "sirc20": (0x1FFF, 0x7F),
}

# Default cap on raw mark/space entries accepted by the hub (hub constant defined elsewhere).
DEFAULT_MAX_RAW_US = 512


class IrImportError(ValueError):
    """Invalid, malformed or too-large IR data."""


class UnsupportedProtocolError(IrImportError):
    """A parsed signal uses a protocol the hub cannot transmit."""


@dataclass(frozen=True)
class IrCode:
    proto: str | None = None
    addr: int | None = None
    cmd: int | None = None
    hz: int | None = None
    us: tuple[int, ...] | None = None
    pronto: str | None = None

    def __post_init__(self) -> None:
        if self.us is not None:
            object.__setattr__(self, "us", tuple(self.us))
        is_proto = self.proto is not None or self.addr is not None or self.cmd is not None
        is_raw = self.hz is not None or self.us is not None
        if int(is_proto) + int(is_raw) + int(self.pronto is not None) != 1:
            raise IrImportError("IrCode needs exactly one of proto/addr/cmd, hz/us, pronto")
        if is_proto:
            self._check_proto()
        elif is_raw:
            self._check_raw()
        else:
            from .pronto import normalise_pronto

            object.__setattr__(self, "pronto", normalise_pronto(self.pronto))

    def _check_proto(self) -> None:
        if self.proto not in _LIMITS:
            raise IrImportError(f"unknown protocol {self.proto!r}")
        if not isinstance(self.addr, int) or not isinstance(self.cmd, int):
            raise IrImportError("protocol code needs integer addr and cmd")
        amax, cmax = _LIMITS[self.proto]
        if not 0 <= self.addr <= amax:
            raise IrImportError(f"{self.proto} addr {self.addr} outside 0..{amax}")
        if not 0 <= self.cmd <= cmax:
            raise IrImportError(f"{self.proto} cmd {self.cmd} outside 0..{cmax}")

    def _check_raw(self) -> None:
        if self.hz is None or self.us is None:
            raise IrImportError("raw code needs both hz and us")
        if not 1 <= self.hz <= 1_000_000:
            raise IrImportError(f"carrier {self.hz} Hz out of range")
        if not self.us:
            raise IrImportError("raw code has no timings")
        if any(not isinstance(v, int) or v <= 0 for v in self.us):
            raise IrImportError("raw timings must be positive integers (microseconds)")

    def raw_length(self) -> int:
        """Mark/space entries the hub must store (0 for protocol codes)."""
        if self.us is not None:
            return len(self.us)
        if self.pronto is not None:
            return len(self.pronto.split()) - 4
        return 0

    def check_size(self, max_raw_us: int = DEFAULT_MAX_RAW_US) -> None:
        n = self.raw_length()
        if n > max_raw_us:
            raise IrImportError(f"raw sequence too long for hub: {n} entries > {max_raw_us}")

    def to_hub_json(self, max_raw_us: int = DEFAULT_MAX_RAW_US) -> dict:
        self.check_size(max_raw_us)
        if self.proto is not None:
            return {"proto": self.proto, "addr": self.addr, "cmd": self.cmd}
        if self.us is not None:
            return {"raw": {"hz": self.hz, "us": list(self.us)}}
        return {"pronto": self.pronto}
