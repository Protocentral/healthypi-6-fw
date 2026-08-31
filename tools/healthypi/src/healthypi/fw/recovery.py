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
from .update import Log, Target, UpdateError, _stdout


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
    log("    hpi fw recover --port <recovery-port> --bundle <file>")
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
                    "  Run `hpi fw update` to update normally, or "
                    "`hpi fw enter-recovery` first if that is what you meant."
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

    log("Reset sent. The device should now boot the application and enumerate")
    log("as the two-port composite again. If the M4 also needs updating, run")
    log("`hpi fw update` against the application's CDC 1 port.")
