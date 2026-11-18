"""Add-on options (/data/options.json written by the Supervisor)."""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path

LOG_LEVELS = ("debug", "info", "warning", "error")


@dataclass(frozen=True)
class Options:
    log_level: str = "info"


def load_options(path: Path) -> Options:
    try:
        raw = json.loads(Path(path).read_text(encoding="utf-8"))
    except OSError, ValueError:
        return Options()
    level = raw.get("log_level") if isinstance(raw, dict) else None
    return Options(log_level=level if level in LOG_LEVELS else "info")
