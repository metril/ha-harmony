"""Shared fixtures: one cmake build of the C binaries per session (file-locked
so xdist workers don't race) and a `hub_process` launcher on an ephemeral port."""

import fcntl
import os
import re
import select
import shutil
import signal
import subprocess
import time
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "build"
TOKEN = "test-token"


def _run(cmd: list[str]) -> None:
    res = subprocess.run(cmd, cwd=ROOT, check=False, capture_output=True, text=True)
    if res.returncode:
        pytest.fail(f"{' '.join(cmd)} failed:\n{res.stdout}\n{res.stderr}", pytrace=False)


@pytest.fixture(scope="session")
def built_binaries() -> Path:
    """Build dir containing the hub binaries (built once, safe under xdist)."""
    if shutil.which("cmake") is None:
        pytest.fail("cmake not found: cannot build the hub binaries", pytrace=False)
    BUILD.mkdir(exist_ok=True)
    with open(BUILD / ".build.lock", "w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        try:
            _run(["cmake", "-S", ".", "-B", "build"])
            _run(["cmake", "--build", "build", "-j", str(os.cpu_count() or 2), "--target", "hub"])
        finally:
            fcntl.flock(lock, fcntl.LOCK_UN)
    return BUILD


class HubProcess:
    def __init__(self, proc: subprocess.Popen, port: int, token: str):
        self.proc = proc
        self.port = port
        self.token = token

    @property
    def url(self) -> str:
        return f"ws://127.0.0.1:{self.port}/"

    def alive(self) -> bool:
        return self.proc.poll() is None


def _read_port(proc: subprocess.Popen, timeout: float = 10.0) -> int:
    deadline = time.monotonic() + timeout
    assert proc.stdout is not None
    fd = proc.stdout.fileno()
    buf = b""
    while time.monotonic() < deadline:
        if select.select([fd], [], [], 0.1)[0]:
            chunk = os.read(fd, 256)
            if not chunk:
                break
            buf += chunk
            m = re.search(rb"listening port=(\d+)\n", buf)
            if m:
                return int(m.group(1))
        elif proc.poll() is not None:
            break
    proc.kill()
    raise AssertionError(f"hub did not report a listening port; output={buf!r}")


@pytest.fixture
def start_hub(built_binaries, tmp_path):
    """Factory: start_hub(*extra_args, token=TOKEN) -> HubProcess (port 0)."""
    procs: list[subprocess.Popen] = []

    def _start(*extra: str, token: str = TOKEN) -> HubProcess:
        proc = subprocess.Popen(
            [str(built_binaries / "hub"), "--sim", "--port", "0", "--token", token, *extra],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            cwd=tmp_path,
        )
        procs.append(proc)
        return HubProcess(proc, _read_port(proc), token)

    yield _start
    for proc in procs:
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        err = proc.stderr.read().decode() if proc.stderr else ""
        assert "Sanitizer" not in err, err


@pytest.fixture
def hub_process(start_hub) -> HubProcess:
    return start_hub()
