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


def test_apply_order_puts_the_c6_last():
    """Any M7 reset holds the C6 in reset, and a C6 reset before its new image
    has proved itself rolls it back -- so the C6 goes after every reset. The M4
    still precedes the M7: both share the one reset (two with an M7 install)."""
    assert fw.APPLY_ORDER == ("m4", "m7", "esp32c6")


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


# --- the release key(s) shipped with the tool -------------------------------


def _public_half(private_pem, dest):
    from cryptography.hazmat.primitives import serialization

    priv = serialization.load_pem_private_key(private_pem.read_bytes(), password=None)
    dest.write_bytes(
        priv.public_key().public_bytes(
            serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo
        )
    )
    return dest


@pytest.fixture
def release_keys(tmp_path, monkeypatch):
    """Point the shipped-release-key directory at an empty temp dir."""
    from healthypi.fw import keys as K

    d = tmp_path / "release_keys"
    d.mkdir()
    monkeypatch.setattr(K, "RELEASE_KEY_DIR", d)
    return d


def test_no_pubkey_verifies_against_the_shipped_release_key(bundle_path, key, release_keys):
    _public_half(key, release_keys / "hp6_release_ec256.pub.pem")
    msg = fw.Bundle(bundle_path).authenticate(None)
    assert "release key" in msg


def test_a_bundle_signed_by_another_key_is_refused_without_pubkey(
    bundle_path, other_key, release_keys
):
    """The point of shipping the key: without --pubkey, a repackaged bundle used
    to pass on digests alone."""
    _public_half(other_key, release_keys / "hp6_release_ec256.pub.pem")
    with pytest.raises(fw.BundleError, match="pass --pubkey"):
        fw.Bundle(bundle_path).authenticate(None)


def test_either_of_two_release_keys_is_accepted(bundle_path, key, other_key, release_keys):
    """Primary + backup: a bundle signed by the backup key still verifies."""
    _public_half(other_key, release_keys / "a_primary.pub.pem")
    _public_half(key, release_keys / "b_backup.pub.pem")
    assert "release key" in fw.Bundle(bundle_path).authenticate(None)


def test_no_shipped_key_checks_digests_and_says_so(bundle_path, release_keys):
    msg = fw.Bundle(bundle_path).authenticate(None)
    assert "NOT checked" in msg


def test_explicit_pubkey_wins_over_the_shipped_key(bundle_path, key, other_key, release_keys):
    """An owner who re-keyed their unit, or the bench with a dev key."""
    _public_half(other_key, release_keys / "hp6_release_ec256.pub.pem")
    assert "verified against" in fw.Bundle(bundle_path).authenticate(key)


def test_a_private_key_and_its_public_half_share_a_fingerprint(key, tmp_path):
    from healthypi.fw.keys import fingerprint

    pub = _public_half(key, tmp_path / "k.pub.pem")
    assert fingerprint(key) == fingerprint(pub)
    assert len(fingerprint(pub)) == 16


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


# --- the ESP32-C6 entry -------------------------------------------------------


def fake_c6_image(version="1.1.0", size=6000, *, chip=13,
                  project=b"healthybridge_esp32c6", desc_magic=0xABCD5432):
    """An ESP-IDF app image as far as the header and app descriptor go: the
    0xE9 image header (chip id at 12), one segment header, esp_app_desc_t at 32.
    Same layout healthybridge_esp32c6.bin has (checked against a real build)."""
    head = bytearray(24)
    head[0] = 0xE9
    head[1] = 1
    head[12:14] = chip.to_bytes(2, "little")
    seg = bytes(8)
    desc = bytearray(256)
    desc[0:4] = desc_magic.to_bytes(4, "little")
    desc[16:16 + len(version)] = version.encode()
    desc[48:48 + len(project)] = project
    desc[112:118] = b"v6.0.1"
    body = bytes(head) + seg + bytes(desc)
    # a deterministic, non-repeating payload so a misplaced chunk changes the hash
    filler = bytes((i * 7 + 3) & 0xFF for i in range(max(0, size - len(body))))
    return body + filler


@pytest.fixture
def c6_bundle(tmp_path, key):
    m7 = tmp_path / "m7.bin"
    m7.write_bytes(b"\xa5" * 4096)
    m4 = tmp_path / "m4.bin"
    m4.write_bytes(b"\x5a" * 2048)
    c6 = tmp_path / "c6.bin"
    c6.write_bytes(fake_c6_image("1.1.0"))
    return fw.create(
        tmp_path / "hpi6-firmware-1.0.1.zip",
        [
            fw.ImageSpec("m7", m7, "1.0.1", "mcumgr-img"),
            fw.ImageSpec("m4", m4, "1.0.1", "hpi-g64", sign=True),
            fw.ImageSpec("esp32c6", c6, "", "hpi-g64-esp", sign=True),
        ],
        release="1.0.1",
        hw_rev=["v5"],
        key_path=key,
        created="2026-11-12T00:00:00Z",
    )


def test_c6_entry_is_signed_and_versioned_from_the_image(c6_bundle, key):
    b = fw.Bundle(c6_bundle)
    b.verify(key)
    e = b.images()["esp32c6"]
    assert e["transport"] == "hpi-g64-esp"
    assert e["version"] == "1.1.0"            # read from the app descriptor
    assert len(b.image_sig("esp32c6")) == 64
    assert b.unusable("esp32c6") is None
    assert "esp32c6" in b.describe() and "signed" in b.describe()


def test_c6_signature_verifies_exactly_as_the_m7_checks_it(c6_bundle, key):
    """Byte-exact against fw_release_sig.c: psa_verify_hash(key, ECDSA(SHA-256),
    digest = SHA-256 of the image FILE, 64-byte raw r||s), with the key the M7
    compiles in -- the raw uncompressed point gen_m4_pubkey.py emits.

    Re-derived here from first principles with `cryptography`, not through
    healthypi's own verifier, in both encodings: raw r||s split and re-encoded
    as DER, and the DER form verified directly.
    """
    import hashlib

    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.hazmat.primitives.asymmetric.utils import (
        Prehashed,
        encode_dss_signature,
    )

    b = fw.Bundle(c6_bundle)
    image = b.read_image("esp32c6")
    digest = hashlib.sha256(image).digest()
    assert b.images()["esp32c6"]["sha256"] == digest.hex()   # `sha` sent at begin

    sig = b.image_sig("esp32c6")
    assert len(sig) == 64
    r, s = int.from_bytes(sig[:32], "big"), int.from_bytes(sig[32:], "big")
    n = 0xFFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551
    assert 0 < r < n and 0 < s < n

    priv = serialization.load_pem_private_key(key.read_bytes(), password=None)
    point = priv.public_key().public_bytes(
        serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint
    )
    assert len(point) == 65 and point[0] == 0x04          # what the M7 imports
    pub = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), point)

    der = encode_dss_signature(r, s)
    # verify_HASH semantics: the 32-byte digest is the input, not hashed again
    pub.verify(der, digest, ec.ECDSA(Prehashed(hashes.SHA256())))
    # ... which is the same statement as verifying the message with SHA-256
    pub.verify(der, image, ec.ECDSA(hashes.SHA256()))
    # and NOT a signature over the digest-as-message (a double hash)
    with pytest.raises(Exception):
        pub.verify(der, digest, ec.ECDSA(hashes.SHA256()))


def test_c6_image_tamper_is_caught(tmp_path, c6_bundle, key):
    bad = _rebuild(c6_bundle, tmp_path / "bad.zip",
                   lambda m: m.__setitem__("esp32c6.bin", fake_c6_image("1.1.0", 6001)))
    with pytest.raises(fw.BundleError, match="digest mismatch|size mismatch"):
        fw.Bundle(bad).verify(key)


def test_c6_sig_edit_fails_the_manifest_signature(tmp_path, c6_bundle, key):
    def flip(members):
        d = json.loads(members["manifest.json"])
        sig = bytearray.fromhex(d["images"]["esp32c6"]["sig"])
        sig[10] ^= 1
        d["images"]["esp32c6"]["sig"] = sig.hex()
        members["manifest.json"] = json.dumps(d, indent=2, sort_keys=True).encode()

    bad = _rebuild(c6_bundle, tmp_path / "flip.zip", flip)
    with pytest.raises(fw.BundleError, match="signature does NOT verify"):
        fw.Bundle(bad).verify(key)


def test_c6_sig_from_another_key_is_caught_even_in_a_resigned_manifest(
    tmp_path, c6_bundle, key, other_key
):
    """A manifest signed by the right key but carrying an image signature the
    device would refuse: caught on the host, before a 1.4 MB transfer."""
    import hashlib

    def swap(members):
        d = json.loads(members["manifest.json"])
        digest = bytes.fromhex(d["images"]["esp32c6"]["sha256"])
        d["images"]["esp32c6"]["sig"] = fw.sign_digest_raw(digest, other_key).hex()
        blob = json.dumps(d, indent=2, sort_keys=True).encode()
        members["manifest.json"] = blob
        members["manifest.sig"] = fw.sign_digest_raw(hashlib.sha256(blob).digest(), key)

    bad = _rebuild(c6_bundle, tmp_path / "swap.zip", swap)
    with pytest.raises(fw.BundleError, match="esp32c6: image signature does NOT verify"):
        fw.Bundle(bad).verify(key)


def test_c6_entry_without_a_signature_is_refused_without_a_key(tmp_path, c6_bundle):
    def strip(members):
        d = json.loads(members["manifest.json"])
        del d["images"]["esp32c6"]["sig"]
        members["manifest.json"] = json.dumps(d, indent=2, sort_keys=True).encode()

    bad = _rebuild(c6_bundle, tmp_path / "nosig.zip", strip)
    with pytest.raises(fw.BundleError, match="requires a 64-byte signature"):
        fw.Bundle(bad).verify(None)


def test_old_esp_ota_http_entry_is_named_and_not_applied(tmp_path, c6_bundle, key):
    """A bundle made before 2026-11: still a valid bundle (its M4/M7 apply), but
    the C6 entry is unsigned and for a transport that never existed."""
    import hashlib

    def legacy(members):
        d = json.loads(members["manifest.json"])
        e = d["images"]["esp32c6"]
        e["transport"] = "esp-ota-http"
        e["version"] = "unknown"
        del e["sig"]
        blob = json.dumps(d, indent=2, sort_keys=True).encode()
        members["manifest.json"] = blob
        members["manifest.sig"] = fw.sign_digest_raw(hashlib.sha256(blob).digest(), key)

    old = fw.Bundle(_rebuild(c6_bundle, tmp_path / "old.zip", legacy))
    old.verify(key)                              # nothing wrong with the bundle
    why = old.unusable("esp32c6")
    assert why and "predates the USB update path" in why
    assert "cannot be applied" in old.describe()
    assert old.unusable("m4") is None and old.unusable("m7") is None


def test_a_bundle_without_a_c6_says_so(bundle_path):
    assert "does not update the C6" in fw.Bundle(bundle_path).describe()


@pytest.mark.parametrize(
    "image, match",
    [
        (fake_c6_image(chip=5), "chip id 5"),                    # ESP32-C3 (HP5)
        (fake_c6_image(desc_magic=0x50), "MERGED flash image"),  # bootloader desc
        (fake_c6_image(project=b"something_else"), "project"),
        (b"\x00" * 4096, "not an ESP-IDF application image"),
        (fake_c6_image(size=0x1F0001), "OTA slot holds at most"),
    ],
)
def test_create_refuses_a_wrong_c6_image(tmp_path, key, image, match):
    c6 = tmp_path / "c6.bin"
    c6.write_bytes(image)
    with pytest.raises(fw.BundleError, match=match):
        fw.create(tmp_path / "x.zip", [fw.ImageSpec("esp32c6", c6, "", "hpi-g64-esp", sign=True)],
                  release="1", hw_rev=["v5"], key_path=key, created="t")


def test_create_refuses_a_version_that_disagrees_with_the_image(tmp_path, key):
    c6 = tmp_path / "c6.bin"
    c6.write_bytes(fake_c6_image("1.1.0"))
    with pytest.raises(fw.BundleError, match="disagrees"):
        fw.create(tmp_path / "x.zip", [fw.ImageSpec("esp32c6", c6, "1.2.0", "hpi-g64-esp", sign=True)],
                  release="1", hw_rev=["v5"], key_path=key, created="t")


@pytest.mark.parametrize("transport, sign, match", [
    ("hpi-g64-esp", False, "must be signed"),
    ("esp-ota-http", False, "retired"),
])
def test_create_refuses_an_unsigned_or_retired_c6_entry(tmp_path, key, transport, sign, match):
    c6 = tmp_path / "c6.bin"
    c6.write_bytes(fake_c6_image())
    with pytest.raises(fw.BundleError, match=match):
        fw.create(tmp_path / "x.zip", [fw.ImageSpec("esp32c6", c6, "", transport, sign=sign)],
                  release="1", hw_rev=["v5"], key_path=key, created="t")


def test_esp_app_info_reads_a_real_layout():
    info = fw.esp_app_info(fake_c6_image("2.3.4"))
    assert (info.chip_id, info.version, info.project_name, info.idf_version) == (
        13, "2.3.4", "healthybridge_esp32c6", "v6.0.1")
