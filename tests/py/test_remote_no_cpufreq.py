"""The remote app never changes the CPU frequency: a frequency change can cut an I2C transfer on the shared bus and leave
the remote dead until the battery is pulled (the stock firmware never writes cpufreq either)."""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FORBIDDEN = re.compile(r"cpufreq|scaling_(governor|setspeed|m(ax|in)_freq)|/sys/devices/system/cpu|cpu0/", re.IGNORECASE)


def test_remote_sources_never_touch_cpufreq():
    hits = []
    for sub in ("remote/app", "remote/page", "remote/sim"):
        for path in sorted((ROOT / sub).rglob("*")):
            if path.suffix in (".c", ".h") and "fonts" not in path.parts:
                for n, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
                    if FORBIDDEN.search(line):
                        hits.append(f"{path.relative_to(ROOT)}:{n}")
    assert not hits, "cpufreq access in the remote sources: " + ", ".join(hits)
