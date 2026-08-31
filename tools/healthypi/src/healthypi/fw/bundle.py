# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT

"""HealthyPi 6 firmware bundle (``.hpifw``) -- create, read, verify.

A release is one file, not a directory of loose binaries, because a HealthyPi 6
unit has three independently-updateable processors whose versions have to move
together. Handing a customer three .bin files and an ordering rule is how a
device ends up with an M7 that no longer understands its M4.

Layout (a plain zip, so it can be inspected without this tool):

    hpi6-2.0.1.hpifw
      manifest.json     what is inside, per processor, with digests
      manifest.sig      ECDSA-P256 over sha256(manifest.json), raw r||s
      m7.signed.bin     MCUboot-signed M7 image      (stock SMP img group)
      m4.bin            raw M4 image + its signature (group-64 0x00A0-0x00A4)
      esp32c6.bin       optional C6 image            (ESP self-OTA over HTTP)

Signing the MANIFEST rather than the bundle covers every image at once through
their digests, and keeps verification cheap on a host that has already streamed
gigabytes. The M7 image additionally carries its own MCUboot signature -- that is
the one the device enforces; the manifest signature is for the host, so a tool
can refuse a tampered bundle before it starts writing flash.

Used by ``scripts/release.sh`` (``hpi fw bundle create``) and
:mod:`healthypi.fw.update` (apply).
"""

from __future__ import annotations

import hashlib
import json
import zipfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from .keys import KeyError_, sign_digest_raw, verify_digest_raw

MANIFEST_NAME = "manifest.json"
SIGNATURE_NAME = "manifest.sig"
FORMAT_VERSION = 1

#: Processors in the order an update must be applied. The M7 goes last: it is
#: the processor that runs the update logic for the other two, so updating it
#: first would mean applying the rest with firmware that is about to be replaced
#: (and, on the M7 path, a reboot in the middle).
APPLY_ORDER = ("esp32c6", "m4", "m7")


class BundleError(Exception):
    """Bundle is missing, malformed, or fails verification."""


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


# --------------------------------------------------------------------------
# create
# --------------------------------------------------------------------------


@dataclass
class ImageSpec:
    """One processor's image on its way into a bundle."""

    name: str  # m7 | m4 | esp32c6
    path: Path
    version: str
    transport: str  # mcumgr-img | hpi-g64 | esp-ota-http
    min_version: str | None = None
    sign: bool = False  # embed a device-verifiable signature (M4 only)


def create(
    out_path: Path,
    images: list[ImageSpec],
    *,
    release: str,
    hw_rev: list[str],
    key_path: Path,
    created: str,
) -> Path:
    """Write a .hpifw bundle. `created` is passed in rather than read from the
    clock so a release build is reproducible."""
    manifest: dict[str, Any] = {
        "format": FORMAT_VERSION,
        "product": "healthypi6",
        "release": release,
        "hw_rev": hw_rev,
        "created": created,
        "images": {},
    }

    for spec in images:
        if not spec.path.is_file():
            raise BundleError(f"{spec.name}: {spec.path} not found")
        digest = sha256_file(spec.path)
        entry: dict[str, Any] = {
            "file": f"{spec.name}.bin",
            "version": spec.version,
            "sha256": digest,
            "size": spec.path.stat().st_size,
            "transport": spec.transport,
        }
        if spec.min_version:
            entry["min_version"] = spec.min_version
        if spec.sign:
            # The M4 image has no container of its own, so its signature travels
            # in the manifest and is handed to the device in the group-64 begin
            # command. The M7 image does not need this: its MCUboot header
            # already carries a signature the bootloader enforces.
            try:
                entry["sig"] = sign_digest_raw(bytes.fromhex(digest), key_path).hex()
            except KeyError_ as exc:
                raise BundleError(str(exc)) from exc
        manifest["images"][spec.name] = entry

    blob = json.dumps(manifest, indent=2, sort_keys=True).encode()
    try:
        sig = sign_digest_raw(hashlib.sha256(blob).digest(), key_path)
    except KeyError_ as exc:
        raise BundleError(str(exc)) from exc

    out_path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(out_path, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr(MANIFEST_NAME, blob)
        zf.writestr(SIGNATURE_NAME, sig)
        for spec in images:
            zf.write(spec.path, f"{spec.name}.bin")
    return out_path


# --------------------------------------------------------------------------
# read / verify
# --------------------------------------------------------------------------


class Bundle:
    def __init__(self, path: Path):
        self.path = Path(path)
        try:
            self._zf = zipfile.ZipFile(self.path)
        except FileNotFoundError:
            raise BundleError(f"{self.path}: no such file") from None
        except zipfile.BadZipFile as exc:
            raise BundleError(f"{self.path} is not a .hpifw bundle: {exc}") from exc

        try:
            self.raw_manifest = self._zf.read(MANIFEST_NAME)
        except KeyError:
            raise BundleError(f"{self.path} has no {MANIFEST_NAME}") from None
        self.manifest = json.loads(self.raw_manifest)

        fmt = self.manifest.get("format")
        if fmt != FORMAT_VERSION:
            raise BundleError(
                f"{self.path}: bundle format {fmt}, this tool understands "
                f"{FORMAT_VERSION}"
            )

    @property
    def release(self) -> str:
        return self.manifest.get("release", "?")

    @property
    def hw_rev(self) -> list[str]:
        return self.manifest.get("hw_rev", [])

    def images(self) -> dict[str, dict]:
        return self.manifest.get("images", {})

    def verify(self, pubkey: Path | None) -> None:
        """Check every image digest, and the manifest signature when a key is
        given. Raises BundleError on the first failure.

        Digests are checked unconditionally -- they cost one pass over data
        already in memory and catch the ordinary failure (a truncated download)
        that a missing key would otherwise let through to the device."""
        for name, entry in self.images().items():
            data = self.read_image(name)
            got = hashlib.sha256(data).hexdigest()
            if got != entry["sha256"]:
                raise BundleError(
                    f"{name}: digest mismatch (bundle says {entry['sha256'][:16]}…, "
                    f"content is {got[:16]}…) — the bundle is corrupt"
                )
            if len(data) != entry["size"]:
                raise BundleError(f"{name}: size mismatch")

        if pubkey is None:
            return
        try:
            sig = self._zf.read(SIGNATURE_NAME)
        except KeyError:
            raise BundleError(f"{self.path} is unsigned but a key was given") from None
        digest = hashlib.sha256(self.raw_manifest).digest()
        try:
            ok = verify_digest_raw(digest, sig, pubkey)
        except KeyError_ as exc:
            raise BundleError(str(exc)) from exc
        if not ok:
            raise BundleError(
                f"{self.path}: manifest signature does NOT verify against {pubkey}"
            )

    def read_image(self, name: str) -> bytes:
        entry = self.images().get(name)
        if entry is None:
            raise BundleError(f"no {name} image in {self.path}")
        return self._zf.read(entry["file"])

    def image_sig(self, name: str) -> bytes | None:
        entry = self.images().get(name) or {}
        raw = entry.get("sig")
        return bytes.fromhex(raw) if raw else None

    def describe(self) -> str:
        lines = [
            f"{self.path.name}: release {self.release} "
            f"(hw {', '.join(self.hw_rev) or 'any'})"
        ]
        for name in APPLY_ORDER:
            entry = self.images().get(name)
            if not entry:
                continue
            extra = " signed" if entry.get("sig") else ""
            lines.append(
                f"  {name:<8} v{entry['version']:<12} "
                f"{entry['size']:>8} B  {entry['transport']}{extra}"
            )
        return "\n".join(lines)
