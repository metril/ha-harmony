"""hub --token-file: bare or `token=` conf-style file; bad input exits 2 without echoing the value."""

import subprocess
from pathlib import Path

import pytest
from conftest import TOKEN, _read_port

SECRET = "s3cr3t-tok.en=="


def _run(hub_bin: Path, tmp_path: Path, *args: str) -> subprocess.Popen:
    out = tmp_path / "out"
    with open(out, "wb") as o, open(tmp_path / "err", "wb") as e:
        return subprocess.Popen([str(hub_bin), "--sim", "--port", "0", *args], stdout=o, stderr=e, cwd=tmp_path)


def _fail(built_binaries: Path, tmp_path: Path, *args: str) -> str:
    p = _run(built_binaries / "hub", tmp_path, *args)
    assert p.wait(timeout=5) == 2
    err = (tmp_path / "err").read_text()
    assert SECRET not in err
    assert (tmp_path / "out").read_text() == ""
    return err


@pytest.mark.parametrize(
    "content", [f"{SECRET}\n", f"{SECRET}\r\n", f"{SECRET}", f"port=8099\ntoken={SECRET}\r\nhub_id=x\n"]
)
def test_token_file_accepted(built_binaries, tmp_path, content):
    f = tmp_path / "tok"
    f.write_text(content)
    p = _run(built_binaries / "hub", tmp_path, "--token-file", str(f))
    try:
        _read_port(p, tmp_path / "out")
    finally:
        p.terminate()
        p.wait(timeout=5)


@pytest.mark.parametrize(
    "content", ["", "\n\n", "token=\n", "port=8099\n", "a\nb\n", "x" * 257 + "\n", "token=" + "x" * 257 + "\n", "y" * 5000]
)
def test_token_file_rejected(built_binaries, tmp_path, content):
    f = tmp_path / "tok"
    f.write_text(content)
    assert "required" in _fail(built_binaries, tmp_path, "--token-file", str(f))


def test_token_file_max_length_ok(built_binaries, tmp_path):
    f = tmp_path / "tok"
    f.write_text("x" * 256 + "\n")
    p = _run(built_binaries / "hub", tmp_path, "--token-file", str(f))
    try:
        _read_port(p, tmp_path / "out")
    finally:
        p.terminate()
        p.wait(timeout=5)


def test_token_file_missing(built_binaries, tmp_path):
    assert "required" in _fail(built_binaries, tmp_path, "--token-file", str(tmp_path / "nope"))


def test_token_and_token_file_conflict(built_binaries, tmp_path):
    f = tmp_path / "tok"
    f.write_text(SECRET)
    err = _fail(built_binaries, tmp_path, "--token", TOKEN, "--token-file", str(f))
    assert "mutually exclusive" in err
    assert TOKEN not in err
