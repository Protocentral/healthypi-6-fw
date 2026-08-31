# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT

"""ECDSA-P256 signing and verification for release artifacts.

Raw ``r||s``, never DER. The device verifies with ``psa_verify_hash()``, which
takes the raw pair, so the conversion happens here and the firmware's update
path needs no ASN.1 parser. See ``app_m7/src/services/m4_update_service.h``.

Split out of :mod:`healthypi.fw.bundle` so that key handling has one home: the
bundle signs a manifest, the M4 image carries its own detached signature, and
both go through these two functions.
"""

from __future__ import annotations

from pathlib import Path


class KeyError_(Exception):
    """The key is missing, the wrong type, or the wrong curve."""


def _sha256_algo():
    from cryptography.hazmat.primitives import hashes

    return hashes.SHA256()


def load_private_key(key_path: Path):
    """Load a PEM P-256 private key, rejecting anything else.

    The curve check is not pedantry: an unnoticed P-384 key produces a 96-byte
    signature that the device rejects at commit time, long after the release was
    cut.
    """
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import ec

    key = serialization.load_pem_private_key(Path(key_path).read_bytes(), password=None)
    if not isinstance(key, ec.EllipticCurvePrivateKey) or key.curve.name != "secp256r1":
        raise KeyError_(f"{key_path} is not an ECDSA P-256 private key")
    return key


def sign_digest_raw(digest: bytes, key_path: Path) -> bytes:
    """ECDSA-P256 over an already-computed SHA-256, returned as raw ``r||s``."""
    from cryptography.hazmat.primitives.asymmetric import ec, utils

    key = load_private_key(key_path)
    der = key.sign(digest, ec.ECDSA(utils.Prehashed(_sha256_algo())))
    r, s = utils.decode_dss_signature(der)
    return r.to_bytes(32, "big") + s.to_bytes(32, "big")


def verify_digest_raw(digest: bytes, sig: bytes, pub_pem_or_key: Path) -> bool:
    """Verify a raw ``r||s`` signature. Accepts a private or a public PEM.

    Accepting either is deliberate: on a release machine the private key is what
    is to hand, and requiring a separately-exported public key would mean one
    more artifact to keep in step with it.
    """
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import ec, utils

    blob = Path(pub_pem_or_key).read_bytes()
    try:
        pub = serialization.load_pem_private_key(blob, password=None).public_key()
    except ValueError:
        pub = serialization.load_pem_public_key(blob)

    if len(sig) != 64:
        raise KeyError_(f"signature is {len(sig)} B, expected 64 (raw r||s)")
    r = int.from_bytes(sig[:32], "big")
    s = int.from_bytes(sig[32:], "big")
    der = utils.encode_dss_signature(r, s)
    try:
        pub.verify(der, digest, ec.ECDSA(utils.Prehashed(_sha256_algo())))
        return True
    except Exception:  # noqa: BLE001 -- any failure is "does not verify"
        return False
