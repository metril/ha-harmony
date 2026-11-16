"""device_backup against a fake SSH transport (no network, no sleeps)."""

import hashlib
import json
import re
import shlex
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import device_backup as db

ERASE = 0x1000
PARTS = {  # index -> (name, bytes)
    0: ("boot", b"B" * 8192),
    1: ("kernel loader", bytes(range(256)) * 32),
    2: ('we"ird name', b"\x00\xff" * 2048),
}


def mtd_text(parts=PARTS) -> str:
    lines = ["dev:    size   erasesize  name"]
    for i, (n, d) in parts.items():
        lines.append(f'mtd{i}: {len(d):08x} {ERASE:08x} "{n}"')
    return "\n".join(lines) + "\n"


class FakeSsh:
    def __init__(self, parts=PARTS, utils=("dd", "sha256sum", "md5sum"), mtd=True,
                 corrupt_hash=(), truncate=(), proc_partitions="", char=True):  # fmt: skip
        self.parts, self.utils, self.mtd = parts, utils, mtd
        self.corrupt_hash, self.truncate = set(corrupt_hash), dict(truncate)
        self.proc_partitions, self.char = proc_partitions, char
        self.streams: list[str] = []
        self.calls: list[list[str]] = []

    def data(self, dev: str) -> bytes:
        i = int(re.search(r"(\d+)$", dev)[1]) if "mtd" in dev else int(dev[-1])
        return self.parts[i][1]

    def run(self, argv):
        self.calls.append(argv)
        cmd = shlex.join(argv)
        if argv[:2] == ["cat", "/proc/mtd"]:
            return (0, mtd_text(self.parts).encode(), b"") if self.mtd else (1, b"", b"no")
        if argv[:2] == ["cat", "/proc/partitions"]:
            return 0, self.proc_partitions.encode(), b""
        if argv[0] == "uname":
            return 0, b"Linux fake 2.6.31 mips\n", b""
        if "command -v" in cmd:
            return 0, "".join(u + "\n" for u in self.utils).encode(), b""
        if argv[-1].startswith("test -c"):
            return (0 if self.char else 1), b"", b""
        m = re.search(r"dd if=(\S+) bs=(\d+) 2>/dev/null \| (\w+)", argv[-1])
        if m:
            dev, tool = m[1], m[3]
            h = hashlib.new("sha256" if tool == "sha256sum" else "md5", self.data(dev)).hexdigest()
            if dev in self.corrupt_hash:
                h = "0" * len(h)
            return 0, f"{h}  -\n".encode(), b""
        raise AssertionError(f"unexpected {argv}")

    def run_stream(self, argv, sink):
        dev = argv[1].removeprefix("if=")
        self.streams.append(dev)
        data = self.data(dev)
        if dev in self.truncate:
            data = data[: self.truncate[dev]]
        for i in range(0, len(data), 1000):
            sink(data[i : i + 1000])
        return 0, b""


def run(tmp_path, fake, *extra, target="hub"):
    rc = db.main(
        ["--host", "h", "--target", target, "--out-dir", str(tmp_path / "out"), *extra],
        runner=fake,
    )
    mp = tmp_path / "out" / "manifest.json"
    return rc, json.loads(mp.read_text()) if mp.exists() else None


def test_parse_proc_mtd_odd_names():
    parts = db.parse_proc_mtd(mtd_text())
    assert [(p.index, p.name, p.size, p.erasesize) for p in parts] == [
        (0, "boot", 8192, ERASE), (1, "kernel loader", 8192, ERASE), (2, 'we"ird name', 4096, ERASE),
    ]  # fmt: skip
    assert parts[0].device == "/dev/mtdblock0"


def test_parse_proc_partitions_filters():
    text = "major minor  #blocks  name\n\n 1 0 4096 ram0\n 31 0 128 mtdblock0\n 8 0 64 nand0\n"
    parts = db.parse_proc_partitions(text)
    assert [(p.name, p.size, p.device) for p in parts] == [
        ("mtdblock0", 131072, "/dev/mtdblock0"), ("nand0", 65536, "/dev/nand0"),
    ]  # fmt: skip


def test_happy_path(tmp_path):
    fake = FakeSsh()
    rc, m = run(tmp_path, fake)
    assert rc == 0 and m["status"] == "ok"
    assert m["target"] == "hub" and m["host"] == "h" and m["uname"].startswith("Linux fake")
    assert m["proc_mtd"] == mtd_text() and m["discovery_source"] == "/proc/mtd"
    assert m["timestamp"].endswith("+00:00") and m["hash_utility"] == "sha256sum"
    assert len(m["partitions"]) == 3
    for p in m["partitions"]:
        data = PARTS[p["index"]][1]
        want = hashlib.sha256(data).hexdigest()
        assert p["status"] == "ok" and p["bytes_received"] == p["size_bytes"] == len(data)
        assert p["device_hash"] == p["local_hash"] == want and p["utility"] == "sha256sum"
        assert Path(p["file"]).read_bytes() == data
        assert p["raw"]["status"] == "ok" and p["raw"]["device_path"] == f"/dev/mtd{p['index']}"
    assert not list((tmp_path / "out").glob("*.part"))
    assert "/dev/mtdblock0" in fake.streams and "/dev/mtd0" in fake.streams


def test_md5_fallback_warns(tmp_path):
    rc, m = run(tmp_path, FakeSsh(utils=("dd", "md5sum")))
    assert rc == 0 and m["hash_utility"] == "md5sum"
    assert any("md5sum" in w and "WARNING" in w for w in m["warnings"])
    assert m["partitions"][0]["local_hash"] == hashlib.md5(PARTS[0][1]).hexdigest()


def test_missing_dd_or_hash_tools_fail_cleanly(tmp_path):
    for utils, msg in ((("sha256sum",), "dd"), (("dd",), "sha256sum")):
        fake = FakeSsh(utils=utils)
        rc, m = run(tmp_path, fake)
        assert rc == 1 and m["status"] == "failed" and m["partitions"] == []
        assert any(msg in e for e in m["errors"]) and not fake.streams


def test_hash_mismatch_marks_corrupt(tmp_path):
    rc, m = run(tmp_path, FakeSsh(corrupt_hash={"/dev/mtdblock1"}))
    assert rc == 1 and m["status"] == "failed"
    p = m["partitions"][1]
    assert p["status"] == "corrupt" and p["file"].endswith(".img.CORRUPT")
    assert Path(p["file"]).exists() and not Path(p["file"][: -len(".CORRUPT")]).exists()
    assert [x["status"] for x in m["partitions"]] == ["ok", "corrupt", "ok"]


def test_short_read_fails(tmp_path):
    rc, m = run(tmp_path, FakeSsh(truncate={"/dev/mtdblock0": 100}))
    p = m["partitions"][0]
    assert rc == 1 and p["status"] == "short_read" and p["bytes_received"] == 100
    assert not (tmp_path / "out" / "00_boot.img").exists()


def test_dry_run_reads_nothing(tmp_path):
    fake = FakeSsh()
    rc = db.main(
        ["--host", "h", "--target", "hub", "--out-dir", str(tmp_path / "out"), "--dry-run"],
        runner=fake,
    )
    assert rc == 0 and fake.streams == []
    assert not any("dd if=" in a for c in fake.calls for a in c)
    assert not (tmp_path / "out").exists()


def test_skip_complete_and_conflict(tmp_path):
    run(tmp_path, FakeSsh())
    fake = FakeSsh()
    rc, m = run(tmp_path, fake)
    assert rc == 0 and all(p["status"] == "skipped" for p in m["partitions"])
    assert fake.streams == []
    (tmp_path / "out" / "00_boot.img").write_bytes(b"junk")
    rc, m = run(tmp_path, FakeSsh())
    assert rc == 1 and m["partitions"][0]["status"] == "conflict"
    assert (tmp_path / "out" / "00_boot.img").read_bytes() == b"junk"


def test_only_filter(tmp_path):
    fake = FakeSsh()
    rc, m = run(tmp_path, fake, "--only", "boot", "--only", "2")
    assert rc == 0 and [p["name"] for p in m["partitions"]] == ["boot", 'we"ird name']
    rc, m = run(tmp_path, FakeSsh(), "--only", "nope")
    assert rc == 1 and m["status"] == "failed" and "nope" in m["errors"][0]


def test_remote_fallback_to_proc_partitions(tmp_path):
    parts = {0: ("nand0", b"R" * 2048), 1: ("nand1", b"S" * 1024)}
    text = "major minor #blocks name\n 8 0 2 nand0\n 8 1 1 nand1\n"
    fake = FakeSsh(parts=parts, mtd=False, proc_partitions=text, char=False)
    # device path ends in index digit: /dev/nand0 -> 0
    rc, m = run(tmp_path, fake, target="remote")
    assert rc == 0 and m["discovery_source"] == "/proc/partitions"
    assert [p["device_path"] for p in m["partitions"]] == ["/dev/nand0", "/dev/nand1"]
    assert all("raw" not in p for p in m["partitions"])


def test_hub_without_mtd_fails_with_manifest(tmp_path):
    rc, m = run(tmp_path, FakeSsh(mtd=False))
    assert rc == 1 and m["status"] == "failed" and "/proc/mtd" in m["errors"][0]


def test_import_has_no_side_effects():
    assert callable(db.main) and callable(db.SshRunner)
