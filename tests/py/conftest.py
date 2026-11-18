"""Shared fixtures: one cmake build of the C binaries per session (file-locked
so xdist workers don't race) and a `hub_process` launcher on an ephemeral port."""

import fcntl
import os
import re
import select
import shutil
import signal
import socket
import subprocess
import tempfile
import time
from pathlib import Path

import pytest
import pytest_socket

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "build"
TOKEN = "test-token"


@pytest.fixture(autouse=True)
def _loopback_sockets(socket_enabled):
    """pytest-homeassistant blocks sockets by default; these tests talk to the real hub."""
    pytest_socket.socket_allow_hosts(["127.0.0.1", "localhost"], allow_unix_socket=True)
    yield


def free_port() -> int:
    """A currently free TCP port on loopback (for restart-on-same-port tests)."""
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


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
            _run(
                [
                    "cmake",
                    "--build",
                    "build",
                    "-j",
                    str(os.cpu_count() or 2),
                    "--target",
                    "hub",
                    "remote_sim",
                ]
            )
        finally:
            fcntl.flock(lock, fcntl.LOCK_UN)
    return BUILD


class HubProcess:
    def __init__(
        self,
        proc: subprocess.Popen,
        port: int,
        token: str,
        rf: str | None = None,
        out_path: Path | None = None,
        err_path: Path | None = None,
    ):
        self.proc = proc
        self.out_path = out_path  # stdout/stderr go to files so the pipes never fill
        self.err_path = err_path
        self.out_pos = 0  # read cursor into out_path (after the port line)
        self.port = port
        self.token = token
        self.rf = rf  # RF socket path when started with --rf

    @property
    def url(self) -> str:
        return f"ws://127.0.0.1:{self.port}/"

    def alive(self) -> bool:
        return self.proc.poll() is None

    def stop(self) -> None:
        """SIGTERM the hub and wait for it to exit (port is free again afterwards)."""
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
        self.proc.wait(timeout=5)


def _read_port(proc: subprocess.Popen, out_path: Path, timeout: float = 10.0) -> int:
    """Poll the hub's stdout file for the port line (the file keeps draining the pipe)."""
    deadline = time.monotonic() + timeout
    buf = b""
    while time.monotonic() < deadline:
        buf = out_path.read_bytes()
        m = re.search(rb"listening port=(\d+)\n", buf)
        if m:
            return int(m.group(1))
        if proc.poll() is not None:
            break
        time.sleep(0.01)
    proc.kill()
    raise AssertionError(f"hub did not report a listening port; output={buf!r}")


@pytest.fixture
def start_hub(built_binaries, tmp_path):
    """Factory: start_hub(*extra_args, token=TOKEN, port=0) -> HubProcess."""
    procs: list[tuple[subprocess.Popen, Path]] = []

    def _start(*extra: str, token: str = TOKEN, port: int = 0) -> HubProcess:
        rf = extra[extra.index("--rf") + 1] if "--rf" in extra else None
        n = len(procs)
        out_path, err_path = tmp_path / f"hub{n}.out", tmp_path / f"hub{n}.err"
        with open(out_path, "wb") as out, open(err_path, "wb") as err:
            proc = subprocess.Popen(
                [
                    str(built_binaries / "hub"),
                    "--sim",
                    "--port",
                    str(port),
                    "--token",
                    token,
                    *extra,
                ],
                stdout=out,
                stderr=err,
                cwd=tmp_path,
            )
        procs.append((proc, err_path))
        hub = HubProcess(proc, _read_port(proc, out_path), token, rf, out_path, err_path)
        hub.out_pos = out_path.read_bytes().index(b"\n") + 1  # skip the port line
        return hub

    yield _start
    for proc, err_path in procs:
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        err = err_path.read_text(errors="replace")
        assert "Sanitizer" not in err, err


@pytest.fixture
def hub_process(start_hub) -> HubProcess:
    return start_hub()


@pytest.fixture
def rf_path():
    """Short per-test RF socket path (sun_path is limited to ~107 bytes, tmp_path can exceed it)."""
    d = tempfile.mkdtemp(prefix="hprf")
    yield os.path.join(d, "rf.sock")
    shutil.rmtree(d, ignore_errors=True)


@pytest.fixture
def hub_rf(start_hub, rf_path) -> HubProcess:
    """hub --sim --rf <rf_path> on an ephemeral ws port; `.rf` is the socket path."""
    return start_hub("--rf", rf_path)


@pytest.fixture
def run_remote(built_binaries):
    """run_remote(rf, *args, timeout=10) -> CompletedProcess (text stdout/stderr) of remote_sim."""

    def _run(rf: str, *args: str, timeout: float = 10.0) -> subprocess.CompletedProcess:
        return subprocess.run(
            [str(built_binaries / "remote_sim"), "--rf", rf, *args],
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
        )

    return _run


class RemoteDump:
    """A running `remote_sim --dump-page`: wait_attached() then result()."""

    def __init__(self, proc: subprocess.Popen):
        self.proc = proc

    def wait_attached(self, timeout: float = 5.0) -> None:
        assert self.proc.stderr is not None
        fd = self.proc.stderr.fileno()
        deadline = time.monotonic() + timeout
        buf = b""
        while b"attached" not in buf:
            left = deadline - time.monotonic()
            assert left > 0, f"remote_sim never attached; stderr={buf!r}"
            if select.select([fd], [], [], min(left, 0.1))[0]:
                chunk = os.read(fd, 256)
                assert chunk, f"remote_sim exited early; stderr={buf!r}"
                buf += chunk

    def result(self, timeout: float = 10.0) -> tuple[int, str]:
        """(exit code, stdout) once the process exits."""
        out, _ = self.proc.communicate(timeout=timeout)
        return self.proc.returncode, out.decode()


@pytest.fixture
def start_remote_dump(built_binaries):
    """start_remote_dump(rf, timeout_ms=5000) -> RemoteDump (not yet attached-checked)."""
    procs: list[subprocess.Popen] = []

    def _start(rf: str, timeout_ms: int = 5000) -> RemoteDump:
        proc = subprocess.Popen(
            [
                str(built_binaries / "remote_sim"),
                "--rf",
                rf,
                "--dump-page",
                "--timeout-ms",
                str(timeout_ms),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        procs.append(proc)
        return RemoteDump(proc)

    yield _start
    for proc in procs:
        if proc.poll() is None:
            proc.kill()
        proc.wait()


def rf_attach(rf: str, timeout: float = 5.0) -> socket.socket:
    """Raw RF peer for tests: connects and consumes the hub's 1-byte 'A' handshake."""
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    sock.settimeout(timeout)
    sock.connect(rf)
    assert sock.recv(8) == b"A", "hub refused the RF connection"
    return sock


# ---------------------------------------------------------------- Home Assistant helpers

HUB_ID = "sim-hub"
ACTIVITY_CONFIG = {
    "keys": {"1": {"type": "ha", "event": "menu"}},
    "activities": [
        {"name": "TV", "steps": [{"type": "none"}], "off": [{"type": "none"}]},
        {"name": "Music", "steps": [{"type": "none"}], "off": [{"type": "none"}]},
    ],
}


@pytest.fixture
def fast_client(monkeypatch):
    """Short reconnect/heartbeat timings so the HA tests stay quick."""
    from custom_components.ha_harmony.const import CLIENT_KWARGS

    monkeypatch.setitem(CLIENT_KWARGS, "backoff_min", 0.05)
    monkeypatch.setitem(CLIENT_KWARGS, "backoff_max", 0.2)
    monkeypatch.setitem(CLIENT_KWARGS, "heartbeat_interval", 5.0)
    monkeypatch.setitem(CLIENT_KWARGS, "request_timeout", 3.0)


async def push_config(url: str, token: str = TOKEN, config: dict | None = None) -> None:
    """Send set_config to a hub over a throwaway ws client."""
    import aiohttp

    async with aiohttp.ClientSession() as s, s.ws_connect(url) as ws:
        await ws.send_json({"v": 1, "id": 1, "t": "hello", "d": {"token": token}})
        assert (await ws.receive_json())["t"] == "hello_ack"
        assert (await ws.receive_json())["t"] == "state_update"
        cfg = ACTIVITY_CONFIG if config is None else config
        await ws.send_json({"v": 1, "id": 2, "t": "set_config", "d": {"config": cfg}})
        assert (await ws.receive_json())["d"]["ok"] is True


def make_entry(host: str, port: int, token: str = TOKEN):
    from pytest_homeassistant_custom_component.common import MockConfigEntry

    return MockConfigEntry(
        domain="ha_harmony",
        unique_id=HUB_ID,
        data={"host": host, "port": port, "token": token},
    )


class FakeHub:
    """In-test aiohttp ws server. `handler(ws, env)` answers each received envelope."""

    def __init__(self, handler):
        self.handler = handler
        self.connections = 0
        self.port = 0
        self._runner = None

    async def _ws(self, request):
        from aiohttp import web

        ws = web.WebSocketResponse()
        await ws.prepare(request)
        self.connections += 1
        async for msg in ws:
            await self.handler(ws, msg.json())
        return ws

    async def start(self):
        from aiohttp import web

        app = web.Application()
        app.router.add_get("/", self._ws)
        self._runner = web.AppRunner(app)
        await self._runner.setup()
        site = web.TCPSite(self._runner, "127.0.0.1", 0)
        await site.start()
        self.port = site._server.sockets[0].getsockname()[1]

    async def stop(self):
        await self._runner.cleanup()


@pytest.fixture
async def fake_hub():
    hubs: list[FakeHub] = []

    async def _make(handler) -> FakeHub:
        hub = FakeHub(handler)
        await hub.start()
        hubs.append(hub)
        return hub

    yield _make
    for hub in hubs:
        await hub.stop()


def page_tap_y(rows: int, row: int, *, titled: bool = False) -> int:
    """Pixel y in the middle of grid row `row`: the grid fills the 240x288 area below the 32 px status bar;
    a full-width row-0 label (the title) is consumed and the rest laid out as rows-1 rows (page-format.md)."""
    shift = 1 if titled and rows > 1 else 0
    n = rows - shift
    r = row - shift
    return 32 + (2 * r + 1) * 288 // (2 * n)
