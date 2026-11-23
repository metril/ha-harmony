"""No personal paths, names or home-network addresses in tracked files."""

import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
# built from parts so this file does not match itself
FORBIDDEN = re.compile(
    "|".join(("jaga" + "nnath", "OLYM" + "POS", r"10\.10\.7[67]\.")), re.IGNORECASE
)


def test_no_personal_strings_in_tracked_files():
    files = subprocess.run(
        ["git", "ls-files", "-z"], cwd=ROOT, capture_output=True, check=True
    ).stdout.split(b"\0")
    hits = []
    for name in filter(None, files):
        path = ROOT / name.decode()
        if not path.is_file() or path.is_symlink():
            continue
        data = path.read_bytes()
        if b"\0" in data[:4096]:
            continue  # binary
        for n, line in enumerate(data.decode("utf-8", "replace").splitlines(), 1):
            if FORBIDDEN.search(line):
                hits.append(f"{name.decode()}:{n}")
    assert not hits, "personal strings in tracked files: " + ", ".join(hits[:20])
