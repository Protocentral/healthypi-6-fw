# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT

"""The firmware bundle (zip): create, verify, and refuse.

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
        tmp_path / "hpi6-firmware-1.0.0.zip",
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
    one = fw.create(tmp_path / "one.zip", specs(), **kw)
    two = fw.create(tmp_path / "two.zip", specs(), **kw)
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
            tmp_path / "x.zip",
            [fw.ImageSpec("m7", tmp_path / "nope.bin", "1.0.0", "mcumgr-img")],
            release="1.0.0", hw_rev=["v5"], key_path=key, created="t",
        )


# ------------------------------------------------------------------ verify --


def test_digests_checked_without_a_key(tmp_path, bundle_path):
    """A missing key must not mean a missing check. Corruption in transit is the
    ordinary failure; the signature only catches a deliberate one."""
    bad = _rebuild(bundle_path, tmp_path / "bad.zip",
                   lambda m: m.__setitem__("m4.bin", b"\x00" * 2048))
    with pytest.raises(fw.BundleError, match="digest mismatch"):
        fw.Bundle(bad).verify(None)


def test_truncated_payload_rejected(tmp_path, bundle_path, key):
    bad = _rebuild(bundle_path, tmp_path / "trunc.zip",
                   lambda m: m.__setitem__("m7.bin", m["m7.bin"][:-16]))
    with pytest.raises(fw.BundleError, match="digest mismatch"):
        fw.Bundle(bad).verify(key)


def test_edited_manifest_fails_the_signature(tmp_path, bundle_path, key):
    def bump(members):
        d = json.loads(members["manifest.json"])
        d["release"] = "9.9.9"
        members["manifest.json"] = json.dumps(d, indent=2, sort_keys=True).encode()

    bad = _rebuild(bundle_path, tmp_path / "edited.zip", bump)
    with pytest.raises(fw.BundleError, match="signature does NOT verify"):
        fw.Bundle(bad).verify(key)


def test_wrong_key_rejected(bundle_path, other_key):
    with pytest.raises(fw.BundleError, match="signature does NOT verify"):
        fw.Bundle(bundle_path).verify(other_key)


def test_extension_is_not_significant(bundle_path, key, tmp_path):
    # Bundles were named .hpifw before 2026-10; the same zip still opens.
    legacy = tmp_path / "hpi6-1.0.0.hpifw"
    legacy.write_bytes(bundle_path.read_bytes())
    fw.Bundle(legacy).verify(key)


def test_not_a_zip(tmp_path):
    junk = tmp_path / "junk.zip"
    junk.write_bytes(b"not a zip")
    with pytest.raises(fw.BundleError, match="not a firmware bundle"):
        fw.Bundle(junk)


def test_missing_file_is_not_a_traceback(tmp_path):
    with pytest.raises(fw.BundleError, match="no such file"):
        fw.Bundle(tmp_path / "absent.zip")


def test_unknown_format_version(tmp_path, bundle_path):
    def future(members):
        d = json.loads(members["manifest.json"])
        d["format"] = 99
        members["manifest.json"] = json.dumps(d).encode()

    bad = _rebuild(bundle_path, tmp_path / "future.zip", future)
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

    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.hazmat.primitives.asymmetric.utils import (
        Prehashed,
        encode_dss_signature,
    )

    digest = hashlib.sha256(b"x").digest()
    sig = fw.sign_digest_raw(digest, key)
    assert len(sig) == 64
    # Prove the layout rather than sniff it: split as r||s, re-encode, verify.
    # (Checking sig[0] != 0x30 for "not DER" failed 1 run in 256, whenever r
    # happened to start with 0x30.)
    r = int.from_bytes(sig[:32], "big")
    s = int.from_bytes(sig[32:], "big")
    priv = serialization.load_pem_private_key(key.read_bytes(), password=None)
    priv.public_key().verify(
        encode_dss_signature(r, s), digest, ec.ECDSA(Prehashed(hashes.SHA256()))
    )


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


# --- refusals made before anything is written -------------------------------


def test_a_dev_build_is_refused_not_reported_up_to_date():
    """A unit on `scripts/build.sh m7` has no img group and no M4-update service.
    Found 2026-09-30: --dry-run against one said "nothing to do"."""
    from healthypi.fw.update import unsupported

    why = unsupported({"m7": False, "m4": False}, ["m4", "m7"])
    assert why and "no MCUboot image group" in why and "no M4-update service" in why
    assert "scripts/flash.sh factory" in why


def test_a_signed_build_is_supported():
    from healthypi.fw.update import unsupported

    assert unsupported({"m7": True, "m4": True}, ["m4", "m7"]) is None


def test_support_is_judged_only_for_what_will_be_written():
    """--only m4 must not be refused for a missing img group."""
    from healthypi.fw.update import unsupported

    assert unsupported({"m7": False, "m4": True}, ["m4"]) is None
    assert "no M4-update service" in unsupported({"m7": True, "m4": False}, ["m4"])


@pytest.mark.parametrize(
    "installed, bundle, refused",
    [
        ("1.0.1", "1.0.0", True),   # downgrade: MCUboot would refuse after a 40 s upload
        ("1.0.1", "1.0.1", False),  # same: left to the version skip / --force
        ("1.0.1", "1.0.2", False),
        ("1.0.1-dev", "1.0.1", False),  # suffixes are not an ordering
        ("", "1.0.0", False),       # unknown installed version: let MCUboot decide
    ],
)
def test_m7_downgrade_is_refused_up_front(installed, bundle, refused):
    from healthypi.fw.update import m7_downgrade

    msg = m7_downgrade(installed, bundle)
    assert bool(msg) is refused
    if refused:
        assert "fw recover" in msg


# --- an extracted bundle (a browser unpacked the .zip) ----------------------


def _extract(bundle_path, dest):
    import zipfile

    dest.mkdir()
    with zipfile.ZipFile(bundle_path) as zf:
        zf.extractall(dest)
    return dest


def test_an_extracted_folder_is_a_bundle(bundle_path, key, tmp_path):
    """Safari's default "Open safe files after downloading" unpacks a .zip.
    The folder carries the same manifest, signature and images, so it must
    verify exactly as the zip does."""
    folder = _extract(bundle_path, tmp_path / "hpi6-firmware-1.0.0")
    b = fw.Bundle(folder)
    b.verify(key)
    assert b.read_image("m7") == fw.Bundle(bundle_path).read_image("m7")
    assert b.image_sig("m4") == fw.Bundle(bundle_path).image_sig("m4")


def test_a_tampered_file_in_the_folder_fails_its_digest(bundle_path, tmp_path):
    folder = _extract(bundle_path, tmp_path / "x")
    (folder / "m4.bin").write_bytes(b"\x00" * 2048)
    with pytest.raises(fw.BundleError, match="digest mismatch"):
        fw.Bundle(folder).verify(None)


def test_a_missing_file_in_the_folder_is_named(bundle_path, tmp_path):
    folder = _extract(bundle_path, tmp_path / "x")
    (folder / "m7.bin").unlink()
    with pytest.raises(fw.BundleError, match="m7.bin is missing"):
        fw.Bundle(folder).read_image("m7")


def test_a_folder_manifest_cannot_reach_outside_the_folder(bundle_path, tmp_path):
    """Member names come from manifest.json. In folder form they are paths, so
    one must not be able to point the reader at a file elsewhere."""
    import json

    (tmp_path / "secret.bin").write_bytes(b"not part of the bundle")
    folder = _extract(bundle_path, tmp_path / "x")
    man = json.loads((folder / "manifest.json").read_text())
    man["images"]["m7"]["file"] = "../secret.bin"
    (folder / "manifest.json").write_text(json.dumps(man))
    with pytest.raises(fw.BundleError, match="outside the bundle"):
        fw.Bundle(folder).read_image("m7")
