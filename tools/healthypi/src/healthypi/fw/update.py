# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT

"""Apply a ``.hpifw`` bundle to a device -- the supported way to update.

One bundle, all three processors, in :data:`~healthypi.fw.bundle.APPLY_ORDER`.
The M7 goes through the stock MCUmgr img group; the M4 through group-64
``0x00A0-0x00A4``, staged in QSPI and written to bank 2 by the M7; the C6
updates itself over WiFi and is skipped here.

The port is CDC 1, the control port. CDC 0 carries the ``.HP6`` sample stream
and is never used for updates.

Operational guide, including the recovery ladder: ``docs/ARCHITECTURE.md`` §9.
"""

from __future__ import annotations

import asyncio
import hashlib
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable

from ..transport import serial_smp
from .bundle import APPLY_ORDER, Bundle

# Bytes an m4fw_chunk request spends on framing before any payload: the 8-byte
# SMP header plus CBOR for {"off": <uint>, "data": <bstr>}. ~34 in practice; 48
# leaves margin, because guessing high costs a few bytes per chunk while
# guessing low fails at runtime with "Data size N exceeds maximum".
_CHUNK_OVERHEAD = 48

_RESET_SETTLE_S = 12.0  # M4 rebinds IPC at ~7-10 s; wait past that
_RECONNECT_TRIES = 30

#: Budget for group-64 0x00A2 (M4 commit): SHA-256 over the staged image, an
#: ECDSA-P256 verify, a 256 KB bank-2 sector erase, then the image write -- all
#: before the device replies. Measured ~3-4 s for a 160 KB image on v5; 30 s is
#: deliberate headroom, and a timeout at this length is a real fault worth
#: surfacing rather than something to wait out.
M4_COMMIT_TIMEOUT_S = 30.0

Log = Callable[[str], None]


def _stdout(msg: str) -> None:
    print(msg, flush=True)


class UpdateError(Exception):
    """The update could not proceed. The message says what the device state is."""


@dataclass
class Target:
    """Where the device is and how to talk to it."""

    port: str | None = None
    baud: int = serial_smp.DEFAULT_BAUD
    frame_size: int = serial_smp.DEFAULT_FRAME_SIZE

    async def connect(self) -> serial_smp.Connection:
        return await serial_smp.connect(
            self.port, baud=self.baud, frame_size=self.frame_size
        )


@dataclass
class Result:
    """What an update actually did."""

    applied: list[str] = field(default_factory=list)
    skipped: list[str] = field(default_factory=list)
    ok: bool = True
    versions_after: dict[str, str] = field(default_factory=dict)


# ---------------------------------------------------------------------------
# version comparison
# ---------------------------------------------------------------------------


def _ver_tuple(v: str) -> tuple:
    """Loose semver: '2.0.1-dev' -> (2, 0, 1). Suffixes are ignored on purpose.

    A '-dev' suffix is a build flavour, not an ordering: treating 2.0.1-dev as
    older than 2.0.1 would make the updater reflash on every run during
    development, which trains people to ignore it.
    """
    core = v.split("-")[0].split("+")[0]
    parts = []
    for piece in core.split("."):
        try:
            parts.append(int(piece))
        except ValueError:
            break
    return tuple(parts) or (0,)


def same_version(device: str, bundle: str) -> bool:
    """Is the device already at the bundle's version?

    Suffix-insensitive, per :func:`_ver_tuple`: a device on "1.0.0-dev" counts as
    already at a bundle's "1.0.0". That is deliberate, but it has a consequence
    worth knowing -- **going from a dev build to the release of the same number
    is skipped**, and you need ``force``. Seen during F8, where a 1.0.0-dev M4
    was skipped against a 1.0.0 bundle. The alternative (ordering "-dev" below
    the release) would reflash on every run during development, which is worse.
    """
    if not device:
        return False  # unknown -> always apply
    return _ver_tuple(device) == _ver_tuple(bundle)


# ---------------------------------------------------------------------------
# device queries
# ---------------------------------------------------------------------------


async def _fw_versions(conn, g, is_error, fmt_error) -> dict[str, str]:
    resp = await conn.request(g.fw_versions())
    if is_error(resp):
        raise UpdateError(f"fw_versions failed: {fmt_error(resp)}")
    return {"m7": resp.m7fw, "m4": resp.m4fw, "esp32c6": resp.espfw}


async def _quiet_the_stream(conn, g, is_error, fmt_error, log: Log) -> None:
    """Stop the CDC 0 sample stream before uploading.

    Not cosmetic: the stream and the upload share one USB device, and leaving it
    running is the usual explanation for an update that crawls.
    """
    resp = await conn.request(g.stream_stop())
    if is_error(resp):
        log(f"  note: stream_stop refused ({fmt_error(resp)}) — continuing")


# ---------------------------------------------------------------------------
# M7 -- stock MCUmgr img group
# ---------------------------------------------------------------------------


def mcuboot_image_hash(image: bytes) -> bytes | None:
    """The SHA-256 TLV of an imgtool-signed image -- the identity MCUboot and the
    img group use.

    This is NOT hashlib.sha256(image). The MCUboot hash covers the header, the
    payload and the PROTECTED TLVs only; the signature and the hash TLV itself
    live in the unprotected area and are excluded. A whole-file digest therefore
    never matches, and `img state write` answers HASH_NOT_FOUND -- which this tool
    used to log as "harmless in overwrite-only mode" while the image sat in
    slot 1 unmarked and was never installed (F8, 2026-07-25). The scripted OTA
    acceptance run had it right; the supported updater did not.
    """
    from smpclient.mcuboot import IMAGE_TLV, ImageInfo

    with tempfile.NamedTemporaryFile(suffix=".bin") as f:
        f.write(image)
        f.flush()
        try:
            info = ImageInfo.load_file(f.name)
            return bytes(info.get_tlv(IMAGE_TLV.SHA256).value)
        except Exception:  # noqa: BLE001 -- not imgtool-signed, or no TLV
            return None


async def _m7_slot_hashes(conn, is_error) -> dict[int, bytes]:
    """slot -> MCUboot image hash, as the device reports it."""
    from smpclient.requests.image_management import ImageStatesRead

    resp = await conn.request(ImageStatesRead())
    if is_error(resp):
        return {}
    out = {}
    for img in getattr(resp, "images", None) or []:
        if img.hash:
            out[img.slot] = bytes(img.hash)
    return out


async def _apply_m7(conn, image: bytes, *, is_error, fmt_error, log: Log) -> bool:
    from smpclient.requests.image_management import ImageStatesWrite

    # slot=0 means "image 0"; the device routes it to that image's SECONDARY
    # slot (slot1, on QSPI) itself. Passing 1 here would name image 1, which
    # does not exist -- MCUboot manages only the M7.
    log(f"M7 : {len(image)} B → QSPI secondary slot (MCUboot img group)")

    img_hash = mcuboot_image_hash(image)
    if img_hash is None:
        raise UpdateError(
            "M7: the bundle's image carries no SHA-256 TLV — it is not an "
            "imgtool-signed MCUboot image, so it cannot be marked for install."
        )

    # An install that cannot change anything must say so rather than report
    # success. MCUboot will decline a secondary slot that is not NEWER than the
    # primary (downgrade prevention), so re-applying the running image is a
    # no-op no matter what the version strings say.
    before = await _m7_slot_hashes(conn, is_error)
    if before.get(0) == img_hash:
        log(
            "  note: the device is ALREADY running this exact image "
            "(identical MCUboot hash). The upload will land in slot 1 and "
            "MCUboot will decline it as not-newer — nothing will change."
        )

    t0 = time.monotonic()
    async for upload_off in conn.client.upload(image, slot=0, upgrade=False):
        pct = 100.0 * upload_off / len(image)
        print(f"\r  upload {upload_off}/{len(image)} B ({pct:.0f}%)", end="", flush=True)
    dt = time.monotonic() - t0
    print(f"\n  uploaded in {dt:.0f}s ({len(image) / 1024 / max(dt, 0.001):.1f} KB/s)")

    # Mark it pending. This is REQUIRED, not advisory: MCUboot reads the pending
    # flag out of the slot-1 trailer to decide whether to upgrade at all, so an
    # unmarked image is simply ignored on the next boot.
    resp = await conn.request(ImageStatesWrite(hash=img_hash, confirm=False))
    if is_error(resp):
        raise UpdateError(
            f"M7: marking the uploaded image for install FAILED: "
            f"{fmt_error(resp)}\n"
            "  The image is in slot 1 but is not flagged pending, so MCUboot "
            "will ignore it and the device will keep running the old firmware.\n"
            "  Nothing is broken on the device — re-run the update."
        )
    log("  marked pending — MCUboot will install it on the next boot")
    return True  # a reset is required


# ---------------------------------------------------------------------------
# M4 -- group-64 upload through the M7
# ---------------------------------------------------------------------------


async def _apply_m4(
    conn, image: bytes, sig: bytes | None, *, g, is_error, fmt_error, log: Log
) -> bool:
    digest = hashlib.sha256(image).digest()
    max_unencoded = getattr(conn.transport, "max_unencoded_size", 256)
    chunk = max(32, max_unencoded - _CHUNK_OVERHEAD)

    status = await conn.request(g.m4fw_status())
    if is_error(status):
        raise UpdateError(
            f"M4 status failed: {fmt_error(status)}\n"
            "  If this is 'rc=8', the running firmware has no M4-update support "
            "at all — it is a DEV build. CONFIG_HPI_M4_UPDATE lives in "
            "prj.signed.conf.\n"
            "  Flash the signed build: scripts/flash.sh signed"
        )
    if status.sig and sig is None:
        raise UpdateError(
            "M4: this device requires a SIGNED image, and the bundle carries no "
            "signature for it.\n"
            "  Rebuild the bundle with scripts/release.sh (which signs), or use "
            "a device built with CONFIG_HPI_M4_UPDATE_REQUIRE_SIGNATURE=n for "
            "bench work."
        )

    log(
        f"M4 : {len(image)} B → QSPI staging, {chunk} B/chunk"
        f"{' (signed)' if sig else ' (unsigned)'}"
    )

    resp = await conn.request(
        g.m4fw_begin(len=len(image), sha=digest, sig=sig)
        if sig
        else g.m4fw_begin(len=len(image), sha=digest)
    )
    if is_error(resp):
        raise UpdateError(f"M4 begin failed: {fmt_error(resp)}")

    t0 = time.monotonic()
    off = 0
    while off < len(image):
        piece = image[off : off + chunk]
        resp = await conn.request(g.m4fw_chunk(off=off, data=piece))
        if is_error(resp):
            raise UpdateError(f"\nM4 chunk at +{off} failed: {fmt_error(resp)}")
        # Trust the device's next-expected offset over local bookkeeping.
        if resp.off != off + len(piece):
            raise UpdateError(
                f"\nM4: device resynced unexpectedly (sent +{off}+{len(piece)}, "
                f"device expects +{resp.off})"
            )
        off = resp.off
        pct = 100.0 * off / len(image)
        print(f"\r  upload {off}/{len(image)} B ({pct:.0f}%)", end="", flush=True)
    dt = time.monotonic() - t0
    print(f"\n  uploaded in {dt:.0f}s ({len(image) / 1024 / max(dt, 0.001):.1f} KB/s)")

    log("  commit (digest → signature → vector check → erase + write bank 2)")
    # Commit is the one long-running group-64 command and it MUST get an explicit
    # timeout: it hashes the staged image, verifies ECDSA-P256, then erases a
    # 256 KB bank-2 sector and writes the image, all before replying. smpclient's
    # 2.5 s default is not close to enough -- the device completed the work and
    # answered correctly while this tool had already given up, reporting a bogus
    # TimeoutError for a successful update (F8, 2026-07-25). D2's original
    # validation predated F4's signature verification, which is what pushed the
    # commit past the default.
    resp = await conn.request(g.m4fw_commit(), timeout_s=M4_COMMIT_TIMEOUT_S)
    if is_error(resp):
        raise UpdateError(
            f"M4 commit REFUSED: {fmt_error(resp)}\n"
            "  Bank 2 was NOT modified — the M4 is still running its old image."
        )
    log("  committed — bank 2 written")
    return True  # a reset is required


# ---------------------------------------------------------------------------
# reset / reconnect
# ---------------------------------------------------------------------------


async def _reset_and_reconnect(conn, target: Target, log: Log):
    from smpclient.requests.os_management import ResetWrite

    log("  resetting…")
    await conn.request(ResetWrite())
    try:
        await conn.client.__aexit__(None, None, None)
    except Exception:  # noqa: BLE001 -- already gone
        pass
    await asyncio.sleep(_RESET_SETTLE_S)

    # Pin the port for reconnection. Autodetect would probe every candidate on
    # each of 30 attempts, and on a machine with other serial devices it can
    # settle on the wrong one while the HealthyPi is still enumerating.
    resolved = Target(port=conn.port, baud=target.baud, frame_size=target.frame_size)
    for attempt in range(_RECONNECT_TRIES):
        try:
            new = await resolved.connect()
            log(f"  reconnected after {attempt + 1} attempt(s)")
            return new
        except Exception:  # noqa: BLE001 -- any transport error means "not back yet"
            await asyncio.sleep(1.0)
    raise UpdateError(
        f"device did not come back on {conn.port} after reset.\n"
        "  USB re-enumeration can change the port name — re-run with the new "
        "port to check the result. If it never appears, see the recovery ladder "
        "in docs/ARCHITECTURE.md."
    )


# ---------------------------------------------------------------------------
# the flow
# ---------------------------------------------------------------------------


async def apply_bundle(
    bundle: Bundle,
    target: Target,
    *,
    pubkey: Path | None = None,
    only: str | None = None,
    force: bool = False,
    dry_run: bool = False,
    log: Log = _stdout,
) -> Result:
    """Verify a bundle, then bring the device up to it.

    Returns a :class:`Result`; raises :class:`UpdateError` if the device refuses
    at a point where continuing would be unsafe or misleading.
    """
    from ..smp.group64 import fmt_error, g, is_error

    bundle.verify(pubkey)
    log(bundle.describe())
    if pubkey:
        log(f"  manifest signature verified against {pubkey}")
    else:
        log("  note: no pubkey given, manifest signature NOT checked (digests were)")

    result = Result()

    # Managed by hand rather than `async with`, because the mid-flow reset
    # replaces the connection: a context manager would still try to close the
    # original, already-disconnected one on the way out.
    conn = await target.connect()
    try:
        installed = await _fw_versions(conn, g, is_error, fmt_error)
        log("\ndevice:")
        for name in APPLY_ORDER:
            entry = bundle.images().get(name)
            if entry:
                cur = installed.get(name) or "?"
                log(f"  {name:<8} installed {cur:<14} bundle {entry['version']}")

        plan: list[str] = []
        for name in APPLY_ORDER:
            entry = bundle.images().get(name)
            if not entry:
                continue
            if only and name != only:
                continue
            if not force and same_version(installed.get(name, ""), entry["version"]):
                log(f"  {name}: already at {entry['version']} — skipping (force to reapply)")
                result.skipped.append(name)
                continue
            plan.append(name)

        if not plan:
            log("\nnothing to do.")
            return result
        log(f"\nplan: {' → '.join(plan)}")
        if dry_run:
            log("dry run: nothing written.")
            return result

        await _quiet_the_stream(conn, g, is_error, fmt_error, log)

        needs_reset = False
        for name in plan:
            if name == "esp32c6":
                log(
                    "esp32c6: skipped — the C6 updates itself over WiFi "
                    "(POST /api/ota/upload); it has no wired path through the "
                    "M7. See docs/ARCHITECTURE.md §1."
                )
                result.skipped.append(name)
                continue
            image = bundle.read_image(name)
            if name == "m4":
                needs_reset |= await _apply_m4(
                    conn,
                    image,
                    bundle.image_sig("m4"),
                    g=g,
                    is_error=is_error,
                    fmt_error=fmt_error,
                    log=log,
                )
            elif name == "m7":
                needs_reset |= await _apply_m7(
                    conn, image, is_error=is_error, fmt_error=fmt_error, log=log
                )
            result.applied.append(name)

        if not needs_reset:
            return result

        conn = await _reset_and_reconnect(conn, target, log)
        after = await _fw_versions(conn, g, is_error, fmt_error)

        # Updating BOTH cores in one cycle costs the M4 its IPC bind, and it takes
        # a second reset to get it back.
        #
        # Why: the M4 has no bootloader. It self-delays ~7 s after a chip reset and
        # binds RPMSG exactly once, because static vrings cannot re-bind against an
        # already-running host (see app_m7/src/platform/ipc.c). That handshake
        # assumes both cores boot together. When the M7 is also updated, MCUboot
        # first copies ~646 KB secondary→primary before the M7 app starts at all,
        # so the M7 arrives late, the M4's one attempt has already gone, and the M7
        # reports "M4 IPC bind timeout -- no vitals". The device runs, but produces
        # no HR/SpO2 until it is reset again.
        #
        # The second reset has no image to copy (slot 1 is already consumed, swap
        # type none), so the cores boot together and bind normally. Reproduced and
        # verified on v5, F8 2026-07-25. An M4-only update never hits this.
        if "m4" in plan and "m7" in plan:
            m4_want = (bundle.images().get("m4") or {}).get("version", "")
            if not same_version(after.get("m4") or "", m4_want):
                log(
                    "\nM4 did not bind after the combined update (expected — "
                    "MCUboot's image copy delayed the M7 past the M4's one-shot "
                    "RPMSG bind). Resetting once more to re-pair the cores…"
                )
                conn = await _reset_and_reconnect(conn, target, log)
                after = await _fw_versions(conn, g, is_error, fmt_error)

        result.versions_after = after
        log("\nafter reset:")
        ok = True
        for name in plan:
            entry = bundle.images().get(name)
            if not entry or name == "esp32c6":
                continue
            got = after.get(name) or "?"
            match = same_version(got, entry["version"])
            ok &= match
            log(
                f"  {name:<8} {got:<14} "
                f"{'OK' if match else 'MISMATCH — expected ' + entry['version']}"
            )

        # A matching version string does NOT prove an install: reapplying the
        # running image, or an upload MCUboot declined, leaves the old version
        # reported and "OK" printed. For the M7 the hash settles it -- slot 0 must
        # now hold the image we shipped.
        if "m7" in plan:
            want = mcuboot_image_hash(bundle.read_image("m7"))
            live = (await _m7_slot_hashes(conn, is_error)).get(0)
            if want and live:
                if live == want:
                    log(
                        f"  m7 slot0 hash {live.hex()[:16]}… matches the bundle "
                        "— image is installed and running"
                    )
                else:
                    ok = False
                    log(
                        f"  m7 slot0 hash {live.hex()[:16]}… does NOT match the "
                        f"bundle's {want.hex()[:16]}… — MCUboot did not install "
                        "the uploaded image (not newer? not marked pending?)"
                    )
        result.ok = ok
        return result
    finally:
        try:
            await conn.client.__aexit__(None, None, None)
        except Exception:  # noqa: BLE001 -- already gone after a reset
            pass
