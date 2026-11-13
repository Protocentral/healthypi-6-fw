# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT

"""HealthyPi 6 firmware bundle -- create, read, verify.

A release is one file, not a directory of loose binaries, because a HealthyPi 6
unit has three independently-updateable processors whose versions have to move
together. Handing a customer three .bin files and an ordering rule is how a
device ends up with an M7 that no longer understands its M4.

Layout (a plain zip, so it can be inspected without this tool):

    hpi6-firmware-2.0.1.zip
      manifest.json     what is inside, per processor, with digests
      manifest.sig      ECDSA-P256 over sha256(manifest.json), raw r||s
      m7.bin            MCUboot-signed M7 image      (stock SMP img group)
      m4.bin            raw M4 image + its signature (group-64 0x00A0-0x00A4)
      esp32c6.bin       C6 app image + its signature (group-64 0x00A6-0x00AA,
                        relayed by the M7)

The extension is not significant: readers open the file as a zip, so bundles
named ``.hpifw`` (the name used before 2026-10) still work. Releases are
named ``hpi6-firmware-<version>.zip``.

Signing the MANIFEST rather than the bundle covers every image at once through
their digests, and keeps verification cheap on a host that has already streamed
gigabytes. The M7 image additionally carries its own MCUboot signature -- that is
the one the device enforces; the manifest signature is for the host, so a tool
can refuse a tampered bundle before it starts writing flash.

Used by ``scripts/release.sh`` (``healthypi fw bundle create``) and
:mod:`healthypi.fw.update` (apply).
"""

from __future__ import annotations

import hashlib
import json
import zipfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from .keys import KeyError_, fingerprint, release_public_keys, sign_digest_raw, verify_digest_raw

MANIFEST_NAME = "manifest.json"
SIGNATURE_NAME = "manifest.sig"
FORMAT_VERSION = 1

#: Processors in the order an update must be applied: the M4 and the M7 first,
#: then the reset (two, when the M7 changed) they need, then the ESP32-C6.
#:
#: The C6 goes LAST because any M7 reset holds the C6 in reset, and a C6 reset
#: before its new image has proved itself rolls that image back -- so nothing
#: may reset the device between the C6's COMMIT and its VALID report. Doing it
#: last also means the C6 is always updated by the M7 firmware the bundle just
#: installed, which is what lets one run update a unit whose old M7 predates the
#: C6 update commands. (Until 2026-11 the order was esp32c6 -> m4 -> m7, from
#: when the C6 was not updated through the M7 at all.)
#:
#: The M4 still precedes the M7: its upload and commit run on the current M7
#: firmware, and both then share one reset.
APPLY_ORDER = ("m4", "m7", "esp32c6")

# --- transports, per image entry ---------------------------------------------
#
# The manifest versions an image's update path through its `transport` string,
# not through `format`: a tool that does not know a transport skips that one
# image and says so (docs/ARCHITECTURE.md §10, "Compatibility rules"), which is
# exactly the behaviour wanted when the C6 path changed. `format` stays 1.

TRANSPORT_M7 = "mcumgr-img"
TRANSPORT_M4 = "hpi-g64"
#: The C6 app image, relayed by the M7 (group-64 espfw_*). Carries `sig`.
TRANSPORT_ESP = "hpi-g64-esp"
#: What C6 entries said before 2026-11: an unsigned image for the C6's own
#: HTTP OTA, which never existed on HealthyPi 6. Recognised so it can be named
#: and skipped -- never applied, because it has no signature the M7 would accept.
TRANSPORT_ESP_LEGACY = "esp-ota-http"

#: Transports whose entry MUST carry a device-verifiable `sig`.
SIGNED_TRANSPORTS = frozenset({TRANSPORT_ESP})

#: Largest C6 image that fits: one OTA slot, 1984 KB (partitions.hp6.csv in
#: healthybridge-esp32). The M7 alone would take up to 4 MiB
#: (HPI_ESPFW_MAX_IMAGE), but the C6 refuses BEGIN for anything over its slot,
#: so checking the slot here moves that refusal from the device to the release.
ESP_MAX_IMAGE = 0x1F0000

# ESP-IDF app image layout (esp_app_format.h): a 24-byte esp_image_header_t,
# one 8-byte segment header, then esp_app_desc_t at offset 32.
_ESP_IMAGE_MAGIC = 0xE9
_ESP_CHIP_ID_OFF = 12          # esp_image_header_t.chip_id, uint16 LE
_ESP_CHIP_ESP32C6 = 13         # ESP_CHIP_ID_ESP32C6
_ESP_APP_DESC_OFF = 32
_ESP_APP_DESC_MAGIC = 0xABCD5432
#: project_name the HealthyPi 6 C6 firmware is built as (healthybridge-esp32,
#: HP6 profile). The C6 itself refuses an image with another project name at
#: COMMIT; checking here refuses it when the bundle is made instead.
ESP_PROJECT_NAME = "healthybridge_esp32c6"


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
    transport: str  # mcumgr-img | hpi-g64 | hpi-g64-esp
    min_version: str | None = None
    sign: bool = False  # embed a device-verifiable signature (M4, C6)


@dataclass(frozen=True)
class EspAppInfo:
    """What an ESP-IDF application image says about itself."""

    chip_id: int
    version: str
    project_name: str
    idf_version: str


def _cstr(blob: bytes) -> str:
    return blob.split(b"\0", 1)[0].decode("utf-8", "replace")


def esp_app_info(data: bytes) -> EspAppInfo:
    """Parse an ESP32-C6 *application* image's header and app descriptor.

    Refuses what the M7/C6 path cannot take, with a reason a release engineer
    can act on: not an ESP image, built for another chip, or the MERGED flash
    image (bootloader + partition table + app), which also starts with 0xE9 but
    carries a bootloader descriptor where the app descriptor should be.
    """
    if len(data) < _ESP_APP_DESC_OFF + 112 or data[0] != _ESP_IMAGE_MAGIC:
        raise BundleError(
            "esp32c6: not an ESP-IDF application image (no 0xE9 image header). "
            "Use build.hp6/healthybridge_esp32c6.bin from healthybridge-esp32."
        )
    chip = int.from_bytes(data[_ESP_CHIP_ID_OFF:_ESP_CHIP_ID_OFF + 2], "little")
    if chip != _ESP_CHIP_ESP32C6:
        raise BundleError(
            f"esp32c6: the image was built for chip id {chip}, not the ESP32-C6 "
            f"({_ESP_CHIP_ESP32C6}) -- wrong target (the HealthyPi 5 build is an "
            "ESP32-C3 image). Build the HP6 profile: ./hp6.sh build."
        )
    magic = int.from_bytes(data[_ESP_APP_DESC_OFF:_ESP_APP_DESC_OFF + 4], "little")
    if magic != _ESP_APP_DESC_MAGIC:
        raise BundleError(
            "esp32c6: the image has no application descriptor -- this looks like "
            "the MERGED flash image (bootloader first), not the app. The M7 relays "
            "the app into an OTA slot: use build.hp6/healthybridge_esp32c6.bin."
        )
    d = _ESP_APP_DESC_OFF
    # esp_app_desc_t: magic(4) secure_version(4) reserv1(8) version[32]
    # project_name[32] time[16] date[16] idf_ver[32] ...
    return EspAppInfo(
        chip_id=chip,
        version=_cstr(data[d + 16:d + 48]),
        project_name=_cstr(data[d + 48:d + 80]),
        idf_version=_cstr(data[d + 112:d + 144]),
    )


def check_esp_image(path: Path, version: str | None) -> str:
    """Validate a C6 app image for a bundle; return its version.

    `version` given on the command line must agree with the one compiled into
    the image -- the image's own descriptor is what the C6 will report once it
    runs, so it is the number the updater compares against.
    """
    data = path.read_bytes()
    if len(data) > ESP_MAX_IMAGE:
        raise BundleError(
            f"esp32c6: {path} is {len(data)} B; a C6 OTA slot holds at most "
            f"{ESP_MAX_IMAGE} B"
        )
    info = esp_app_info(data)
    if info.project_name != ESP_PROJECT_NAME:
        raise BundleError(
            f"esp32c6: the image is project {info.project_name!r}, not "
            f"{ESP_PROJECT_NAME!r} -- the C6 refuses another project at COMMIT. "
            "Wrong build profile?"
        )
    if not info.version:
        raise BundleError("esp32c6: the image's app descriptor carries no version")
    if version and version not in ("unknown", info.version):
        raise BundleError(
            f"esp32c6: --esp32c6-version {version} disagrees with the version "
            f"compiled into the image ({info.version}). Omit the option to use "
            "the image's own."
        )
    return info.version


def create(
    out_path: Path,
    images: list[ImageSpec],
    *,
    release: str,
    hw_rev: list[str],
    key_path: Path,
    created: str,
) -> Path:
    """Write a bundle zip. `created` is passed in rather than read from the
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
        if spec.transport == TRANSPORT_ESP_LEGACY:
            raise BundleError(
                f"{spec.name}: transport {TRANSPORT_ESP_LEGACY!r} is retired -- the "
                f"C6 is updated through the M7 now ({TRANSPORT_ESP!r})"
            )
        if spec.transport in SIGNED_TRANSPORTS and not spec.sign:
            # There is no unsigned mode on the device side: espfw_begin refuses
            # a request without a signature, so an unsigned entry is useless.
            raise BundleError(f"{spec.name}: transport {spec.transport!r} must be signed")
        if spec.transport == TRANSPORT_ESP:
            spec.version = check_esp_image(spec.path, spec.version)
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
            # command. The C6 image likewise: the M7 checks this signature over
            # the image's SHA-256 (fw_release_sig.c, the same verifier as the
            # M4's) before it relays a byte. The M7 image does not need this:
            # its MCUboot header already carries a signature the bootloader
            # enforces.
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
    """A release bundle: the zip, or the folder it was extracted to.

    The folder form exists because browsers unpack downloads -- Safari's
    default "Open safe files after downloading" turns hpi6-firmware-*.zip into
    a directory -- and a user holding that directory has everything the zip
    had. Nothing is trusted more for it: the digests in manifest.json and the
    manifest signature are checked exactly as for the zip.
    """

    def __init__(self, path: Path):
        self.path = Path(path)
        self._zf: zipfile.ZipFile | None = None
        self._dir: Path | None = None
        if self.path.is_dir():
            self._dir = self.path.resolve()
        else:
            try:
                self._zf = zipfile.ZipFile(self.path)
            except FileNotFoundError:
                raise BundleError(f"{self.path}: no such file") from None
            except zipfile.BadZipFile as exc:
                raise BundleError(
                    f"{self.path} is not a firmware bundle (zip): {exc}"
                ) from exc

        try:
            self.raw_manifest = self._read(MANIFEST_NAME)
        except KeyError:
            raise BundleError(f"{self.path} has no {MANIFEST_NAME}") from None
        self.manifest = json.loads(self.raw_manifest)

        fmt = self.manifest.get("format")
        if fmt != FORMAT_VERSION:
            raise BundleError(
                f"{self.path}: bundle format {fmt}, this tool understands "
                f"{FORMAT_VERSION}"
            )

    def _read(self, member: str) -> bytes:
        """One member's bytes, from the zip or the extracted folder. Raises
        KeyError when it is absent, like ZipFile.read()."""
        if self._zf is not None:
            return self._zf.read(member)
        target = (self._dir / member).resolve()
        # Member names come from manifest.json; never follow one out of the
        # bundle folder.
        if self._dir not in target.parents and target != self._dir:
            raise BundleError(f"{self.path}: member {member!r} is outside the bundle")
        if not target.is_file():
            raise KeyError(member)
        return target.read_bytes()

    @property
    def release(self) -> str:
        return self.manifest.get("release", "?")

    @property
    def hw_rev(self) -> list[str]:
        return self.manifest.get("hw_rev", [])

    def images(self) -> dict[str, dict]:
        return self.manifest.get("images", {})

    def unusable(self, name: str) -> str | None:
        """Why this tool will not apply the bundle's `name` image, or None.

        Today only the C6 entry can be unusable: a bundle made before 2026-11
        carries it with transport ``esp-ota-http`` and no signature, which the
        M7's update path refuses by design. Such an image is skipped -- never
        half-applied -- and the rest of the bundle is still good.
        """
        entry = self.images().get(name)
        if entry is None or name != "esp32c6":
            return None
        transport = entry.get("transport")
        if transport == TRANSPORT_ESP_LEGACY:
            return (
                "this bundle's ESP32-C6 image predates the USB update path "
                f"(transport {TRANSPORT_ESP_LEGACY!r}, unsigned): the M7 accepts "
                "only a signed C6 image, so it cannot be applied. Use a bundle "
                "built by scripts/release.sh from November 2026 on."
            )
        if transport != TRANSPORT_ESP:
            return (
                f"unknown ESP32-C6 transport {transport!r} -- this healthypi is "
                "older than the bundle; upgrade it (pip install -U "
                "protocentral-healthypi)."
            )
        return None

    def verify(self, pubkey: Path | None) -> None:
        """Check every image digest, and the manifest signature when a key is
        given. Raises BundleError on the first failure.

        Digests are checked unconditionally -- they cost one pass over data
        already in memory and catch the ordinary failure (a truncated download)
        that a missing key would otherwise let through to the device.

        With a key, each image signature the device will check (the M4's and
        the C6's) is checked too, against the same key: the device verifies
        those with its compiled-in release key, so one that does not verify here
        would be refused there -- after the upload rather than before it."""
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
            if entry.get("transport") in SIGNED_TRANSPORTS:
                raw = entry.get("sig") or ""
                if len(raw) != 128:
                    raise BundleError(
                        f"{name}: transport {entry['transport']!r} requires a 64-byte "
                        f"signature and the entry carries {'none' if not raw else 'a malformed one'}"
                    )

        if pubkey is None:
            return
        for name, entry in self.images().items():
            raw = entry.get("sig")
            if not raw:
                continue
            try:
                good = verify_digest_raw(
                    bytes.fromhex(entry["sha256"]), bytes.fromhex(raw), pubkey
                )
            except (KeyError_, ValueError) as exc:
                raise BundleError(f"{name}: image signature unreadable: {exc}") from exc
            if not good:
                raise BundleError(
                    f"{name}: image signature does NOT verify against {pubkey} -- "
                    "the device would refuse this image"
                )
        try:
            sig = self._read(SIGNATURE_NAME)
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

    def authenticate(self, pubkey: Path | None = None) -> str:
        """Check digests, then the manifest signature, and say what was checked.

        With `pubkey`, the signature must verify against it -- the bench case,
        a dev key, or an owner who re-keyed their unit. Without it, against the
        HealthyPi 6 release key(s) shipped in this package; any one may match
        (primary + backup). Only when the package ships no release key is the
        signature left unchecked, and the returned line says so.

        Every device-facing command goes through here, so "no --pubkey" can no
        longer mean "anyone's repackaged bundle is accepted".
        """
        if pubkey is not None:
            self.verify(Path(pubkey))
            return f"manifest signature verified against {pubkey} ({fingerprint(Path(pubkey))})"
        keys = release_public_keys()
        if not keys:
            self.verify(None)
            return (
                "note: this healthypi carries no release public key, so the manifest "
                "signature was NOT checked (digests were). Pass --pubkey to check it."
            )
        self.verify(None)  # digests first: a corrupt download is the common case
        for key in keys:
            try:
                self.verify(key)
            except BundleError:
                continue
            return f"manifest signature verified: HealthyPi 6 release key {fingerprint(key)}"
        known = ", ".join(fingerprint(k) for k in keys)
        raise BundleError(
            f"{self.path}: manifest signature does not verify against the HealthyPi 6 "
            f"release key(s) shipped with this tool ({known}).\n"
            "  If you built and signed this bundle yourself (your own key, or the dev "
            "key), pass --pubkey <that key>.pem."
        )

    def read_image(self, name: str) -> bytes:
        entry = self.images().get(name)
        if entry is None:
            raise BundleError(f"no {name} image in {self.path}")
        try:
            return self._read(entry["file"])
        except KeyError:
            raise BundleError(f"{self.path}: {entry['file']} is missing") from None

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
            if self.unusable(name):
                extra += "  (cannot be applied -- see `fw update`)"
            lines.append(
                f"  {name:<8} v{entry['version']:<12} "
                f"{entry['size']:>8} B  {entry['transport']}{extra}"
            )
        if "esp32c6" not in self.images():
            lines.append("  (no esp32c6 image: this bundle does not update the C6)")
        return "\n".join(lines)
