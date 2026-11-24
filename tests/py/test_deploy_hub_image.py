"""First install from a release image: deploy_hub `push --image F | --release X.Y.Z` (offline verify, then the same push)."""

import sys
from pathlib import Path

import pytest
from test_deploy_hub import make
from test_hub_image import DEV_HUB, pub_of, sign

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import deploy_hub as dh
import hub_image
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

BIN = b"\x7fELF-stripped-hub" * 40 + b"HPBUILDID=761;"


@pytest.fixture
def release(monkeypatch):
    """A throwaway key standing in for the release key (its private half is not in this repo)."""
    key = Ed25519PrivateKey.generate()
    monkeypatch.setattr(hub_image, "release_pubkey", lambda *a: pub_of(key))
    return key


def args(*extra):
    return ["push", "--yes", "--no-space-check", *extra]


def test_install_from_a_file_writes_the_binary_and_the_build_record(release, tmp_path):
    img = tmp_path / "hub-0.5.9.img"
    img.write_bytes(sign(release, BIN, build=761))
    d, hub, lines = make()
    assert dh.main(args("--image", str(img)), deployer=d) == 0
    assert hub.files[dh.REMOTE_HUB] == BIN  # the header is stripped
    assert hub.files[dh.REMOTE_BUILD] == b"761\n"
    assert not any(p.endswith(".new") for p in hub.files)
    text = "\n".join(lines)
    assert "build 761" in text and "md5" in text


def test_install_from_a_release_downloads_then_verifies(release, monkeypatch):
    got = []

    def fake_download(version):
        got.append(version)
        return sign(release, BIN, build=761)

    monkeypatch.setattr(hub_image, "download", fake_download)
    d, hub, _ = make()
    assert dh.main(args("--release", "0.5.9"), deployer=d) == 0
    assert got == ["0.5.9"] and hub.files[dh.REMOTE_HUB] == BIN


@pytest.mark.parametrize("why", ["dev key", "tampered", "wrong target", "truncated"])
def test_a_bad_image_is_refused_before_anything_is_written(release, tmp_path, capsys, why):
    good = sign(release, BIN)
    data = {
        "dev key": DEV_HUB,
        "tampered": good[:-1] + b"\x00",
        "wrong target": sign(release, BIN, target=2),
        "truncated": good[:-10],
    }[why]
    img = tmp_path / "x.img"
    img.write_bytes(data)
    d, hub, _ = make()
    assert dh.main(args("--image", str(img)), deployer=d) == 1
    assert "ERROR" in capsys.readouterr().err
    assert hub.files == {} and not any(
        c[0] != "uname" for c in hub.log
    )  # not even a connection's write


def test_exactly_one_source_is_required(capsys):
    for extra in ([], ["--hub", "a", "--image", "b"], ["--image", "b", "--release", "0.5.9"]):
        with pytest.raises(SystemExit):
            dh.main(args(*extra), deployer=make()[0])
