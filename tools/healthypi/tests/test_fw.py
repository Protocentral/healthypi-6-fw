# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT

"""The .hpifw bundle: create, verify, and refuse.

These cover the offline half of the update path -- the half that can be tested
without a board. What they are really guarding is the *negative* cases: a
bundle that verifies when it should not is how a wrong image reaches flash.

The device half (``healthypi.fw.update``) needs hardware and lives in the bench
acceptance run, not here.
"""

from __future__ import annotations

import json
import zipfile

import pytest

from healthypi import fw

crypto = pytest.importorskip("cryptography", reason="signing needs cryptography")


@pytest.fixture(scope="module")
def key(tmp_path_factory):
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import ec

    path = tmp_path_factory.mktemp("keys") / "test_ec256.pem"
    k = ec.generate_private_key(ec.SECP256R1())
    path.write_bytes(
        k.private_bytes(
            serialization.Encoding.PEM,
            serialization.PrivateFormat.PKCS8,
            serialization.NoEncryption(),
        )
    )
    return path


@pytest.fixture(scope="module")
def other_key(tmp_path_factory):
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import ec

    path = tmp_path_factory.mktemp("keys2") / "other_ec256.pem"
    k = ec.generate_private_key(ec.SECP256R1())
    path.write_bytes(
        k.private_bytes(
            serialization.Encoding.PEM,
            serialization.PrivateFormat.PKCS8,
            serialization.NoEncryption(),
        )
    )
    return path


@pytest.fixture
def bundle_path(tmp_path, key):
    m7 = tmp_path / "m7.bin"
    m7.write_bytes(b"\xa5" * 4096)
    m4 = tmp_path / "m4.bin"
    m4.write_bytes(b"\x5a" * 2048)
    return fw.create(
        tmp_path / "hpi6-1.0.0.hpifw",
        [
            fw.ImageSpec("m7", m7, "1.0.0", "mcumgr-img"),
            fw.ImageSpec("m4", m4, "1.0.0", "hpi-g64", sign=True),
        ],
        release="1.0.0",
        hw_rev=["v5"],
        key_path=key,
        created="2026-08-03T00:00:00Z",
    )


def _rebuild(src, dst, mutate):
    """Copy a bundle, mutating its members. The zip is DEFLATE-compressed, so a
    byte-level patch of the container does not reach the payload -- rewriting is
    the only way to actually tamper with one."""
    zf = zipfile.ZipFile(src)
    members = {n: zf.read(n) for n in zf.namelist()}
    mutate(members)
    with zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED) as out:
        for name, blob in members.items():
            out.writestr(name, blob)
    return dst


# ------------------------------------------------------------------ create --


def test_roundtrip(bundle_path, key):
    b = fw.Bundle(bundle_path)
    b.verify(key)
    assert b.release == "1.0.0"
    assert b.hw_rev == ["v5"]
    assert set(b.images()) == {"m7", "m4"}
    assert len(b.read_image("m7")) == 4096
    assert b.image_sig("m4") is not None and len(b.image_sig("m4")) == 64
    assert b.image_sig("m7") is None  # MCUboot signs the M7 itself


def test_manifest_is_reproducible(tmp_path, key):
    """Same inputs -> same manifest. `created` is a parameter, not the clock,
    precisely so a release can be rebuilt and compared."""
    m7 = tmp_path / "a.bin"
    m7.write_bytes(b"\x01" * 512)
    specs = lambda: [fw.ImageSpec("m7", m7, "1.0.0", "mcumgr-img")]  # noqa: E731
    kw = dict(release="1.0.0", hw_rev=["v5"], key_path=key,
              created="2026-08-03T00:00:00Z")
    one = fw.create(tmp_path / "one.hpifw", specs(), **kw)
    two = fw.create(tmp_path / "two.hpifw", specs(), **kw)
    assert (zipfile.ZipFile(one).read("manifest.json")
            == zipfile.ZipFile(two).read("manifest.json"))


def test_apply_order_puts_m7_last():
    """The M7 applies the other images, so it must not be replaced first."""
    assert fw.APPLY_ORDER[-1] == "m7"
    assert set(fw.APPLY_ORDER) == {"esp32c6", "m4", "m7"}


def test_describe_lists_every_image(bundle_path):
    text = fw.Bundle(bundle_path).describe()
    assert "m4" in text and "m7" in text
    assert "signed" in text  # the M4 entry carries a detached signature


def test_missing_source_image_is_named(tmp_path, key):
    with pytest.raises(fw.BundleError, match="not found"):
        fw.create(
            tmp_path / "x.hpifw",
            [fw.ImageSpec("m7", tmp_path / "nope.bin", "1.0.0", "mcumgr-img")],
            release="1.0.0", hw_rev=["v5"], key_path=key, created="t",
        )


# ------------------------------------------------------------------ verify --


def test_digests_checked_without_a_key(tmp_path, bundle_path):
    """A missing key must not mean a missing check. Corruption in transit is the
    ordinary failure; the signature only catches a deliberate one."""
    bad = _rebuild(bundle_path, tmp_path / "bad.hpifw",
                   lambda m: m.__setitem__("m4.bin", b"\x00" * 2048))
    with pytest.raises(fw.BundleError, match="digest mismatch"):
        fw.Bundle(bad).verify(None)


def test_truncated_payload_rejected(tmp_path, bundle_path, key):
    bad = _rebuild(bundle_path, tmp_path / "trunc.hpifw",
                   lambda m: m.__setitem__("m7.bin", m["m7.bin"][:-16]))
    with pytest.raises(fw.BundleError, match="digest mismatch"):
        fw.Bundle(bad).verify(key)


def test_edited_manifest_fails_the_signature(tmp_path, bundle_path, key):
    def bump(members):
        d = json.loads(members["manifest.json"])
        d["release"] = "9.9.9"
        members["manifest.json"] = json.dumps(d, indent=2, sort_keys=True).encode()

    bad = _rebuild(bundle_path, tmp_path / "edited.hpifw", bump)
    with pytest.raises(fw.BundleError, match="signature does NOT verify"):
        fw.Bundle(bad).verify(key)


def test_wrong_key_rejected(bundle_path, other_key):
    with pytest.raises(fw.BundleError, match="signature does NOT verify"):
        fw.Bundle(bundle_path).verify(other_key)


def test_not_a_zip(tmp_path):
    junk = tmp_path / "junk.hpifw"
    junk.write_bytes(b"not a zip")
    with pytest.raises(fw.BundleError, match="not a .hpifw"):
        fw.Bundle(junk)


def test_missing_file_is_not_a_traceback(tmp_path):
    with pytest.raises(fw.BundleError, match="no such file"):
        fw.Bundle(tmp_path / "absent.hpifw")


def test_unknown_format_version(tmp_path, bundle_path):
    def future(members):
        d = json.loads(members["manifest.json"])
        d["format"] = 99
        members["manifest.json"] = json.dumps(d).encode()

    bad = _rebuild(bundle_path, tmp_path / "future.hpifw", future)
    with pytest.raises(fw.BundleError, match="understands"):
        fw.Bundle(bad)


def test_absent_image_named_clearly(bundle_path):
    with pytest.raises(fw.BundleError, match="no esp32c6 image"):
        fw.Bundle(bundle_path).read_image("esp32c6")


# -------------------------------------------------------------------- keys --


def test_signature_is_raw_not_der(key):
    """The device verifies with psa_verify_hash(), which takes raw r||s. A DER
    signature is variable-length and would be rejected on the device, after the
    upload."""
    import hashlib

    sig = fw.sign_digest_raw(hashlib.sha256(b"x").digest(), key)
    assert len(sig) == 64
    assert sig[0] != 0x30  # a DER SEQUENCE would start here


def test_verify_accepts_a_public_pem(tmp_path, key):
    import hashlib

    from cryptography.hazmat.primitives import serialization

    priv = serialization.load_pem_private_key(key.read_bytes(), password=None)
    pub = tmp_path / "pub.pem"
    pub.write_bytes(
        priv.public_key().public_bytes(
            serialization.Encoding.PEM,
            serialization.PublicFormat.SubjectPublicKeyInfo,
        )
    )
    digest = hashlib.sha256(b"payload").digest()
    assert fw.verify_digest_raw(digest, fw.sign_digest_raw(digest, key), pub)


def test_wrong_curve_refused(tmp_path):
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import ec

    from healthypi.fw.keys import KeyError_, load_private_key

    path = tmp_path / "p384.pem"
    k = ec.generate_private_key(ec.SECP384R1())
    path.write_bytes(
        k.private_bytes(
            serialization.Encoding.PEM,
            serialization.PrivateFormat.PKCS8,
            serialization.NoEncryption(),
        )
    )
    # Caught here rather than at commit time on the device, where a 96-byte
    # signature fails long after the release was cut.
    with pytest.raises(KeyError_, match="P-256"):
        load_private_key(path)
