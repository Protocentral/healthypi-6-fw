# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT

"""ESP32-C6 firmware update, relayed by the M7 (group-64 ``espfw_*``).

The host half of ``docs/MCUMGR_COMMANDS.md`` §5.12, followed step for step:

1. ``espfw_status`` -- the commands exist (``ENOTSUP`` otherwise) and ``busy``
   is false.
2. ``espfw_begin {len, sha, sig}``; 268 = signature refused, nothing sent.
3. poll ``espfw_status`` ~1 Hz until ``st`` = 2 receiving (8 = failed), ≤15 s.
4. ``espfw_chunk`` in order. 272: resend the SAME chunk. 260: read ``rx`` and
   resume there. 273: the session is over -- start again from ``begin``. Never
   pause 30 s (the C6 and the M7 both give up).
5. ``espfw_commit`` exactly once. A lost reply is NOT a reason to resend it.
6. poll ``espfw_status`` ~1 Hz until ``st`` is 6 done / 7 rolled back /
   8 failed; 9 timeout is final only after 45 s, and may still turn into 6/7.

Every ``espfw_*`` request runs with the catalog's 5 s timeout, which
:class:`~healthypi.transport.serial_smp.Connection` applies by itself -- nothing
here passes a timeout, so nothing here can pass the wrong one.

A C6 image keeps its slot only if the C6 hears from the M7 within 60 s of
restarting into it; any reset of the M7 (which holds the C6 in reset) or of the
C6 before then rolls it back. That is why the C6 goes last in a bundle update,
and why the reset-type commands are refused while ``busy`` -- see
:func:`busy_reason`.

**Not yet run on hardware** -- covered offline against a fake device only.
"""

from __future__ import annotations

import asyncio
import hashlib
import time
from dataclasses import dataclass
from typing import Callable

from ..smp import catalog
from .errors import (
    EXIT_C6_NEEDS_USB_FLASH,
    EXIT_C6_ROLLED_BACK,
    EXIT_C6_UNCONFIRMED,
    EXIT_OK,
    EXIT_UNVERIFIED,
    UpdateError,
)

Log = Callable[[str], None]

# enum hpi_espfw_state
IDLE, PREPARING, RECEIVING, COMMITTING, REBOOTING, VERIFYING = range(6)
DONE, ROLLED_BACK, FAILED, TIMEOUT = 6, 7, 8, 9
# enum hpi_espfw_err (the ones a host acts on)
ERR_SIG_INVALID, ERR_LINK, ERR_NO_OTA, ERR_C6_REFUSED = 1, 2, 3, 4
ERR_ABORTED, ERR_HOST_IDLE = 9, 10
# HealthyBridge OTA status codes (espfw_status.c6_err)
C6_RUNNING_UNCONFIRMED, C6_NO_SLOT = 0x08, 0x0F

# group-64 codes the sequence branches on
E_NOT_READY, E_HW_FAULT, E_TRANSFER_INVALID = 256, 257, 260
E_IMAGE_INVALID, E_IMAGE_TOO_LARGE, E_BUSY = 268, 269, 270
E_ESP_NO_REPLY, E_ESP_REFUSED, E_ESP_UPDATE_ACTIVE, E_IMAGE_INCOMPLETE = 272, 273, 274, 275
# protocol-wide MGMT_ERR codes
MGMT_ENOTSUP, MGMT_EACCESSDENIED = 8, 11

#: Poll cadence for espfw_status ("about once a second").
POLL_S = 1.0
#: begin -> st 2 receiving: the M7 powers the C6 and opens the transfer.
PREPARE_BUDGET_S = 15.0
#: commit -> a final state. The device gives the restart 10 s and the self-test
#: 20 s; 45 s is the documented host budget.
OUTCOME_BUDGET_S = 45.0
#: How long to wait for a C6 we powered to report its version.
VERSION_PROBE_S = 10.0
#: Consecutive no-answer retries of ONE chunk (272, or no reply at all) before
#: giving up. At the 5 s request timeout, 5 lost replies are 25 s -- inside the
#: 30 s the device waits for a chunk before abandoning the session.
MAX_CHUNK_RETRIES = 5
#: Resyncs (260) tolerated in one transfer.
MAX_RESYNCS = 5
#: A 273 ends the session; start again from begin this many times.
MAX_RESTARTS = 1

#: Bytes an espfw_chunk request spends on SMP header + CBOR framing; the same
#: {"off", "data"} shape as m4fw_chunk, so the same margin.
CHUNK_OVERHEAD = 48

HB_FLASH_URL = "https://github.com/Protocentral/healthybridge-esp32#flashing-a-healthypi-6"

# Indirection so the offline tests can run the 1 Hz polling on a virtual clock.
_now = time.monotonic


async def _sleep(seconds: float) -> None:
    await asyncio.sleep(seconds)


def _stdout(msg: str) -> None:
    print(msg, flush=True)


def _progress(off: int, total: int) -> None:
    pct = 100.0 * off / max(total, 1)
    print(f"\r  upload {off}/{total} B ({pct:.0f}%)", end="", flush=True)


# ---------------------------------------------------------------------------
# reading replies
# ---------------------------------------------------------------------------


def group_code(resp) -> int | None:
    """The group-64 code of an error reply, or None (call only on an error)."""
    err = getattr(resp, "err", None)
    if err is None:
        return None
    try:
        grp = getattr(err, "group", None)
        if grp is not None and int(grp) != catalog.GROUP_ID:
            return None
        return int(err.rc)
    except (TypeError, ValueError):
        return None


def mgmt_code(resp) -> int | None:
    """The protocol-wide MGMT_ERR code of an error reply, or None."""
    rc = getattr(resp, "rc", None)
    try:
        return int(rc) if rc is not None else None
    except (TypeError, ValueError):
        return None


def state_name(st: int) -> str:
    return catalog.ESPFW_STATES.get(st, f"state {st}")


def err_name(err: int) -> str:
    return catalog.ESPFW_ERRORS.get(err, f"err {err}")


def c6_err_name(code: int) -> str:
    return catalog.ESPFW_C6_ERRORS.get(code, f"0x{code:02X}")


def chunk_size(conn) -> int:
    """Bytes of image per espfw_chunk on this connection."""
    max_unencoded = getattr(getattr(conn, "transport", None), "max_unencoded_size", 256)
    return max(32, min(catalog.ESPFW_MAX_CHUNK, max_unencoded - CHUNK_OVERHEAD))


async def read_status(conn, g, is_error):
    """The ``espfw_status`` reply, or None when this firmware has no espfw_*.

    A build without ``CONFIG_HPI_ESP32_UPDATE`` -- a development build, or any
    M7 firmware older than the commands -- answers ENOTSUP. espfw_status is not
    lock-gated and never fails otherwise, so any error means "not here".
    """
    resp = await conn.request(g.espfw_status())
    return None if is_error(resp) else resp


def describe_status(s) -> str:
    """espfw_status as lines a person can read (``healthypi fw esp32c6-status``)."""
    img = {0: "valid", 1: "pending verify", 2: "new", 3: "invalid", 4: "aborted",
           5: "undefined", 255: "unknown"}
    lines = [
        f"session   {state_name(s.st)} ({s.st})"
        + (f", err {err_name(s.err)} ({s.err})" if s.err else ""),
        f"transfer  {s.rx}/{s.len} B relayed and acknowledged",
        f"C6        version {s.ver or 'unknown'}, slot "
        f"{s.slot if s.slot != 255 else '?'}, running image {img.get(s.img, s.img)}, "
        f"other slot {img.get(s.other, s.other)}",
    ]
    if s.c6_err:
        lines.append(f"C6 error  {c6_err_name(s.c6_err)} ({s.c6_err})")
    lines.append(
        "busy      YES -- do not reset the device, enter recovery or `conn disable`"
        if s.busy else "busy      no"
    )
    if s.img == 255 and not s.ver:
        lines.append("(C6 fields unknown: it is held in reset, the boot default -- "
                     "`healthypi conn enable --radios none` powers it)")
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# messages a user acts on
# ---------------------------------------------------------------------------


def explain_busy(s) -> str:
    """Why a reset-type command must wait, from an espfw_status with busy set."""
    if PREPARING <= s.st <= VERIFYING:
        what = (f"an ESP32-C6 firmware update is in progress "
                f"({state_name(s.st).lower()}, {s.rx}/{s.len} B)")
    else:
        what = (f"the ESP32-C6's new firmware ({s.ver or 'version unknown'}) is "
                "running but still proving itself (pending verification)")
    return (
        f"{what}.\n"
        "  Resetting now -- the device (os reset, entering recovery) or the C6 "
        "(conn disable) -- would roll the C6 update back, so the device refuses "
        "(274) and this tool does not ask.\n"
        "  Wait until `healthypi fw esp32c6-status` shows busy: no (normally "
        "within a minute), then retry."
    )


#: For a 274 seen without a status to hand.
EXPLAIN_274 = (
    "the device refused (274 ESP_UPDATE_ACTIVE): an ESP32-C6 firmware update is in "
    "progress, or its new image is still proving itself, and a reset now would "
    "roll it back.\n"
    "  Wait until `healthypi fw esp32c6-status` shows busy: no (normally within "
    "a minute), then retry."
)


async def busy_reason(conn, g, is_error) -> str | None:
    """Text explaining why a reset must wait, or None when it need not.

    None also when the firmware has no espfw_* -- such a unit cannot be
    mid-C6-update, so nothing guards its resets.
    """
    try:
        s = await read_status(conn, g, is_error)
    except Exception:  # noqa: BLE001 -- no answer is no guard; the device still refuses
        return None
    if s is None or not s.busy:
        return None
    return explain_busy(s)


def is_update_active(resp, is_error) -> bool:
    """Is this reply the device's 274 refusal?"""
    return bool(is_error(resp)) and group_code(resp) == E_ESP_UPDATE_ACTIVE


def no_espfw_message(m7_version: str, m7_just_installed: bool) -> str:
    """ENOTSUP from espfw_status, worded for where the run is."""
    if m7_just_installed:
        return (
            f"esp32c6: the M7 firmware now running ({m7_version or '?'}) has no "
            "ESP32-C6 update commands (espfw_status answers ENOTSUP), although it "
            "was just installed from this bundle. The bundle's M7 was built "
            "without CONFIG_HPI_ESP32_UPDATE -- not a scripts/release.sh build. "
            "The M4 and M7 steps above are complete; the C6 was not touched."
        )
    return (
        f"esp32c6: this unit's M7 firmware ({m7_version or '?'}) has no ESP32-C6 "
        "update commands (espfw_status answers ENOTSUP), and this bundle does not "
        "install a newer M7.\n"
        "  The C6 is updated THROUGH the M7, so the M7 has to be updated first. "
        "A bundle whose M7 is newer does both in one run (M7, reset, then the C6).\n"
        "  A development build (scripts/build.sh m7) never has these commands: "
        "flash the signed build."
    )


def needs_usb_flash_message(detail: str) -> str:
    return (
        f"esp32c6: {detail}\n"
        "  Its firmware predates the USB update path, so it needs ONE flash over "
        "its own USB port, with the case open. Every update after that goes "
        "through `healthypi fw update`.\n"
        "    1. healthypi conn enable --radios none    (release the C6 from reset)\n"
        "    2. connect the C6's USB port; in healthybridge-esp32:\n"
        "         ./hp6.sh && ./flash_hp6.sh <PORT>\n"
        "       (a full erase plus the merged image -- required for the first move "
        "onto the A/B partition layout; it erases stored Wi-Fi credentials)\n"
        f"  Instructions: {HB_FLASH_URL}"
    )


def sig_refused_message(cur: str) -> str:
    return (
        "esp32c6: the device REFUSED the image's signature (268 IMAGE_INVALID).\n"
        "  The M7 checks the C6 image against the release key compiled into it, "
        "and this bundle was signed with a different key -- typically a dev-key "
        "bundle on a release unit, or the reverse.\n"
        f"  Nothing was sent to the C6; it still runs {cur or 'its current firmware'}."
    )


def _unchanged(cur: str) -> str:
    return (
        "  The C6's boot slot is unchanged: it still runs "
        f"{cur or 'its previous firmware'}. Retry with "
        "`healthypi fw update --only esp32c6 --bundle …`."
    )


def failed_error(s, cur: str) -> UpdateError:
    """An UpdateError for a session that ended in st 8 FAILED (or aborted)."""
    err = s.err
    if err == ERR_NO_OTA:
        return UpdateError(
            needs_usb_flash_message(
                "the C6 answers the M7 but not the firmware-update protocol "
                "(espfw err 3 NO_OTA)."
            ),
            EXIT_C6_NEEDS_USB_FLASH,
        )
    if err == ERR_C6_REFUSED and s.c6_err == C6_NO_SLOT:
        return UpdateError(
            needs_usb_flash_message(
                "the C6 has no second OTA slot (C6 NO_SLOT): it is still on the old "
                "single-partition layout."
            ),
            EXIT_C6_NEEDS_USB_FLASH,
        )
    if err == ERR_SIG_INVALID:
        return UpdateError(sig_refused_message(cur))
    reasons = {
        ERR_LINK: "the M7 could not power the C6, or it never answered. "
                  "`healthypi conn status` shows the link (3 = fault).",
        5: "the C6 stopped answering mid-session (NO_REPLY).",
        6: "a C6 reply disagreed with the transfer (PROTOCOL).",
        7: "the relayed bytes do not hash to the signed digest (DIGEST) -- the "
           "C6 never saw COMMIT.",
        8: "commit came before every byte was relayed (INCOMPLETE).",
        ERR_ABORTED: "the session was aborted (espfw_abort).",
        ERR_HOST_IDLE: "the device gave up after 30 s without a chunk (HOST_IDLE) "
                       "-- was this host suspended, or the cable disturbed?",
        11: "the M7's crypto library failed (CRYPTO).",
        15: "the C6 restarted on its old image without switching (NOT_SWITCHED).",
    }
    if err == ERR_C6_REFUSED:
        why = f"the C6 refused: {c6_err_name(s.c6_err)} ({s.c6_err})."
        if s.c6_err == C6_RUNNING_UNCONFIRMED:
            why += (" Its RUNNING image is itself still pending verification; wait "
                    "until `healthypi fw esp32c6-status` shows busy: no, then retry.")
    else:
        why = reasons.get(err, f"{err_name(err)} ({err}).")
    return UpdateError(f"esp32c6: the update FAILED -- {why}\n{_unchanged(cur)}")


# ---------------------------------------------------------------------------
# version
# ---------------------------------------------------------------------------


async def learn_version(conn, g, is_error, log: Log) -> tuple[str, bool]:
    """The C6's running version, powering it (radios off) if it must.

    Returns ``(version, powered_by_us)``. The version is cached by the M7 only
    while the C6 is powered: it is ``""`` while the M7 holds it in reset, which
    is how every boot starts. The least intrusive way to learn it is
    ``conn_enable`` with no radio -- what `healthypi conn enable --radios none`
    does: the C6 boots, the M7 asks it for its version (HealthyBridge
    GET_VERSION, and OTA STATUS from the update service), and no radio starts,
    so the USB-brown-out reason for keeping radios off at boot does not apply.
    Beginning an update session would power it too, but would also open a
    transfer on the C6 just to read a number. A caller that powered it puts it
    back with :func:`restore_power`.
    """
    cs = await conn.request(g.conn_status())
    if is_error(cs):
        return "", False
    powered = False
    if cs.link == 0:
        resp = await conn.request(g.conn_enable(radios=0))
        if is_error(resp):
            return "", False
        powered = True
        log("  esp32c6: version unknown (the C6 is held in reset, the boot default) "
            "-- powering it, radios off, to ask")
    deadline = _now() + VERSION_PROBE_S
    while True:
        s = await read_status(conn, g, is_error)
        ver = s.ver if s is not None else ""
        if not ver:
            fv = await conn.request(g.fw_versions())
            ver = "" if is_error(fv) else (fv.espfw or "")
        if ver:
            return ver, powered
        if _now() >= deadline:
            return "", powered
        cs = await conn.request(g.conn_status())
        if not is_error(cs) and cs.link == 3:
            return "", powered  # powered but silent: a fault, not worth waiting
        await _sleep(POLL_S)


async def restore_power(conn, g, is_error, log: Log) -> None:
    """Put a C6 this tool powered back into reset -- unless that would roll it back."""
    try:
        s = await read_status(conn, g, is_error)
        if s is not None and s.busy:
            log("  esp32c6: left powered -- its image is not confirmed yet, and "
                "a reset would roll it back")
            return
        resp = await conn.request(g.conn_disable())
    except Exception as exc:  # noqa: BLE001 -- best effort, never fails the update
        log(f"  note: could not put the C6 back into reset ({exc})")
        return
    if is_error(resp):
        log("  note: the C6 was left powered (conn_disable refused); "
            "`healthypi conn disable` puts it back into reset")
    else:
        log("  esp32c6: back in reset, as it was")


# ---------------------------------------------------------------------------
# the update
# ---------------------------------------------------------------------------


@dataclass
class C6Result:
    applied: bool = False
    skipped: bool = False
    exit_code: int = EXIT_OK
    version_after: str = ""
    message: str = ""


class _Restart(Exception):
    """273: the C6 ended the session; start over from begin."""

    def __init__(self, status):
        super().__init__("restart")
        self.status = status


async def _abort_quietly(conn, g) -> None:
    try:
        await conn.request(g.espfw_abort())
    except Exception:  # noqa: BLE001 -- the device abandons it after 30 s anyway
        pass


async def _status_tolerant(conn, g, is_error):
    """espfw_status, or None on a lost reply (keep polling)."""
    try:
        return await read_status(conn, g, is_error)
    except (TimeoutError, asyncio.TimeoutError):
        return None


async def _begin(conn, g, is_error, fmt_error, image: bytes, sha: bytes, sig: bytes,
                 cur: str, log: Log) -> None:
    try:
        resp = await conn.request(g.espfw_begin(len=len(image), sha=sha, sig=sig))
    except (TimeoutError, asyncio.TimeoutError):
        # begin answers at once; a lost reply may still have started our
        # session. Resending would answer BUSY, so look instead.
        s = await _status_tolerant(conn, g, is_error)
        if s is not None and s.st in (PREPARING, RECEIVING) and s.len == len(image):
            log("  (begin's reply was lost, but the session is open)")
            return
        raise UpdateError(
            "esp32c6: no reply to espfw_begin, and the device shows no session for "
            "this image. Nothing was sent to the C6; re-run the update."
        ) from None
    if not is_error(resp):
        return
    code = group_code(resp)
    if code == E_IMAGE_INVALID:
        raise UpdateError(sig_refused_message(cur))
    if code == E_BUSY:
        s = await _status_tolerant(conn, g, is_error)
        raise UpdateError("esp32c6: " + (explain_busy(s) if s is not None and s.busy
                                         else "the device reports an update already in "
                                              "progress (270 BUSY)."))
    if code == E_IMAGE_TOO_LARGE:
        raise UpdateError(f"esp32c6: the device refused a {len(image)} B image as too "
                          "large (269). Nothing was sent.")
    if code == E_HW_FAULT:
        raise UpdateError("esp32c6: the M7 could not take the link to the C6 "
                          "(257 HW_FAULT). Check `healthypi conn status`. Nothing was sent.")
    if mgmt_code(resp) == MGMT_EACCESSDENIED:
        raise UpdateError("esp32c6: the device is LOCKED, and writing the C6's firmware "
                          "requires it to be unlocked (docs/DEVICE_LOCK.md).")
    raise UpdateError(f"esp32c6: espfw_begin refused: {fmt_error(resp)}")


async def _wait_receiving(conn, g, is_error, cur: str, log: Log) -> None:
    deadline = _now() + PREPARE_BUDGET_S
    seen = None
    while True:
        s = await _status_tolerant(conn, g, is_error)
        if s is not None:
            if s.st != seen:
                seen = s.st
                if s.st == PREPARING:
                    log("  preparing: the M7 is powering the C6 and opening the transfer")
            if s.st == RECEIVING:
                log(f"  C6 ready: running {s.ver or '?'} from slot {s.slot}")
                return
            if s.st == FAILED or (s.st == IDLE and s.err):
                raise failed_error(s, cur)
        if _now() >= deadline:
            await _abort_quietly(conn, g)
            raise UpdateError(
                f"esp32c6: the C6 was not ready to receive within "
                f"{PREPARE_BUDGET_S:.0f} s (state "
                f"{state_name(s.st) if s is not None else 'unknown'}). The session was "
                f"aborted.\n{_unchanged(cur)}"
            )
        await _sleep(POLL_S)


async def _send(conn, g, is_error, fmt_error, image: bytes, cur: str, log: Log,
                progress) -> None:
    size = len(image)
    chunk = chunk_size(conn)
    off = 0
    retries = 0
    resyncs = 0
    t0 = _now()
    while off < size:
        piece = image[off:off + chunk]
        try:
            resp = await conn.request(g.espfw_chunk(off=off, data=piece))
        except (TimeoutError, asyncio.TimeoutError):
            resp = None
        if resp is None or (is_error(resp) and group_code(resp) == E_ESP_NO_REPLY):
            # No answer from the C6 (272), or none from the device at all. Either
            # way: resend the SAME chunk -- the device acknowledges a repeat of
            # the chunk it accepted last without relaying it twice.
            retries += 1
            if retries > MAX_CHUNK_RETRIES:
                await _abort_quietly(conn, g)
                raise UpdateError(
                    f"\nesp32c6: no answer for the chunk at +{off} after "
                    f"{MAX_CHUNK_RETRIES} retries "
                    f"({'the C6 is not answering the M7' if resp is not None else 'the device is not answering -- cable?'}).\n"
                    "  The transfer was abandoned before COMMIT (the device drops it "
                    f"after 30 s regardless).\n{_unchanged(cur)}"
                )
            continue
        if is_error(resp):
            code = group_code(resp)
            if code == E_TRANSFER_INVALID:
                s = await _status_tolerant(conn, g, is_error)
                if s is None or s.st != RECEIVING:
                    if s is not None and s.st in (FAILED, IDLE):
                        raise failed_error(s, cur)
                    raise UpdateError(f"\nesp32c6: chunk at +{off} refused (260) and the "
                                      f"session is not receiving.\n{_unchanged(cur)}")
                resyncs += 1
                if resyncs > MAX_RESYNCS:
                    await _abort_quietly(conn, g)
                    raise UpdateError(f"\nesp32c6: the device kept refusing chunk "
                                      f"offsets (260 x{resyncs}).\n{_unchanged(cur)}")
                log(f"\n  device expects +{s.rx}, not +{off}: resuming there")
                off = s.rx
                retries = 0
                continue
            if code == E_ESP_REFUSED:
                raise _Restart(await _status_tolerant(conn, g, is_error))
            if code == E_NOT_READY:
                s = await _status_tolerant(conn, g, is_error)
                if s is not None and s.st in (FAILED, IDLE):
                    raise failed_error(s, cur)
            await _abort_quietly(conn, g)
            raise UpdateError(f"\nesp32c6: chunk at +{off} refused: {fmt_error(resp)}\n"
                              f"{_unchanged(cur)}")
        retries = 0
        nxt = int(resp.off)
        if not off < nxt <= size:
            await _abort_quietly(conn, g)
            raise UpdateError(f"\nesp32c6: the device answered next offset +{nxt} for "
                              f"a chunk at +{off} of {size}.\n{_unchanged(cur)}")
        off = nxt
        progress(off, size)
    dt = max(_now() - t0, 0.001)
    log(f"\n  relayed in {dt:.0f}s ({size / 1024 / dt:.1f} KB/s)")


async def _commit(conn, g, is_error, fmt_error, cur: str, log: Log) -> None:
    log("  commit (length + digest on the M7, then the C6 verifies and switches slots)")
    try:
        resp = await conn.request(g.espfw_commit())
    except (TimeoutError, asyncio.TimeoutError):
        # NEVER resend: a second COMMIT is not idempotent on the C6 side. The
        # outcome is in espfw_status either way.
        log("  no reply to commit -- not resending it; reading the outcome instead")
        return
    if not is_error(resp):
        return
    code = group_code(resp)
    if code == E_IMAGE_INCOMPLETE:
        why = "the device says not every byte was relayed (275 IMAGE_INCOMPLETE)"
    elif code == E_IMAGE_INVALID:
        why = "the relayed bytes do not hash to the signed digest (268)"
    elif code == E_NOT_READY:
        s = await _status_tolerant(conn, g, is_error)
        if s is not None and s.st in (FAILED, IDLE):
            raise failed_error(s, cur)
        why = f"commit refused: {fmt_error(resp)}"
    else:
        why = f"commit refused: {fmt_error(resp)}"
    raise UpdateError(f"esp32c6: {why}. The C6 never saw COMMIT.\n{_unchanged(cur)}")


async def _outcome(conn, g, is_error, log: Log):
    """Poll to a final state. Returns the last status seen (None if none)."""
    t_commit = _now()
    last = None
    seen = None
    while True:
        s = await _status_tolerant(conn, g, is_error)
        if s is not None:
            last = s
            if s.st != seen:
                seen = s.st
                note = {
                    COMMITTING: "committing",
                    REBOOTING: "C6 verifying the image and restarting into it",
                    VERIFYING: f"C6 up on {s.ver or '?'}, proving the new image "
                               "(do not reset or unplug)",
                    TIMEOUT: "not confirmed yet -- still waiting",
                }.get(s.st)
                if note:
                    log(f"  {note}")
            if s.st in (DONE, ROLLED_BACK, FAILED, IDLE):
                return s
        if _now() - t_commit >= OUTCOME_BUDGET_S:
            return last
        await _sleep(POLL_S)


async def update_c6(
    conn,
    image: bytes,
    sig: bytes,
    want: str,
    *,
    g,
    is_error,
    fmt_error,
    force: bool = False,
    status=None,
    m7_version: str = "",
    m7_just_installed: bool = False,
    log: Log = _stdout,
    progress=_progress,
) -> C6Result:
    """Bring the C6 to `image` (version `want`), following §5.12 exactly.

    Raises :class:`UpdateError` (with an exit ``code``) when the update cannot
    happen or failed with the C6 unchanged; returns a :class:`C6Result` for a
    completed, skipped, rolled-back or unconfirmed one.
    """
    from .update import _ver_tuple, same_version  # noqa: PLC0415 -- cycle

    s = status if status is not None else await read_status(conn, g, is_error)
    if s is None:
        raise UpdateError(no_espfw_message(m7_version, m7_just_installed))
    if s.busy:
        raise UpdateError("esp32c6: " + explain_busy(s))

    cur = s.ver
    if not cur:
        fv = await conn.request(g.fw_versions())
        cur = "" if is_error(fv) else (fv.espfw or "")
    powered = False
    if not cur and not force:
        cur, powered = await learn_version(conn, g, is_error, log)

    try:
        if cur and not force:
            if same_version(cur, want):
                log(f"esp32c6: already at {want} -- skipping (--force to reapply)")
                return C6Result(skipped=True, version_after=cur)
            if _ver_tuple(want) < _ver_tuple(cur):
                raise UpdateError(
                    f"esp32c6: the bundle's C6 {want} is OLDER than the installed {cur}.\n"
                    "  The C6 has no downgrade protection of its own, so this tool "
                    "refuses instead. Pass --force to install it anyway."
                )
        if not cur:
            log("esp32c6: running version unknown -- applying "
                + ("(--force)" if force else "(it could not be read)"))

        sha = hashlib.sha256(image).digest()
        log(f"ESP32-C6 : {len(image)} B → the C6's inactive OTA slot, relayed by the M7, "
            f"{chunk_size(conn)} B/chunk (signed), {cur or '?'} → {want}")
        attempt = 0
        while True:
            await _begin(conn, g, is_error, fmt_error, image, sha, sig, cur, log)
            await _wait_receiving(conn, g, is_error, cur, log)
            try:
                await _send(conn, g, is_error, fmt_error, image, cur, log, progress)
                break
            except _Restart as r:
                rs = r.status
                why = (f"{c6_err_name(rs.c6_err)} ({rs.c6_err})" if rs is not None
                       else "reason unknown")
                if attempt >= MAX_RESTARTS:
                    if rs is not None:
                        raise failed_error(rs, cur) from None
                    raise UpdateError(f"\nesp32c6: the C6 refused the transfer (273).\n"
                                      f"{_unchanged(cur)}") from None
                attempt += 1
                log(f"\n  the C6 ended the session (273: {why}) -- starting again "
                    "from begin")

        await _commit(conn, g, is_error, fmt_error, cur, log)
        final = await _outcome(conn, g, is_error, log)

        if final is not None and final.st == DONE:
            ok = same_version(final.ver, want)
            log(f"  esp32c6 {final.ver or '?':<14} "
                f"{'OK — running from slot ' + str(final.slot) + ' and VALID' if ok else 'MISMATCH — expected ' + want}")
            return C6Result(applied=True, exit_code=EXIT_OK if ok else EXIT_UNVERIFIED,
                            version_after=final.ver)
        if final is not None and final.st == ROLLED_BACK:
            msg = (f"esp32c6: the new image ({want}) failed its self-test after "
                   f"restarting and the C6 ROLLED BACK to {final.ver or cur or 'its previous image'}.\n"
                   "  The C6 is fine on its old firmware. The new image could not hold "
                   "the link to the M7 -- report it with this output.")
            log(msg)
            return C6Result(applied=False, exit_code=EXIT_C6_ROLLED_BACK,
                            version_after=final.ver, message=msg)
        if final is not None and final.st in (FAILED, IDLE):
            raise failed_error(final, cur)
        st = state_name(final.st) if final is not None else "no answer"
        msg = (
            f"esp32c6: the outcome was NOT confirmed within {OUTCOME_BUDGET_S:.0f} s "
            f"of commit (last state: {st}).\n"
            "  The C6 was left POWERED so that its new image is not rolled back by "
            "a reset. Do not reset or unplug the device yet.\n"
            "  The M7 keeps watching: re-check with\n"
            "    healthypi fw esp32c6-status --port <CDC1>\n"
            "  until busy: no -- 'done' means the new image is installed, 'rolled "
            "back' that the C6 returned to its old one."
        )
        log(msg)
        return C6Result(applied=False, exit_code=EXIT_C6_UNCONFIRMED,
                        version_after=getattr(final, "ver", ""), message=msg)
    finally:
        if powered:
            await restore_power(conn, g, is_error, log)
