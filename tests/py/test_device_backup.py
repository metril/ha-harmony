"""device_backup against a fake SSH transport (no network, no sleeps)."""

import hashlib
import json
import re
import shlex
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import device_backup as db

ERASE = 0x1000
_EL = 0x1F000  # Elite gluebi/NAND erase size; mtd0-3 use 0x20000
ELITE_NAMES = ["u-boot env", "ubl", "u-boot", "ubi", "kernel", "kernel2", "root", "data",
               "cache", "mfg", "log"]  # fmt: skip
ELITE_ERASE = {i: (0x20000 if i < 4 else _EL) for i in range(11)}
ELITE = {  # real /proc/mtd names and erase sizes, tiny contents
    i: (n, bytes([i + 1]) * (ELITE_ERASE[i] // 64 * 2)) for i, n in enumerate(ELITE_NAMES)
}
ELITE_MOUNTS = (
    "rootfs / rootfs rw 0 0\n"
    "ubi0:cache /cache ubifs rw,relatime 0 0\n"
    "/dev/mtdblock6 /mnt/root squashfs ro,relatime 0 0\n"
    "ubi0:data /mnt/data ubifs rw,relatime 0 0\n"
    "none / unionfs rw,relatime,dirs=/mnt/data=rw:/mnt/root=ro 0 0\n"
)
HUB_MOUNTS = "/dev/mtdblock4 / jffs2 rw,relatime 0 0\n/dev/mtdblock5 /x jffs2 rw 0 0\n"
PARTS = {  # index -> (name, bytes)
    0: ("boot", b"B" * 8192),
    1: ("kernel loader", bytes(range(256)) * 32),
    2: ('we"ird name', b"\x00\xff" * 2048),
}


def mtd_text(parts=PARTS, erase=None) -> str:
    lines = ["dev:    size   erasesize  name"]
    for i, (n, d) in parts.items():
        lines.append(f'mtd{i}: {len(d):08x} {(erase or {}).get(i, ERASE):08x} "{n}"')
    return "\n".join(lines) + "\n"


class FakeSsh:
    def __init__(self, parts=PARTS, utils=("dd", "cat", "sha256sum", "md5sum"), mtd=True,
                 corrupt_hash=(), truncate=(), proc_partitions="", char=True,
                 devmap=None, command_v=True, hash_fail=None, explode=None,
                 stream_err=b"", mounts=None, erase=None):  # fmt: skip
        self.mounts, self.erase = mounts, erase
        self.parts, self.utils, self.mtd = parts, utils, mtd
        self.devmap = (
            devmap
            if devmap is not None
            else {
                f"/dev/{kind}{i}{sfx}": d
                for i, (_, d) in parts.items()
                for kind, sfx in (("mtdblock", ""), ("mtd", ""), ("mtd", "ro"))
            }
            | {f"/dev/mtdr{i}": d for i, (_, d) in parts.items()}
        )
        self.command_v, self.hash_fail = command_v, dict(hash_fail or {})
        self.explode, self.stream_err = dict(explode or {}), stream_err
        self.corrupt_hash, self.truncate = set(corrupt_hash), dict(truncate)
        self.proc_partitions, self.char = proc_partitions, char
        self.streams: list[str] = []
        self.stream_argvs: list[list[str]] = []
        self.calls: list[list[str]] = []

    def data(self, dev: str) -> bytes:
        return self.devmap[dev]  # exact lookup: an unknown node is a test failure, not wrong data

    def run(self, argv):
        self.calls.append(argv)
        cmd = shlex.join(argv)
        if argv[:2] == ["cat", "/proc/mtd"]:
            return (0, mtd_text(self.parts, self.erase).encode(), b"") if self.mtd else (1, b"", b"no")
        if argv[:2] == ["cat", "/proc/partitions"]:
            return 0, self.proc_partitions.encode(), b""
        if argv[:2] == ["cat", "/proc/mounts"]:
            return (0, self.mounts.encode(), b"") if self.mounts is not None else (1, b"", b"no")
        if argv[0] == "uname":
            return 0, b"Linux fake 2.6.31 mips\n", b""
        if "command -v" in cmd:
            if not self.command_v:
                return 127, b"", b"sh: command: not found"
            return 0, "".join(u + "\n" for u in self.utils).encode(), b""
        for u, probe in db.PROBES.items():
            if argv[-1] == probe:
                return (0 if u in self.utils else 1), b"", b""
        if argv[-1].startswith("test -c"):
            node = shlex.split(argv[-1])[-1]
            ok = self.char if isinstance(self.char, bool) else node in self.char
            return (0 if ok else 1), b"", b""
        m = re.fullmatch(
            r'\{ (?:dd if=(\S+) bs=(\d+)|cat (\S+)); echo "rc=\$\?" >&2; \} \| (\w+)', argv[-1]
        )
        if m:
            dev, tool = m[1] or m[3], m[4]
            data, bs = self.data(dev), int(m[2] or 0)
            fail = self.hash_fail.get(dev)
            if fail:
                data = data[: len(data) // 2]
            h = hashlib.new("sha256" if tool == "sha256sum" else "md5", data).hexdigest()
            if dev in self.corrupt_hash:
                h = "0" * len(h)
            err = b""
            if bs:
                recs = f"{len(data) // bs}+{1 if len(data) % bs else 0}"
                err = f"{recs} records in\n{recs} records out\n".encode()
            err += b"rc=1\n" if fail == "rc" else b"rc=0\n"
            return 0, f"{h}  -\n".encode(), err
        raise AssertionError(f"unexpected {argv}")

    def run_stream(self, argv, sink):
        self.calls.append(argv)
        self.stream_argvs.append(argv)
        dev = argv[1].removeprefix("if=")
        self.streams.append(dev)
        if dev in self.explode:
            raise self.explode[dev]
        data = self.data(dev)
        if dev in self.truncate:
            data = data[: self.truncate[dev]]
        for i in range(0, len(data), 1000):
            sink(data[i : i + 1000])
        return 0, self.stream_err


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
    devmap = {"/dev/nand0": parts[0][1], "/dev/nand1": parts[1][1]}
    fake = FakeSsh(parts=parts, mtd=False, proc_partitions=text, char=False, devmap=devmap)
    rc, m = run(tmp_path, fake, target="remote")
    assert rc == 0 and m["discovery_source"] == "/proc/partitions"
    assert [p["device_path"] for p in m["partitions"]] == ["/dev/nand0", "/dev/nand1"]
    assert all("raw" not in p for p in m["partitions"])


def test_hub_without_mtd_fails_with_manifest(tmp_path):
    rc, m = run(tmp_path, FakeSsh(mtd=False))
    assert rc == 1 and m["status"] == "failed" and "/proc/mtd" in m["errors"][0]


def test_import_has_no_side_effects():
    assert callable(db.main) and callable(db.SshRunner)


# --- SSH timeouts -------------------------------------------------------------------------


def ssh_opts(argv):
    return {argv[i + 1] for i, a in enumerate(argv) if a == "-o"}


def test_ssh_argv_has_timeouts():
    opts = ssh_opts(db.SshRunner("h")._ssh(["true"]))
    assert {"ConnectTimeout=15", "ServerAliveInterval=15", "ServerAliveCountMax=3"} <= opts
    opts = ssh_opts(db.SshRunner("h", timeout=7)._ssh(["true"]))
    assert {"ConnectTimeout=7", "ServerAliveInterval=7", "ServerAliveCountMax=3"} <= opts


def test_ssh_port_flag():
    assert "-p" not in db.SshRunner("h")._ssh(["true"])
    argv = db.SshRunner("h", port=2222)._ssh(["true"])
    assert argv[argv.index("-p") + 1] == "2222"


def test_port_cli_knob(tmp_path, monkeypatch):
    seen = {}

    def factory(host, user, key, timeout, idle_timeout, port=None):
        seen["port"] = port
        return FakeSsh()

    monkeypatch.setattr(db, "SshRunner", factory)
    assert db.main(["--host", "h", "--port", "2222", "--target", "hub",
                    "--out-dir", str(tmp_path / "o"), "--dry-run"]) == 0  # fmt: skip
    assert seen == {"port": 2222}


def test_ssh_timeout_cli_knob(tmp_path, monkeypatch):
    seen = {}

    def factory(host, user, key, timeout, idle_timeout, port=None):
        seen.update(timeout=timeout, idle=idle_timeout)
        return FakeSsh()

    monkeypatch.setattr(db, "SshRunner", factory)
    assert db.main(["--host", "h", "--target", "hub", "--out-dir", str(tmp_path / "o"),
                    "--ssh-timeout", "9", "--dry-run"]) == 0  # fmt: skip
    assert seen == {"timeout": 9, "idle": None}
    assert db.main(["--host", "h", "--target", "hub", "--out-dir", str(tmp_path / "o"),
                    "--idle-timeout", "300", "--dry-run"]) == 0  # fmt: skip
    assert seen["idle"] == 300


def test_idle_timeout_defaults_to_4x_ssh_timeout():
    assert db.SshRunner("h", timeout=9).idle_timeout == 36
    assert db.SshRunner("h", timeout=9, idle_timeout=300).idle_timeout == 300


def _python_child(code):
    return lambda argv: [sys.executable, "-I", "-c", code]


def test_stream_stall_is_killed():
    r = db.SshRunner("h", idle_timeout=0.2)
    r._ssh = _python_child("import time; time.sleep(60)")
    t0, got = time.monotonic(), []
    rc, err = r.run_stream(["cat", "x"], got.append)
    assert rc != 0 and b"stalled" in err and got == [] and time.monotonic() - t0 < 10


def test_run_timeout_is_enforced():
    r = db.SshRunner("h", run_timeout=0.2)
    r._ssh = _python_child("import time; time.sleep(60)")
    rc, _, err = r.run(["x"])
    assert rc != 0 and b"timed out" in err


def test_stream_delivers_data_and_stderr():
    r = db.SshRunner("h", idle_timeout=5)
    r._ssh = _python_child(
        "import sys; sys.stdout.buffer.write(b'abc' * 1000); sys.stderr.write('oops'); sys.exit(3)"
    )
    got = []
    rc, err = r.run_stream(["x"], got.append)
    assert rc == 3 and b"".join(got) == b"abc" * 1000 and err == b"oops"


# --- cat read path ------------------------------------------------------------------------


def test_dd_preferred_when_present(tmp_path):
    fake = FakeSsh()
    rc, m = run(tmp_path, fake)
    assert rc == 0 and m["read_method"] == "dd"
    assert {a[0] for a in fake.stream_argvs} == {"dd"}
    assert ["dd", "if=/dev/mtdblock0", "bs=4096"] in fake.stream_argvs
    assert m["partitions"][0]["read_method"] == "dd"
    assert m["partitions"][0]["read_node"] == "/dev/mtdblock0"


def test_cat_with_ro_node_when_dd_missing(tmp_path):
    fake = FakeSsh(utils=("cat", "sha256sum"))
    rc, m = run(tmp_path, fake)
    assert rc == 0 and m["status"] == "ok" and m["read_method"] == "cat"
    assert any("dd missing" in w for w in m["warnings"])
    assert fake.stream_argvs == [["cat", f"/dev/mtd{i}ro"] for i in PARTS]
    for p in m["partitions"]:
        data = PARTS[p["index"]][1]
        assert p["read_method"] == "cat" and p["read_node"] == f"/dev/mtd{p['index']}ro"
        assert p["device_path"] == p["read_node"] and "raw" not in p
        assert p["device_hash"] == p["local_hash"] == hashlib.sha256(data).hexdigest()
        assert Path(p["file"]).read_bytes() == data
    hash_cmds = [c[-1] for c in fake.calls if "| sha256sum" in c[-1]]
    assert hash_cmds and all(c.startswith("{ cat /dev/mtd") for c in hash_cmds)


def test_cat_with_mtdr_node_when_no_mtdNro(tmp_path):
    nodes = {f"/dev/mtdr{i}" for i in PARTS}
    fake = FakeSsh(utils=("cat", "sha256sum"), char=nodes)
    rc, m = run(tmp_path, fake)
    assert rc == 0 and m["read_method"] == "cat"
    assert fake.stream_argvs == [["cat", f"/dev/mtdr{i}"] for i in PARTS]
    assert all(p["read_node"] == f"/dev/mtdr{p['index']}" for p in m["partitions"])


def test_cat_block_device_without_ro_node(tmp_path):
    fake = FakeSsh(utils=("cat", "sha256sum"), char=False)
    rc, m = run(tmp_path, fake)
    assert rc == 0 and m["read_method"] == "cat"
    assert fake.stream_argvs == [["cat", f"/dev/mtdblock{i}"] for i in PARTS]
    assert all(p["read_node"] == f"/dev/mtdblock{p['index']}" for p in m["partitions"])
    assert all("raw" not in p for p in m["partitions"])


def test_neither_dd_nor_cat_fails_early(tmp_path):
    fake = FakeSsh(utils=("sha256sum",))
    rc, m = run(tmp_path, fake)
    assert rc == 1 and m["status"] == "failed" and not fake.streams
    assert any("dd" in e and "cat" in e for e in m["errors"])


# --- brick safety -------------------------------------------------------------------------

ALLOWED_PROGRAMS = {"sh", "cat", "dd", "test", "uname", "sha256sum", "md5sum", "command"}
DEV = r"/dev/[A-Za-z0-9_.-]+"
SH_SHAPES = tuple(
    re.compile(x)
    for x in (
        re.escape(db.DETECT_SCRIPT),
        *(re.escape(p) for p in db.PROBES.values()),
        rf"test -[cb] {DEV}",
        rf'\{{ (?:dd if={DEV} bs=\d+|cat {DEV}); echo "rc=\$\?" >&2; \}} \| (?:sha256sum|md5sum)',
    )
)
ARG_OK = {
    "cat": re.compile(rf"/proc/mtd|/proc/partitions|/proc/mounts|{DEV}"),
    "dd": re.compile(rf"if={DEV}|bs=\d+"),
    "uname": re.compile(r"-a"),
}


def violations(remote: str) -> list[str]:
    """Allow-list check of one remote command line; empty list means permitted."""
    try:
        argv = shlex.split(remote)
    except ValueError:
        return [f"unparseable: {remote!r}"]
    if not argv or argv[0] not in ALLOWED_PROGRAMS:
        return [f"program not allowed: {remote!r}"]
    if argv[0] == "sh":
        ok = len(argv) == 3 and argv[1] == "-c" and any(r.fullmatch(argv[2]) for r in SH_SHAPES)
        return [] if ok else [f"sh script not a permitted shape: {remote!r}"]
    if argv[0] in ARG_OK:
        bad = [a for a in argv[1:] if not ARG_OK[argv[0]].fullmatch(a)]
        return [f"{argv[0]} arg not allowed: {bad}"] if bad else []
    return []  # test/sha256sum/md5sum/command only ever reached via the sh shapes above


class RecordingSsh(db.SshRunner):
    """Real SshRunner argv construction; records the exact ssh argv, delegates to a fake."""

    def __init__(self, fake: FakeSsh, log: list[list[str]]) -> None:
        super().__init__("h")
        self.fake, self.log = fake, log

    def run(self, argv):
        self.log.append(self._ssh(argv))
        return self.fake.run(argv)

    def run_stream(self, argv, sink):
        self.log.append(self._ssh(argv))
        return self.fake.run_stream(argv, sink)


def sh(script: str) -> str:
    return shlex.join(["sh", "-c", script])


KNOWN_BAD = (
    "dd if=/dev/zero of=/dev/mtd3", "nandwrite -p /dev/mtd3 x", "flash_erase /dev/mtd1",
    "mtd_debug write /dev/mtd1 0 4 x", "ubiupdatevol /dev/ubi0_0 x", "flashcp x /dev/mtd0",
    "ubiformat /dev/mtd1", "ubimkvol /dev/ubi0 -N x", "mtd write x boot", "sysupgrade x.bin",
    "cp img /dev/mtd0", "rm /mnt/data/x", "mv a b", "sed -i s/a/b/ /etc/x", "tee /dev/mtd0",
    sh("cat x >/dev/mtd2"), sh("cat x > /dev/mtdblock2"), sh("tee /dev/mtd0 < x"),
    sh("cat img >$DEV"), sh("echo hi > /etc/passwd"), sh("rm -rf /mnt/data"),
    sh("sed -i s/a/b/ /etc/x"), sh("dd if=/dev/mtd0 bs=4096 of=/dev/mtd1"),
    sh('{ dd if=/dev/mtd0 bs=4096; echo "rc=$?" >&2; } | sha256sum; rm /x'),
)  # fmt: skip


def test_checker_rejects_known_bad_and_new_writes():
    for bad in KNOWN_BAD:
        assert violations(bad), bad
    # a write command newly introduced into the tool must fail the allow-list
    for new in ("flashcp img /dev/mtd0", sh("tee /dev/mtd0"), sh("cat img | tee /dev/mtd0")):
        assert violations(new), new
    for good in (
        "cat /proc/mtd", "dd if=/dev/mtd0 bs=4096", "uname -a", "cat /dev/mtd0ro",
        sh('{ dd if=/dev/mtd0 bs=4096; echo "rc=$?" >&2; } | sha256sum'),
        sh("test -c /dev/mtd0"), sh(db.DETECT_SCRIPT), sh(db.PROBES["dd"]),
    ):  # fmt: skip
        assert not violations(good), good


GLUEBI_NODE = re.compile(r"mtd(?:block|r)?[78](?!\d)")


def test_commands_are_read_only_by_construction(tmp_path):
    log: list[list[str]] = []
    scenarios = [
        ({}, ()),  # full dd dump (+ raw)
        ({}, ("--dry-run",)),
        ({"utils": ("cat", "sha256sum")}, ()),  # cat + ro
        ({"utils": ("cat", "md5sum"), "char": False}, ()),  # cat + block, md5
        ({"corrupt_hash": {"/dev/mtdblock1"}}, ()),  # failure paths
        ({"truncate": {"/dev/mtdblock0": 100}}, ()),
        ({"utils": ("sha256sum",)}, ()),
        ({"utils": ("dd",)}, ()),
        ({"mtd": False}, ()),
        ({"command_v": False}, ()),  # direct-probe fallback
        ({"hash_fail": {"/dev/mtdblock0": "rc"}}, ()),  # device hash pass failure
        ({"parts": ELITE, "erase": ELITE_ERASE, "mounts": ELITE_MOUNTS, "utils": ("cat", "md5sum"),
          "char": False}, ()),  # Elite: cat + md5 + block nodes only
        ({"parts": ELITE, "erase": ELITE_ERASE, "mounts": ELITE_MOUNTS}, ()),
        ({"parts": ELITE, "erase": ELITE_ERASE, "mounts": ELITE_MOUNTS}, ("--dry-run",)),
        ({"parts": ELITE, "erase": ELITE_ERASE, "mounts": ELITE_MOUNTS}, ("--only", "data")),
    ]
    for i, (kw, extra) in enumerate(scenarios):
        out = tmp_path / str(i)
        tgt = "remote" if "mounts" in kw else "hub"
        db.main(["--host", "h", "--target", tgt, "--out-dir", str(out), *extra],
                runner=RecordingSsh(FakeSsh(**kw), log))  # fmt: skip
        if i == 0:  # second pass: skip/conflict path against the finished dump
            db.main(["--host", "h", "--target", "hub", "--out-dir", str(out)],
                    runner=RecordingSsh(FakeSsh(), log))  # fmt: skip
            (out / "00_boot.img").write_bytes(b"junk")
            db.main(["--host", "h", "--target", "hub", "--out-dir", str(out)],
                    runner=RecordingSsh(FakeSsh(), log))  # fmt: skip
    assert len(log) > 40 and all(c[0] == "ssh" for c in log)
    for argv in log:
        assert not violations(argv[-1]), argv  # the remote command itself, allow-listed
        assert not GLUEBI_NODE.search(argv[-1]), argv  # Elite: mtd7/mtd8 never touched


# --- --verify -----------------------------------------------------------------------------


def make_tree(tmp_path, parts=PARTS):
    rc, m = run(tmp_path, FakeSsh(parts=parts))
    assert rc == 0
    return tmp_path / "out", m


def verify(d, *extra):
    return db.main(["--verify", str(d), *extra])


def test_verify_good_tree(tmp_path, capsys):
    d, _ = make_tree(tmp_path)
    assert verify(d) == 0
    cap = capsys.readouterr()
    assert "RESULT: OK" in cap.out and "boot" in cap.out and "(raw)" in cap.out
    assert cap.err == ""


def test_verify_flipped_byte_names_partition(tmp_path, capsys):
    d, _ = make_tree(tmp_path)
    img = d / "01_kernel_loader.img"
    b = bytearray(img.read_bytes())
    b[100] ^= 0xFF
    img.write_bytes(bytes(b))
    assert verify(d) == 1
    err = capsys.readouterr().err
    assert "kernel loader" in err and "01_kernel_loader.img" in err
    assert "boot" not in err.replace("kernel loader", "")  # only the damaged one is named


def test_verify_size_mismatch(tmp_path, capsys):
    d, m = make_tree(tmp_path)
    m["partitions"][0]["size_bytes"] += 4096
    (d / "manifest.json").write_text(json.dumps(m))
    assert verify(d) == 1
    assert "size" in capsys.readouterr().err


def test_verify_missing_image(tmp_path, capsys):
    d, _ = make_tree(tmp_path)
    (d / "02_we_ird_name.img").unlink()
    assert verify(d) == 1
    assert "missing" in capsys.readouterr().err


def test_verify_extra_image_warns_but_passes(tmp_path, capsys):
    d, _ = make_tree(tmp_path)
    (d / "stray.img").write_bytes(b"x")
    assert verify(d) == 0
    cap = capsys.readouterr()
    assert "stray.img" in cap.err and "WARNING" in cap.err


def test_verify_leftovers_fail(tmp_path, capsys):
    for suffix in (".part", ".CORRUPT"):
        d, _ = make_tree(tmp_path / suffix.strip("."))
        (d / f"00_boot.img{suffix}").write_bytes(b"x")
        assert verify(d) == 1
        assert f"leftover 00_boot.img{suffix}" in capsys.readouterr().err


def test_verify_missing_or_dryrun_manifest_fails(tmp_path):
    assert verify(tmp_path) == 1
    run(tmp_path, FakeSsh(), "--dry-run")  # dry-run writes no manifest
    (tmp_path / "out").mkdir()
    (tmp_path / "out" / "manifest.json").write_text(json.dumps({"partitions": []}))
    assert verify(tmp_path / "out") == 1


def test_verify_json_shape_and_magics(tmp_path, capsys):
    parts = {
        0: ("rootfs", b"hsqs" + b"\0" * 4092),
        1: ("data", b"\x85\x19" + b"\0" * 4094),
        2: ("ubi", b"UBI#" + b"\0" * 4092),
        3: ("u-boot", b"\x27\x05\x19\x56" + b"\0" * 4092),
        4: ("misc", b"\0" * 4096),
    }
    d, _ = make_tree(tmp_path, parts)
    capsys.readouterr()
    assert verify(d, "--json") == 0
    res = json.loads(capsys.readouterr().out)
    assert set(res) == {"dir", "ok", "images", "errors", "warnings"} and res["ok"] is True
    assert res["errors"] == [] and res["warnings"] == []
    assert {"partition", "file", "status", "magic", "size", "expected_size", "hash"} <= set(
        res["images"][0]
    )
    magics = {i["partition"]: i["magic"] for i in res["images"]}
    assert magics["rootfs"] == "squashfs" and magics["data"] == "jffs2"
    assert magics["ubi"] == "ubi" and magics["u-boot"] == "uboot" and magics["misc"] == "unknown"
    # an unexpected magic alone never fails; a bad hash does, and --json still reports it
    (d / "04_misc.img").write_bytes(b"\1" * 4096)
    assert verify(d, "--json") == 1
    res = json.loads(capsys.readouterr().out)
    assert res["ok"] is False and any("misc" in e for e in res["errors"])


# --- review fixes -------------------------------------------------------------------------


def test_zero_size_partition_is_error_not_ok(tmp_path):
    parts = {0: ("empty", b""), 1: ("boot", b"B" * 4096)}
    fake = FakeSsh(parts=parts)
    rc, m = run(tmp_path, fake)
    p = m["partitions"][0]
    assert rc == 1 and m["status"] == "failed" and p["status"] == "error"
    assert "size" in p["error"] and p["file"] is None
    assert not list((tmp_path / "out").glob("00_empty*")) and "/dev/mtdblock0" not in fake.streams
    assert m["partitions"][1]["status"] == "ok"


def test_verify_rejects_zero_size_image(tmp_path, capsys):
    d, m = make_tree(tmp_path)
    empty = d / "00_boot.img"
    empty.write_bytes(b"")
    e = hashlib.sha256(b"").hexdigest()
    m["partitions"][0].update(size_bytes=0, local_hash=e, device_hash=e)
    m["partitions"][0].pop("raw")
    (d / "manifest.json").write_text(json.dumps(m))
    assert verify(d) == 1
    assert "00_boot.img" in capsys.readouterr().err


def test_device_hash_read_failure_is_not_corrupt(tmp_path):
    fake = FakeSsh(hash_fail={"/dev/mtdblock1": "rc"})
    rc, m = run(tmp_path, fake)
    p = m["partitions"][1]
    assert rc == 1 and p["status"] == "hash_failed" and "may be intact" in p["error"]
    assert p["local_hash"] and p["device_hash"] is None
    assert not list((tmp_path / "out").glob("*.CORRUPT"))
    assert not (tmp_path / "out" / "01_kernel_loader.img").exists()  # never promoted unverified
    assert Path(p["file"]).name == "01_kernel_loader.img.part"


def test_device_hash_short_dd_records_detected(tmp_path):
    rc, m = run(tmp_path, FakeSsh(hash_fail={"/dev/mtdblock0": "short"}))
    p = m["partitions"][0]
    assert rc == 1 and p["status"] == "hash_failed" and "records" in p["error"]


def test_existing_image_hash_failure_is_not_conflict(tmp_path):
    run(tmp_path, FakeSsh())
    rc, m = run(tmp_path, FakeSsh(hash_fail={"/dev/mtdblock0": "rc"}))
    assert rc == 1 and m["partitions"][0]["status"] == "hash_failed"
    assert (tmp_path / "out" / "00_boot.img").read_bytes() == PARTS[0][1]


def test_stream_stderr_recorded(tmp_path):
    rc, m = run(tmp_path, FakeSsh(stream_err=b"2+0 records in\n2+0 records out\n"))
    assert rc == 0 and "2+0 records out" in m["partitions"][0]["device_stderr"]


def test_manifest_written_incrementally_on_midrun_exception(tmp_path):
    for exc in (RuntimeError("link dropped"), ZeroDivisionError("boom"), KeyboardInterrupt()):
        out = tmp_path / type(exc).__name__
        fake = FakeSsh(explode={"/dev/mtdblock1": exc})
        argv = ["--host", "h", "--target", "hub", "--out-dir", str(out)]
        try:
            db.main(argv, runner=fake)
        except KeyboardInterrupt:
            assert isinstance(exc, KeyboardInterrupt)
        m = json.loads((out / "manifest.json").read_text())
        assert m["status"] == "failed" and [p["name"] for p in m["partitions"]] == ["boot"]
        assert m["partitions"][0]["status"] == "ok" and m["partitions"][0]["device_hash"]
        assert (out / "00_boot.img").exists()
        assert db.main(["--verify", str(out)]) == 1  # unfinished run never verifies as OK


def test_detect_falls_back_to_direct_probes(tmp_path):
    fake = FakeSsh(command_v=False)
    rc, m = run(tmp_path, fake)
    assert rc == 0 and m["utilities"]["dd"] and m["utilities"]["sha256sum"]
    assert not m["utilities"]["find"]
    rc, m = run(tmp_path / "x", FakeSsh(command_v=False, utils=("sha256sum",)))
    assert rc == 1 and any("dd" in e for e in m["errors"])  # probes also say "absent"


def test_ssh_failure_reported_as_connection_error(tmp_path):
    class Down(FakeSsh):
        def run(self, argv):
            return 255, b"", b"ssh: connect to host h port 22: Connection refused\n"

    b = db.Backup(Down(), "hub", "h", tmp_path / "o")
    assert b.execute([], True) == 1
    assert b.errors == ["ssh connection failed: ssh: connect to host h port 22: Connection refused"]


def test_keyboard_interrupt_exits_130_without_traceback(tmp_path, monkeypatch, capsys):
    class Boom(FakeSsh):
        def run_stream(self, argv, sink):
            raise KeyboardInterrupt

    out = tmp_path / "o"
    assert db.main(["--host", "h", "--target", "hub", "--out-dir", str(out)], runner=Boom()) == 130
    assert "interrupted" in capsys.readouterr().err
    assert json.loads((out / "manifest.json").read_text())["status"] == "failed"


# --- mounted gluebi volumes (Elite) ---------------------------------------------------------


def elite(tmp_path, *extra, **kw):
    fake = FakeSsh(parts=ELITE, erase=ELITE_ERASE, mounts=ELITE_MOUNTS, utils=("cat", "md5sum"),
                   char=False, **kw)  # fmt: skip
    return fake, *run(tmp_path, fake, *extra, target="remote")


def test_elite_skips_mounted_gluebi_but_images_ro_squashfs(tmp_path, capsys):
    fake, rc, m = elite(tmp_path)
    assert rc == 0 and m["status"] == "ok"
    by = {p["name"]: p for p in m["partitions"]}
    for n in ("data", "cache"):
        assert by[n]["status"] == "skipped" and by[n]["file"] is None
        assert by[n]["reason"] == (
            f"mounted gluebi volume (ubi0:{n}, rw) \u2014 contained in mtd3 'ubi'"
        )
    for n in ("root", "ubi", "kernel", "mfg", "log", "ubl"):
        assert by[n]["status"] == "ok", n  # ro squashfs (mtdblock6) is still imaged
    assert "/dev/mtdblock6" in fake.streams
    assert not [d for d in fake.streams if GLUEBI_NODE.search(d)]
    assert not [c for c in fake.calls if GLUEBI_NODE.search(shlex.join(c))]
    assert "data: skipped" in capsys.readouterr().out


def test_elite_dry_run_announces_skips(tmp_path, capsys):
    fake, rc, _ = elite(tmp_path, "--dry-run")
    out = capsys.readouterr().out
    assert rc == 0 and "SKIP mtd7" in out and "SKIP mtd8" in out
    assert "'data'" not in out and "'cache'" not in out  # not in the read plan
    assert not fake.streams


def test_elite_verify_passes_with_skips(tmp_path, capsys):
    elite(tmp_path)
    assert verify(tmp_path / "out") == 0
    out = capsys.readouterr().out
    assert "SKIPPED" in out and "mounted gluebi volume (ubi0:data, rw)" in out
    assert "RESULT: OK" in out


def test_elite_verify_still_fails_on_other_errors(tmp_path):
    elite(tmp_path)
    (tmp_path / "out" / "03_ubi.img").write_bytes(b"x")
    assert verify(tmp_path / "out") == 1


def test_unknown_container_and_rw_only(tmp_path):
    parts = {i: v for i, v in ELITE.items() if v[0] != "ubi"}
    fake = FakeSsh(parts=parts, erase=ELITE_ERASE, mounts=ELITE_MOUNTS)
    _, m = run(tmp_path, fake, target="remote")
    by = {p["name"]: p for p in m["partitions"]}
    assert by["data"]["reason"].endswith("contained in unknown container")
    ro = ELITE_MOUNTS.replace("ubifs rw,relatime", "ubifs ro,relatime")
    _, m = run(tmp_path / "ro", FakeSsh(parts=ELITE, erase=ELITE_ERASE, mounts=ro), target="remote")
    assert all(p["status"] == "ok" for p in m["partitions"])  # ro ubifs is not skipped


def test_hub_jffs2_mounts_not_skipped(tmp_path):
    _, m = run(tmp_path, FakeSsh(mounts=HUB_MOUNTS))
    assert [p["status"] for p in m["partitions"]] == ["ok"] * 3
    _, m = run(tmp_path / "b", FakeSsh(mounts=ELITE_MOUNTS.replace("mtdblock6", "x")))
    assert all(p["status"] == "ok" for p in m["partitions"])  # hub target never skips
