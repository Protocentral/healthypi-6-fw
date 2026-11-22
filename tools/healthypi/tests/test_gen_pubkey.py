"""tools/build/gen_m4_pubkey.py: the key lists MCUboot and the M7 compile in.

Both images must trust the same keys, in the same order, in the encoding each
verifier expects. These check the generated C against keys made here, byte for
byte, without a toolchain.
"""

from __future__ import annotations

import hashlib
import re
import subprocess
import sys
from pathlib import Path

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ec

GEN = Path(__file__).resolve().parents[3] / "tools" / "build" / "gen_m4_pubkey.py"


def _key(tmp_path: Path, name: str, public: bool = False) -> tuple[Path, ec.EllipticCurvePublicKey]:
    k = ec.generate_private_key(ec.SECP256R1())
    path = tmp_path / name
    if public:
        path.write_bytes(k.public_key().public_bytes(
            serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo))
    else:
        path.write_bytes(k.private_bytes(
            serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
            serialization.NoEncryption()))
    return path, k.public_key()


def _run(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, str(GEN), *args], capture_output=True, text=True)


def _arrays(c: str) -> list[bytes]:
    """Every brace-delimited run of 0x.. bytes, in order."""
    out = []
    for body in re.findall(r"\{([^{}]*0x[^{}]*)\}", c):
        out.append(bytes(int(h, 16) for h in re.findall(r"0x([0-9a-f]{2})", body)))
    return out


def test_m7_list_holds_both_points_primary_first(tmp_path):
    p, ppub = _key(tmp_path, "primary.pem")
    b, bpub = _key(tmp_path, "backup.pub.pem", public=True)    # a public key works too
    out = tmp_path / "keys.c"
    r = _run("--key", str(p), "--key", str(b), "--output", str(out))
    assert r.returncode == 0, r.stderr
    c = out.read_text()
    assert "hpi_fw_release_pubkeys[2][65]" in c
    assert "hpi_fw_release_pubkey_count = 2;" in c
    point = lambda k: k.public_bytes(serialization.Encoding.X962,
                                     serialization.PublicFormat.UncompressedPoint)
    assert _arrays(c) == [point(ppub), point(bpub)]


def test_m7_list_with_one_key(tmp_path):
    p, ppub = _key(tmp_path, "primary.pem")
    out = tmp_path / "keys.c"
    assert _run("--key", str(p), "--output", str(out)).returncode == 0
    assert "hpi_fw_release_pubkey_count = 1;" in out.read_text()


def test_mcuboot_table_is_the_der_imgtool_hashes(tmp_path):
    """MCUboot picks the key whose SHA-256 matches the image's KEYHASH TLV, and
    imgtool hashes SubjectPublicKeyInfo DER. A different encoding would build,
    boot nothing, and look like a bad signature."""
    p, ppub = _key(tmp_path, "primary.pem")
    b, bpub = _key(tmp_path, "backup.pem")
    out = tmp_path / "boot_keys.c"
    r = _run("--format", "mcuboot", "--key", str(p), "--key", str(b), "--output", str(out))
    assert r.returncode == 0, r.stderr
    c = out.read_text()
    der = lambda k: k.public_bytes(serialization.Encoding.DER,
                                   serialization.PublicFormat.SubjectPublicKeyInfo)
    arrays = [a for a in _arrays(c) if len(a) == 91]
    assert arrays == [der(ppub), der(bpub)]
    assert "const int bootutil_key_cnt = 2;" in c
    assert "const struct bootutil_key bootutil_keys[]" in c
    # What imgtool writes into KEYHASH for each key -- distinct, so each image
    # selects exactly one entry.
    assert len({hashlib.sha256(a).digest() for a in arrays}) == 2


def test_same_key_twice_is_refused(tmp_path):
    """A 'backup' that is the primary again protects nothing."""
    p, _ = _key(tmp_path, "primary.pem")
    pub = tmp_path / "primary.pub.pem"
    pub.write_bytes(serialization.load_pem_private_key(p.read_bytes(), None).public_key()
                    .public_bytes(serialization.Encoding.PEM,
                                  serialization.PublicFormat.SubjectPublicKeyInfo))
    for fmt in ("m7", "mcuboot"):
        r = _run("--format", fmt, "--key", str(p), "--key", str(pub),
                 "--output", str(tmp_path / "x.c"))
        assert r.returncode != 0
        assert "twice" in r.stderr


def test_non_p256_key_is_refused(tmp_path):
    k = ec.generate_private_key(ec.SECP384R1())
    path = tmp_path / "p384.pem"
    path.write_bytes(k.private_bytes(serialization.Encoding.PEM,
                                     serialization.PrivateFormat.PKCS8,
                                     serialization.NoEncryption()))
    r = _run("--key", str(path), "--output", str(tmp_path / "x.c"))
    assert r.returncode != 0
    assert "P-256" in r.stderr
