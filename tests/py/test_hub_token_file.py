"""hub --token-file: bare or `token=` conf-style file; bad input exits 2 without echoing the value.

CONF_FIXTURES is shared with test_boot_hook.py so the hook's verdict and the daemon's exit status
cannot drift apart. Only token-related conf shapes belong there (bare tokens are daemon-only)."""

import subprocess
from pathlib import Path

import aiohttp
import pytest
from conftest import TOKEN, _read_port

SECRET = "s3cr3t-tok.en=="
LONG = "x" * 300
# (id, conf text, expected token or None when both hook and daemon must refuse)
CONF_FIXTURES = [
    ("plain", f"token={SECRET}\n", SECRET),
    ("crlf-daemon-only-skipped", None, None),
    ("no-trailing-newline", f"port=8099\ntoken={SECRET}", SECRET),
    ("key-order", f"port=8099\ntoken={SECRET}\nhub_id=x\n", SECRET),
    ("duplicate-last-wins", f"token=first-one\ntoken={SECRET}\n", SECRET),
    ("max-256", "token=" + "x" * 256 + "\n", "x" * 256),
    ("over-256", "token=" + "x" * 257 + "\n", None),
    ("over-long-token-secret", f"token={SECRET}{LONG}\n", None),
    ("empty-token", "token=\n", None),
    ("no-token", "port=8099\n", None),
    ("long-nontoken-line-skipped", "junk=" + "y" * 5000 + f"\ntoken={SECRET}\n", SECRET),
    ("long-nontoken-line-after", f"token={SECRET}\njunk=" + "y" * 5000, SECRET),
    ("duplicate-last-too-long", f"token={SECRET}\ntoken={LONG}\n", None),
    ("too-many-lines", "a=1\n" * 51 + f"token={SECRET}\n", None),
]
CONF_FIXTURES = [c for c in CONF_FIXTURES if c[1] is not None]


def _run(hub_bin: Path, tmp_path: Path, *args: str) -> subprocess.Popen:
    with open(tmp_path / "out", "wb") as o, open(tmp_path / "err", "wb") as e:
        return subprocess.Popen(
            [str(hub_bin), "--sim", "--port", "0", *args], stdout=o, stderr=e, cwd=tmp_path
        )


def _fail(built_binaries: Path, tmp_path: Path, *args: str) -> str:
    p = _run(built_binaries / "hub", tmp_path, *args)
    assert p.wait(timeout=5) == 2
    return (tmp_path / "out").read_text() + (tmp_path / "err").read_text()


async def _auth(port: int, token: str) -> str:
    async with aiohttp.ClientSession() as s, s.ws_connect(f"ws://127.0.0.1:{port}/") as ws:
        await ws.send_json({"v": 1, "id": 1, "t": "hello", "d": {"token": token}})
        return (await ws.receive_json(timeout=5))["t"]


async def _accepted(built_binaries, tmp_path, content, want):
    f = tmp_path / "tok"
    f.write_text(content)
    p = _run(built_binaries / "hub", tmp_path, "--token-file", str(f))
    try:
        port = _read_port(p, tmp_path / "out")
        assert await _auth(port, want) == "hello_ack"
        assert await _auth(port, want + "x") == "error"
        assert await _auth(port, TOKEN) == "error"
    finally:
        p.terminate()
        p.wait(timeout=5)


@pytest.mark.parametrize("content", [f"{SECRET}\n", f"{SECRET}\r\n", SECRET, f"\n{SECRET}\n\n"])
async def test_bare_token_file_value(built_binaries, tmp_path, content):
    await _accepted(built_binaries, tmp_path, content, SECRET)


@pytest.mark.parametrize(
    ("content", "want"),
    [(c, w) for _, c, w in CONF_FIXTURES if w],
    ids=[i for i, _, w in CONF_FIXTURES if w],
)
async def test_conf_fixture_accepted_with_exact_value(built_binaries, tmp_path, content, want):
    await _accepted(built_binaries, tmp_path, content, want)


async def test_conf_style_crlf(built_binaries, tmp_path):
    await _accepted(built_binaries, tmp_path, f"port=8099\r\ntoken={SECRET}\r\n", SECRET)


@pytest.mark.parametrize(
    ("content"),
    [c for _, c, w in CONF_FIXTURES if w is None],
    ids=[i for i, _, w in CONF_FIXTURES if w is None],
)
def test_conf_fixture_rejected(built_binaries, tmp_path, content):
    f = tmp_path / "tok"
    f.write_text(content)
    out = _fail(built_binaries, tmp_path, "--token-file", str(f))
    assert "required" in out
    assert SECRET not in out


@pytest.mark.parametrize(
    "content",
    [
        "",
        "\n\n",
        "a\nb\n",
        f"{SECRET}\nsecond\n",
        f"{SECRET}{LONG}\n",
        SECRET + "y" * 5000,
        f"{SECRET}\nport=1\n",
    ],
)
def test_bare_file_rejected_and_never_echoed(built_binaries, tmp_path, content):
    f = tmp_path / "tok"
    f.write_text(content)
    out = _fail(built_binaries, tmp_path, "--token-file", str(f))
    assert "required" in out
    assert SECRET not in out


def test_token_file_missing(built_binaries, tmp_path):
    assert "required" in _fail(built_binaries, tmp_path, "--token-file", str(tmp_path / "nope"))


def test_token_and_token_file_conflict(built_binaries, tmp_path):
    f = tmp_path / "tok"
    f.write_text(SECRET)
    out = _fail(built_binaries, tmp_path, "--token", TOKEN, "--token-file", str(f))
    assert "mutually exclusive" in out
    assert TOKEN not in out and SECRET not in out
