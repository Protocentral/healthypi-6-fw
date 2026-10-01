# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT

"""MCUboot serial recovery -- entering it, and flashing from inside it.

Overwrite-only MCUboot has no revert, so recovery is the floor beneath a failed
update: the bootloader exposes its own SMP img group over a single USB CDC port
and will take an M7 image with nothing else running.

Two directions:

* :func:`enter_recovery` -- ask a *running application* to reboot into the
  bootloader (group-64 ``0x00A5``, a retained flag in backup SRAM).
* :func:`recover` -- write the M7 image to a device that is *already* in
  recovery. Only the M7: in recovery nothing else is running, so the group-64
  M4 path does not exist. Update the M4 afterwards, from the recovered app.

Ladder and rationale: ``docs/ARCHITECTURE.md`` §9.
"""

from __future__ import annotations

import asyncio
import logging
import time

from .bundle import Bundle
from .update import Log, Target, UpdateError, _fw_versions, _stdout, same_version

#: How long the recovered application gets to boot and enumerate. MCUboot
#: validates the new image first; the M4's IPC bind takes ~7-10 s after that.
_APP_RETURN_S = 45.0


async def enter_recovery(target: Target, *, log: Log = _stdout) -> None:
    """Reboot a running device into MCUboot serial recovery."""
    from ..smp.group64 import fmt_error, g, is_error
    from ..transport import serial_smp

    conn = await serial_smp.connect(
        target.port, baud=target.baud, frame_size=target.frame_size
    )
    try:
        state = await conn.request(g.enter_recovery())
        if is_error(state):
            raise UpdateError(
                f"recovery state read failed: {fmt_error(state)}\n"
                "  Command 0x00A5 exists only in the signed build "
                "(CONFIG_HPI_RECOVERY_MODE)."
            )
        if not state.av:
            raise UpdateError(
                "this firmware cannot enter recovery — it was built without a "
                "bootloader, so there is nothing to reboot into.\n"
                "  Only the signed build (scripts/build.sh signed) supports it."
            )

        resp = await conn.request(g.enter_recovery_write(arm=True, rst=True))
        if is_error(resp):
            raise UpdateError(f"enter_recovery failed: {fmt_error(resp)}")
    finally:
        try:
            await conn.client.__aexit__(None, None, None)
        except Exception:  # noqa: BLE001 -- the device is rebooting
            pass

    log("Recovery armed; the device is rebooting into MCUboot serial recovery.")
    log('It will re-enumerate as a SINGLE CDC port named "HealthyPi 6 Recovery"')
    log("— NOT the port you just used. Find it, then:")
    log("    healthypi fw recover --port <recovery-port> --bundle <file>")
    log("If you pick the wrong port, recover says so rather than failing")
    log("obscurely; it identifies the mode from the protocol, not the USB IDs.")


class _DropParamProbe(logging.Filter):
    """Silence one expected smpclient warning.

    smpclient probes ``os mgmt_params`` on connect and logs a WARNING with a raw
    error dump when it is unsupported. MCUboot's serial recovery does not
    implement that command, so a perfectly healthy recovery printed a scary
    "Error reading MCUMgr parameters: ... rc=ENOTSUP" in the middle of the one
    operation a panicking user runs (F8, 2026-07-25). Every other smpclient
    warning still gets through; this is the only path where the probe is
    expected to fail.
    """

    def filter(self, record: logging.LogRecord) -> bool:
        return "Error reading MCUMgr parameters" not in record.getMessage()


async def recover(bundle: Bundle, target: Target, *, pubkey=None, log: Log = _stdout) -> None:
    """Flash the M7 from within the bootloader's serial recovery mode."""
    from smpclient.requests.os_management import ResetWrite

    from ..smp.group64 import g, is_error
    from ..transport import serial_smp

    bundle.verify(pubkey)
    image = bundle.read_image("m7")

    logging.getLogger("smpclient").addFilter(_DropParamProbe())

    log(f"recovery: writing M7 {len(image)} B via the bootloader img group")
    conn = await serial_smp.connect(
        target.port, baud=target.baud, frame_size=target.frame_size
    )
    try:
        # Which side of the reboot is this port on? The application answers
        # group 64; MCUboot's serial recovery does not implement it. That is a
        # protocol fact, so it holds regardless of USB VID/PID -- which the
        # application and the bootloader deliberately share, there being one
        # allocated pid.codes PID. Getting this wrong used to mean an obscure
        # failure mid-upload; now it is one line before anything is written.
        try:
            probe = await asyncio.wait_for(conn.request(g.device_info()), timeout=3)
            if not is_error(probe):
                raise UpdateError(
                    f"{conn.port} is the APPLICATION, not the bootloader.\n"
                    "  Run `healthypi fw update` to update normally, or "
                    "`healthypi fw enter-recovery` first if that is what you meant."
                )
        except asyncio.TimeoutError:
            pass  # no group 64 -> bootloader, which is what we want

        t0 = time.monotonic()
        async for off in conn.client.upload(image, slot=0):
            print(
                f"\r  upload {off}/{len(image)} B ({100.0 * off / len(image):.0f}%)",
                end="",
                flush=True,
            )
        dt = time.monotonic() - t0
        print(f"\n  uploaded in {dt:.0f}s")
        await conn.request(ResetWrite())
    finally:
        try:
            await conn.client.__aexit__(None, None, None)
        except Exception:  # noqa: BLE001
            pass

    log("  reset sent; waiting for the application to come back…")
    await _confirm(bundle, target, log)


async def _confirm(bundle: Bundle, target: Target, log: Log) -> None:
    """Find the recovered application and check it runs the bundle's M7.

    The recovery port disappears with the reset and the application comes back
    on different ports, so this looks for whichever port answers group 64.
    Without it, recover ended at "the device should now boot" -- the one step
    of a rescue that most needs confirming was the one left unchecked.
    """
    # Probing ports that are not up yet (or are CDC 0, which never answers)
    # times out by design; smpclient logs each of those as an ERROR. They are
    # expected misses here, not failures, so keep them off the user's screen.
    smp_log = logging.getLogger("smpclient")
    saved_level = smp_log.level
    smp_log.setLevel(logging.CRITICAL)
    try:
        await _find_and_check(bundle, target, log)
    finally:
        smp_log.setLevel(saved_level)


async def _find_and_check(bundle: Bundle, target: Target, log: Log) -> None:
    from ..smp.group64 import fmt_error, g, is_error
    from ..transport import serial_smp

    images = bundle.images()
    want_m7 = images["m7"]["version"]
    deadline = time.monotonic() + _APP_RETURN_S
    while time.monotonic() < deadline:
        await asyncio.sleep(2.0)
        for port in serial_smp.candidates():
            try:
                conn = await serial_smp.connect(
                    port, baud=target.baud, frame_size=target.frame_size, timeout_s=2.0
                )
            except Exception:  # noqa: BLE001 -- not this port, or not yet
                continue
            try:
                try:
                    probe = await asyncio.wait_for(conn.request(g.device_info()), timeout=3)
                except Exception:  # noqa: BLE001
                    continue
                if is_error(probe):
                    continue
                versions = await _fw_versions(conn, g, is_error, fmt_error)
                # The M7 learns the M4's version over IPC, which binds ~7-10 s
                # after boot. Judge the M4 only once it has had that long.
                m4_deadline = time.monotonic() + 15.0
                while not versions.get("m4") and time.monotonic() < m4_deadline:
                    await asyncio.sleep(2.0)
                    versions = await _fw_versions(conn, g, is_error, fmt_error)
            finally:
                try:
                    await conn.client.__aexit__(None, None, None)
                except Exception:  # noqa: BLE001
                    pass
            m7 = versions.get("m7", "")
            log(f"  application is back on {port}: m7 {m7 or '?'}, m4 {versions.get('m4') or '?'}")
            if not same_version(m7, want_m7):
                raise UpdateError(
                    f"recovery wrote the bundle's M7 {want_m7}, but the application "
                    f"reports {m7 or 'no version'}. The image may not have been "
                    "accepted; enter recovery again and retry."
                )
            log(f"  m7 {m7} OK — recovered.")
            want_m4 = (images.get("m4") or {}).get("version")
            if want_m4 and not same_version(versions.get("m4", ""), want_m4):
                log(
                    f"\nThe M4 is not at the bundle's {want_m4} — recovery writes the "
                    "M7 only. Bring it into line from the application:\n"
                    f"  healthypi fw update --port {port} --bundle <file> --only m4"
                )
            return
    raise UpdateError(
        f"the image was written, but no application answered within "
        f"{_APP_RETURN_S:.0f} s. If the unit came back up as \"HealthyPi 6 "
        "Recovery\", MCUboot did not accept the image; retry the recovery. "
        "Otherwise check `healthypi device versions` once it has finished booting."
    )
