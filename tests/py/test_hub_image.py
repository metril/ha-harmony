"""Offline verification of a signed hub image (first install from a release): tools/hub_image.py.

No private release key exists here, so acceptance of a "release-signed" image is shown with a throwaway key whose public
half is passed in; against the committed release key the dev-signed vector (made by `ota_sign sign --dev`) is refused."""

import hashlib
import struct
import sys
from pathlib import Path

import pytest
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import hub_image as hi

# `ota_sign sign --dev -t hub -b 7` over b"ha-harmony vector payload" (and -t remote), made with the C tool.
DEV_PUB = bytes.fromhex("79b5562e8fe654f94078b112e8a98ba7901f853ae695bed7e0e3910bad049664")
PAYLOAD = b"ha-harmony vector payload"
DEV_HUB = bytes.fromhex(
    "484f54410101000007000000190000001dc877611f197110b287bbe26ed4efe4246750f1ce57b7afe73f16fdad8636384f53efdbd38a978b6ec2bee2d5b3535677a25d6a01774cff3aca7145142c821111c9e1cbbc2cb7ea2cde5602e48ccd36db6b38efbf6fcec73055604ae22aaada03f3aec830867129fb90e6853731188b8bb8cff7e4a9f91073542d7460bea90068612d6861726d6f6e7920766563746f72207061796c6f6164"
)
DEV_REMOTE = bytes.fromhex(
    "484f54410102000007000000190000001dc877611f197110b287bbe26ed4efe4246750f1ce57b7afe73f16fdad8636384f53efdbd38a978b6ec2bee2d5b3535677a25d6a01774cff3aca7145142c8211d73738d858303210c869ea0d3a902afc8fbb0cc9d38a9befd7f0e8a2b13093e174dc56f0dc06cd5af4b91c847ace4ee4d6e9fdcf64c455df560b239612ec220968612d6861726d6f6e7920766563746f72207061796c6f6164"
)


def sign(key: Ed25519PrivateKey, payload: bytes, build=9, target=1) -> bytes:
    head = struct.pack("<4sBBHII", b"HOTA", 1, target, 0, build, len(payload))
    head += hashlib.sha512(payload).digest()
    return head + key.sign(head) + payload


@pytest.fixture
def key():
    return Ed25519PrivateKey.generate()


def pub_of(key) -> bytes:
    return key.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)


def test_the_pure_python_verify_agrees_with_the_c_tool_on_a_vector():
    img = hi.verify_image(DEV_HUB, DEV_PUB)
    assert (img.build, img.payload) == (7, PAYLOAD)


def test_committed_release_key_is_a_public_release_key():
    pk = hi.release_pubkey()
    assert len(pk) == 32 and pk != DEV_PUB
    text = hi.RELEASE_HEADER.read_text()
    assert "OTA_PUBKEY_IS_DEV 0" in text and "seed" not in text.lower()


def test_accepts_an_image_signed_by_the_expected_key(key):
    img = hi.verify_image(sign(key, b"hub-binary" * 50, build=123), pub_of(key))
    assert img.build == 123 and img.payload == b"hub-binary" * 50


def test_rejects_the_dev_key_against_the_release_key():
    with pytest.raises(hi.ImageError, match="signature"):
        hi.verify_image(DEV_HUB, hi.release_pubkey())


def test_rejects_an_image_signed_by_another_key(key):
    other = Ed25519PrivateKey.generate()
    with pytest.raises(hi.ImageError, match="signature"):
        hi.verify_image(sign(other, b"x" * 100), pub_of(key))


@pytest.mark.parametrize(
    "where", [5, 9, 20, 60, 85, 120, 150]
)  # header fields, digest, signature, payload
def test_rejects_a_tampered_image(key, where):
    data = bytearray(sign(key, b"p" * 200))
    data[where] ^= 1
    with pytest.raises(hi.ImageError):
        hi.verify_image(bytes(data), pub_of(key))


def test_rejects_the_wrong_target(key):
    with pytest.raises(hi.ImageError, match="not for the hub"):
        hi.verify_image(sign(key, b"x" * 100, target=2), pub_of(key))
    with pytest.raises(hi.ImageError, match="not for the hub"):
        hi.verify_image(DEV_REMOTE, DEV_PUB)


@pytest.mark.parametrize("cut", [1, 50])
def test_rejects_a_truncated_or_padded_image(key, cut):
    data = sign(key, b"p" * 200)
    with pytest.raises(hi.ImageError, match="size"):
        hi.verify_image(data[:-cut], pub_of(key))
    with pytest.raises(hi.ImageError, match="size"):
        hi.verify_image(data + b"\0" * cut, pub_of(key))


@pytest.mark.parametrize("data", [b"", b"HOTA", b"MZ" + bytes(200)])
def test_rejects_junk(data):
    with pytest.raises(hi.ImageError):
        hi.verify_image(data, DEV_PUB)


def test_release_url_and_version_check():
    assert hi.release_url("0.5.9") == (
        "https://github.com/metril/ha-harmony/releases/download/v0.5.9/hub-0.5.9.img"
    )
    for bad in ("0.5", "v0.5.9", "0.5.9/../x", ""):
        with pytest.raises(hi.ImageError):
            hi.release_url(bad)


def test_download_uses_the_injected_opener_and_caps_the_size():
    class Resp:
        def __init__(self, data):
            self.data = data

        def read(self, n=-1):
            return self.data[:n]

        def __enter__(self):
            return self

        def __exit__(self, *a):
            return False

    urls = []

    def opener(url, timeout):
        urls.append(url)
        return Resp(b"abc")

    assert hi.download("0.5.9", opener=opener) == b"abc"
    assert urls == [hi.release_url("0.5.9")]
    with pytest.raises(hi.ImageError, match="too large"):
        hi.download("0.5.9", opener=lambda u, timeout: Resp(bytes(hi.MAX_IMAGE + 1)))
