"""Add-on options (/data/options.json written by the Supervisor)."""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path

LOG_LEVELS = ("debug", "info", "warning", "error")


@dataclass(frozen=True)
class Options:
    hub_host: str = ""
    hub_port: int = 8099
    hub_token: str = field(default="", repr=False)  # a secret: never log or return it
    log_level: str = "info"

    @property
    def hub_configured(self) -> bool:
        return bool(self.hub_host and self.hub_token)

    def public(self) -> dict[str, object]:
        return {
            "hub_host": self.hub_host,
            "hub_port": self.hub_port,
            "hub_token_set": bool(self.hub_token),
        }


def load_options(path: Path) -> Options:
    try:
        raw = json.loads(Path(path).read_text(encoding="utf-8"))
    except OSError, ValueError:
        return Options()
    if not isinstance(raw, dict):
        return Options()
    level = raw.get("log_level")
    port = raw.get("hub_port")
    return Options(
        hub_host=str(raw.get("hub_host") or "")[:253],
        hub_port=port if isinstance(port, int) and 1 <= port <= 65535 else 8099,
        hub_token=str(raw.get("hub_token") or ""),
        log_level=level if level in LOG_LEVELS else "info",
    )
