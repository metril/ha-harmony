"""tools/ota_sign.c: keygen, sign, verify, pubheader; and the release-build refusal of the dev key."""

import os
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
CC = shutil.which("cc") or shutil.which("gcc")


@pytest.fixture(scope="session")
def tool(tmp_path_factory):
    out = tmp_path_factory.mktemp("ota_tool") / "ota_sign"
    subprocess.run(
        [CC, "-O1", "-std=c11", "-w", "-I", "common/ota", "-I", "vendor/tweetnacl",
         "-DOTA_PUBKEY_HEADER=\"ota_pubkey_dev.h\"", "-o", str(out), "tools/ota_sign.c",
         "common/ota/ota_image.c", "vendor/tweetnacl/tweetnacl.c"],
        cwd=ROOT, check=True, capture_output=True,
    )
    return out


def run(tool, *args, home=None, ok=True):
    env = dict(os.environ, HOME=str(home)) if home else os.environ
    r = subprocess.run([str(tool), *map(str, args)], capture_output=True, text=True, env=env, check=False)
    assert (r.returncode == 0) == ok, r.stderr + r.stdout
    return r


def test_keygen_default_path_mode_and_no_overwrite(tool, tmp_path):
    r = run(tool, "keygen", home=tmp_path)
    key = tmp_path / ".config/ha-harmony/ota_ed25519"
    assert key.is_file() and (key.stat().st_mode & 0o777) == 0o600
    assert len(key.read_text().strip()) == 64
    pub = run(tool, "pubkey", home=tmp_path).stdout.strip()
    assert len(pub) == 64 and pub in r.stdout
    again = run(tool, "keygen", home=tmp_path, ok=False)
    assert "refusing" in again.stderr
    assert run(tool, "pubkey", home=tmp_path).stdout.strip() == pub  # untouched


def test_sign_verify_roundtrip_and_rejections(tool, tmp_path):
    run(tool, "keygen", home=tmp_path)
    payload = tmp_path / "hub.bin"
    payload.write_bytes(os.urandom(5000))
    img = tmp_path / "hub.img"
    run(tool, "sign", "-t", "hub", "-b", 7, payload, img, home=tmp_path)
    assert img.stat().st_size == 144 + 5000
    pub = run(tool, "pubkey", home=tmp_path).stdout.strip()
    ok = run(tool, "verify", "-p", pub, img)
    assert "target=hub build=7" in ok.stdout
    run(tool, "verify", "-t", "hub", "-m", 7, "-p", pub, img)
    assert "downgrade" in run(tool, "verify", "-m", 8, "-p", pub, img, ok=False).stderr
    assert "another device" in run(tool, "verify", "-t", "remote", "-p", pub, img, ok=False).stderr
    run(tool, "verify", "-k", tmp_path / ".config/ha-harmony/ota_ed25519", img)
    # wrong key
    assert "signature" in run(tool, "verify", "-p", "00" * 32, img, ok=False).stderr
    # tampered payload, truncated file
    data = bytearray(img.read_bytes())
    data[200] ^= 1
    bad = tmp_path / "bad.img"
    bad.write_bytes(data)
    assert "digest" in run(tool, "verify", "-p", pub, bad, ok=False).stderr
    bad.write_bytes(bytes(data[:-1]))
    assert run(tool, "verify", "-p", pub, bad, ok=False).returncode == 1
    img.write_bytes(img.read_bytes()[:100])
    assert "truncated" in run(tool, "verify", "-p", pub, img, ok=False).stderr


def test_sign_needs_key_and_args(tool, tmp_path):
    p = tmp_path / "p"
    p.write_bytes(b"x")
    assert "private key" in run(tool, "sign", "-t", "hub", "-b", 1, p, tmp_path / "o", home=tmp_path, ok=False).stderr
    run(tool, "sign", "--dev", "-t", "remote", "-b", 1, p, tmp_path / "o", home=tmp_path)
    run(tool, "sign", "-t", "hub", p, tmp_path / "o", home=tmp_path, ok=False)


def test_pubheader_dev_matches_committed_and_generated_is_not_dev(tool, tmp_path):
    dev = run(tool, "pubheader", "--dev").stdout
    committed = (ROOT / "common/ota/ota_pubkey_dev.h").read_text()
    pub = run(tool, "pubkey", "--dev").stdout.strip()
    assert "OTA_PUBKEY_IS_DEV 1" in dev and "OTA_PUBKEY_IS_DEV 1" in committed
    flat = lambda s: "".join(s.split())
    assert flat(dev.split("OTA_PUBKEY_INIT")[1].split("#endif")[0].replace("\\", "")) == flat(
        committed.split("OTA_PUBKEY_INIT")[1].split("#endif")[0].replace("\\", "")
    )
    assert pub == "79b5562e8fe654f94078b112e8a98ba7901f853ae695bed7e0e3910bad049664"
    run(tool, "keygen", home=tmp_path)
    gen = run(tool, "pubheader", home=tmp_path).stdout
    assert "OTA_PUBKEY_IS_DEV 0" in gen


@pytest.mark.skipif(not shutil.which("cmake"), reason="cmake missing")
def test_release_build_refuses_dev_key(tmp_path, tool):
    base = ["cmake", "-S", str(ROOT), "-B", str(tmp_path / "b")]
    r = subprocess.run([*base, "-DCMAKE_BUILD_TYPE=Release", "-DHP_BUILD_TESTS=OFF"], capture_output=True, text=True, check=False)
    assert r.returncode != 0 and "DEV/TEST public key" in r.stderr
    allow = subprocess.run([*base, "-DCMAKE_BUILD_TYPE=RelWithDebInfo", "-DHP_BUILD_TESTS=OFF", "-DHP_ALLOW_DEV_OTA_KEY=ON"],
                           capture_output=True, text=True, check=False)
    assert allow.returncode == 0 and "DEV/TEST OTA PUBLIC KEY" in allow.stderr
    r = subprocess.run([*base, "-DCMAKE_BUILD_TYPE=RelWithDebInfo", "-DHP_BUILD_TESTS=OFF", "-DHP_ALLOW_DEV_OTA_KEY=OFF"],
                       capture_output=True, text=True, check=False)
    assert r.returncode != 0
    gen = tmp_path / "gen.h"
    gen.write_text(run(tool, "pubheader", "--dev").stdout.replace("IS_DEV 1", "IS_DEV 0"))
    r = subprocess.run([*base, "-DCMAKE_BUILD_TYPE=Release", "-DHP_BUILD_TESTS=OFF", f"-DHP_OTA_PUBKEY_HEADER={gen}"],
                       capture_output=True, text=True, check=False)
    assert r.returncode == 0, r.stderr
