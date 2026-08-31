# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT

"""The group-64 acceptance cases.

Scope is bounded by ``healthypi.smp.catalog``: a command the firmware declares
but never routes gets no case, for the same reason it gets no CLI verb -- a test
that can only assert ENOTSUP measures nothing.

Read-only by default. Anything that changes device state is tagged
``destructive`` and skipped unless asked for, so the default run is safe against
a unit that is mid-recording.
"""

from __future__ import annotations

import asyncio
import time

from ..smp import catalog
from .suite import REQ_SD, REQ_SIGNED, REQ_UNLOCK, case, fail, ok, skip


def _err(name, resp, fmt_error, what):
    return fail(name, f"{what}: {fmt_error(resp)}")


def _is_err(resp, group_id: int, code: int) -> bool:
    """True when this is the specific ``err`` map ``{group: g, rc: c}``.

    Both halves matter: an error code only means something inside the group that
    raised it, and this connection carries the stock groups as well as ours.
    """
    err = getattr(resp, "err", None)
    if err is None:
        return False
    try:
        return int(getattr(err, "group", -1)) == group_id and int(err.rc) == code
    except (TypeError, ValueError):
        return False


# --------------------------------------------------------------- identity ---


@case("os/echo", group="core")
async def echo(conn, g, is_error, fmt_error):
    """The liveness check, and how CDC 1 is told from CDC 0 -- CDC 0 accepts the
    open and never answers."""
    from smpclient.requests.os_management import EchoWrite

    resp = await conn.request(EchoWrite(d="healthypi"))
    if is_error(resp):
        return _err("os/echo", resp, fmt_error, "echo refused")
    got = getattr(resp, "r", None)
    if got != "healthypi":
        return fail("os/echo", f"echoed {got!r}, expected 'healthypi'")
    return ok("os/echo", "round-trip verified")


@case("device_info", group="core")
async def device_info(conn, g, is_error, fmt_error):
    resp = await conn.request(g.device_info())
    if is_error(resp):
        return _err("device_info", resp, fmt_error, "refused")
    sn, fw, br = resp.sn, resp.fw, resp.br
    if not sn:
        return fail("device_info", "serial number is empty")
    if not fw:
        return fail("device_info", "firmware version is empty")
    return ok("device_info", f"sn={sn} fw={fw} board={br} up={resp.up}s")


@case("fw_versions", group="core")
async def fw_versions(conn, g, is_error, fmt_error):
    resp = await conn.request(g.fw_versions())
    if is_error(resp):
        return _err("fw_versions", resp, fmt_error, "refused")
    if not resp.m7fw:
        return fail("fw_versions", "M7 version is empty")
    m4 = resp.m4fw or "-"
    # An empty M4 version is not a failure: the M4 binds ~7-10 s after reset and
    # a device polled before that legitimately has nothing to report yet.
    return ok("fw_versions", f"m7={resp.m7fw} m4={m4} esp={resp.espfw or '-'}")


@case("telemetry", group="core")
async def telemetry(conn, g, is_error, fmt_error):
    resp = await conn.request(g.telemetry())
    if is_error(resp):
        return _err("telemetry", resp, fmt_error, "refused")
    if not resp.ok:
        return fail("telemetry", "device reports the telemetry source unhealthy "
                                 "(fuel gauge not answering?)")
    if resp.vbat_mv == 0:
        # No cell fitted -- the ordinary state of a bench unit on USB, and an
        # unmet precondition rather than a defect. This used to FAIL, which put
        # a permanent red line in every bench run; a suite that always shows one
        # failure teaches people to skim past the failures, which is worse than
        # having no suite. Note `batt` cannot be used to detect this: the catalog
        # defines it as "literally !usb", i.e. running-on-battery, not
        # battery-present.
        return skip("telemetry",
                    "no battery fitted (vbat 0 mV); connect a cell to exercise "
                    "the fuel gauge, or run on battery")
    if not 2500 <= resp.vbat_mv <= 4500:
        # Non-zero but implausible is a real fault: the gauge answered with a
        # number that cannot be a Li-ion cell.
        return fail("telemetry", f"vbat {resp.vbat_mv} mV is outside a plausible "
                                 "Li-ion range")
    return ok("telemetry", f"vbat={resp.vbat_mv}mV soc={resp.soc}% usb={resp.usb}")


@case("datetime", group="core")
async def datetime_read(conn, g, is_error, fmt_error):
    """Stock `os` datetime, READ ONLY.

    The write half lives in its own destructive case. It used to run here, on
    every default `hpi test run`, and it set the clock to a hardcoded date in
    the past and never put it back -- so a suite advertised as "read-only by
    default" silently backdated the device, and any recording started just
    afterwards carried a wrong wall-clock timestamp.
    """
    try:
        from smpclient.requests.os_management import DateTimeRead
    except ImportError:
        return skip("datetime", "this smpclient build has no datetime request")

    resp = await conn.request(DateTimeRead())
    if is_error(resp):
        if _is_err(resp, catalog.OS_GROUP_ID, catalog.OS_ERR_RTC_NOT_SET):
            return skip(
                "datetime",
                "device clock has never been set (os/RTC_NOT_SET); set it with "
                "`healthypi device datetime --set now` and re-run",
            )
        return _err("datetime", resp, fmt_error, "read refused")
    return ok("datetime", f"clock reads {getattr(resp, 'datetime', None)}")


@case("datetime write", group="core", destructive=True)
async def datetime_rw(conn, g, is_error, fmt_error):
    """Stock `os` datetime. Read, write, read back, then RESTORE."""
    try:
        from smpclient.requests.os_management import DateTimeRead, DateTimeWrite
    except ImportError:
        return skip("datetime", "this smpclient build has no datetime request")

    resp = await conn.request(DateTimeRead())
    if is_error(resp):
        if _is_err(resp, catalog.OS_GROUP_ID, catalog.OS_ERR_RTC_NOT_SET):
            # Not a defect and not a firmware bug: the STM32 RTC reports no time
            # at all until the calendar has been written once, and Zephyr's os
            # handler turns that into os/RTC_NOT_SET. A device fresh off the
            # line, or one whose VBAT cell was disconnected, legitimately lands
            # here -- so it is an unmet precondition, which this suite reports
            # as SKIP rather than as a failure someone learns to ignore.
            return skip(
                "datetime",
                "device clock has never been set (os/RTC_NOT_SET); set it with "
                "`healthypi device datetime --set now` and re-run",
            )
        return _err("datetime", resp, fmt_error, "read refused")
    before = getattr(resp, "datetime", None)

    stamp = "2026-08-03T12:34:56"
    w = await conn.request(DateTimeWrite(datetime=stamp))
    if is_error(w):
        return _err("datetime", w, fmt_error, "write refused")

    back = await conn.request(DateTimeRead())
    if is_error(back):
        return _err("datetime", back, fmt_error, "read-back refused")
    got = getattr(back, "datetime", "") or ""
    if not got.startswith("2026-08-03T12:34:5"):
        return fail("datetime write", f"wrote {stamp}, read back {got!r}")

    # Put the clock back. Leaving a device backdated is a side effect nobody
    # asked for, and it silently mis-stamps the next recording.
    restored = "not restored (nothing to restore to)"
    if before:
        r = await conn.request(DateTimeWrite(datetime=str(before)[:19]))
        restored = "restored" if not is_error(r) else "RESTORE FAILED"
    return ok("datetime write", f"r/w verified (was {before}); {restored}")


# ----------------------------------------------------------------- stream ---


@case("stream_status", group="stream")
async def stream_status(conn, g, is_error, fmt_error):
    resp = await conn.request(g.stream_status())
    if is_error(resp):
        return _err("stream_status", resp, fmt_error, "refused")
    return ok("stream_status",
              f"active={resp.active} ch=0x{resp.ch:02X} sent={resp.sent} "
              f"dropped={resp.dropped}")


@case("stream start/stop", group="stream", requires=(REQ_UNLOCK,),
      destructive=True)
async def stream_cycle(conn, g, is_error, fmt_error):
    """Enable ECG+PPG, confirm the device reports it, then stop.

    Destructive because it leaves streaming off: a host that was mid-capture
    would lose it.
    """
    resp = await conn.request(g.stream_start(ch=0x03, ann=0))
    if is_error(resp):
        return _err("stream start/stop", resp, fmt_error, "start refused")

    st = await conn.request(g.stream_status())
    if is_error(st):
        return _err("stream start/stop", st, fmt_error, "status refused")
    if not st.active:
        return fail("stream start/stop",
                    "device accepted stream_start but reports active=false")
    if st.ch != 0x03:
        return fail("stream start/stop",
                    f"asked for ch=0x03, device reports 0x{st.ch:02X}")

    stop = await conn.request(g.stream_stop())
    if is_error(stop):
        return _err("stream start/stop", stop, fmt_error, "stop refused")

    st2 = await conn.request(g.stream_status())
    if not is_error(st2) and st2.active:
        return fail("stream start/stop", "still active after stream_stop")
    return ok("stream start/stop", "enabled, confirmed, disabled")


@case("stream_stop idempotent", group="stream")
async def stream_stop_idempotent(conn, g, is_error, fmt_error):
    """Stopping an already-stopped stream must succeed, not error. The updater
    calls this unconditionally before an upload."""
    resp = await conn.request(g.stream_stop())
    if is_error(resp):
        return _err("stream_stop idempotent", resp, fmt_error,
                    "refused on an already-stopped stream")
    return ok("stream_stop idempotent", "no-op accepted")


# --------------------------------------------------------------- recording ---


@case("sd_status", group="recording")
async def sd_status(conn, g, is_error, fmt_error):
    resp = await conn.request(g.sd_status())
    if is_error(resp):
        return _err("sd_status", resp, fmt_error, "refused")
    return ok("sd_status",
              f"active={resp.active} bytes={resp.bytes} path={resp.path or '-'}")


@case("record start/stop", group="recording",
      requires=(REQ_UNLOCK, REQ_SD), destructive=True)
async def record_cycle(conn, g, is_error, fmt_error):
    """Record briefly, confirm bytes actually land, stop.

    This is the one case that proves the recording path end to end rather than
    trusting a status flag: a device that reports active but writes nothing
    fails here.
    """
    start = await conn.request(g.sd_record_start(name="hpi-selftest"))
    if is_error(start):
        return _err("record start/stop", start, fmt_error, "start refused")

    await asyncio.sleep(2.0)
    mid = await conn.request(g.sd_status())
    stop = await conn.request(g.sd_record_stop())

    if is_error(mid):
        return _err("record start/stop", mid, fmt_error, "status refused")
    if is_error(stop):
        return _err("record start/stop", stop, fmt_error, "stop refused")
    if not mid.active:
        return fail("record start/stop", "started, but status says inactive")
    if mid.bytes == 0:
        return fail("record start/stop",
                    "recording is active but 0 bytes written after 2 s -- "
                    "the writer is not receiving bus frames")
    return ok("record start/stop", f"{mid.bytes} B to {mid.path}")


@case("transfer_mode", group="recording")
async def transfer_mode(conn, g, is_error, fmt_error):
    resp = await conn.request(g.transfer_mode())
    if is_error(resp):
        return _err("transfer_mode", resp, fmt_error, "refused")
    return ok("transfer_mode", f"armed={resp.armed}")


# ------------------------------------------------------------------ system ---


@case("module_list", group="system")
async def module_list(conn, g, is_error, fmt_error):
    resp = await conn.request(g.module_list())
    if is_error(resp):
        return _err("module_list", resp, fmt_error, "refused")
    return ok("module_list", f"slot A={resp.a} slot B={resp.b}")


@case("wifi_status", group="system")
async def wifi_status(conn, g, is_error, fmt_error):
    resp = await conn.request(g.wifi_status())
    if is_error(resp):
        return _err("wifi_status", resp, fmt_error, "refused")
    return ok("wifi_status",
              f"state={resp.state} ssid={resp.ssid or '-'} ip={resp.ip or '-'}")


@case("diag_selftest", group="system")
async def diag_selftest(conn, g, is_error, fmt_error):
    """Run the on-device self-test and report per-subsystem results.

    A failing subsystem is reported, not treated as a suite failure: a bench
    unit with no SD card and no battery legitimately fails two of them. What
    would be a real failure is the command not answering at all.
    """
    resp = await conn.request(g.diag_run_selftest())
    if is_error(resp):
        return _err("diag_selftest", resp, fmt_error, "refused")
    subs = {"sd": resp.sd, "batt": resp.batt, "ecg": resp.ecg,
            "ppg": resp.ppg, "m4": resp.m4, "qspi": resp.qspi}
    bad = [k for k, v in subs.items() if not v]
    # `pass` is a Python keyword, so the generated model carries it under that
    # exact name and it is only reachable via getattr -- resp.pass is a syntax
    # error and resp.pass_ does not exist.
    npass = getattr(resp, "pass", None)
    nfail = getattr(resp, "fail", None)
    detail = f"suite v{resp.suite_ver}: "
    if npass is not None and nfail is not None:
        detail += f"{npass} passed, {nfail} failed  "
    detail += " ".join(f"{k}={'ok' if v else 'FAIL'}" for k, v in subs.items())
    if bad:
        detail += f"  (down: {', '.join(bad)})"
    return ok("diag_selftest", detail)


@case("diag_lead_off", group="system")
async def diag_lead_off(conn, g, is_error, fmt_error):
    """ECG electrode state, and which sensor the reported heart rate came from.

    Electrodes off is NOT a failure -- a bench unit with nothing connected
    correctly reports all four off. What this checks is that the device answers
    with a *fresh* state (`ok`), because a stale mask reads as "leads on" and
    that is the reading that matters: an ECG heart rate is only credible while
    the electrodes are attached.
    """
    resp = await conn.request(g.diag_lead_off())
    if is_error(resp):
        return _err("diag_lead_off", resp, fmt_error, "refused")

    off = [n for n, v in (("RA", resp.ra), ("LA", resp.la),
                          ("LL", resp.ll), ("V1", resp.v1)) if v]
    if not resp.ok:
        return fail("diag_lead_off",
                    f"stale state: no ECG sample for {resp.age_ms} ms "
                    "(acquisition stopped?)")
    src = "PPG" if resp.hr_src else "ECG"
    detail = ("all electrodes on" if not off else f"off: {', '.join(off)}")
    detail += f"  hr={resp.hr or '-'} ({src})  age={resp.age_ms} ms"
    if off and resp.hr and not resp.hr_src:
        return fail("diag_lead_off",
                    "reports an ECG heart rate while electrodes are off")
    return ok("diag_lead_off", detail)


@case("lock_state", group="system")
async def lock_state(conn, g, is_error, fmt_error):
    resp = await conn.request(g.lock_state())
    if is_error(resp):
        return skip("lock_state", "no lock gate in this build "
                                  "(CONFIG_HPI_SECURITY=n)")
    return ok("lock_state", "unlocked" if resp.state == 1 else "locked")


# --------------------------------------------------------------------- fw ---


@case("m4fw_status", group="fw", requires=(REQ_SIGNED,))
async def m4fw_status(conn, g, is_error, fmt_error):
    resp = await conn.request(g.m4fw_status())
    if is_error(resp):
        return _err("m4fw_status", resp, fmt_error, "refused")
    return ok("m4fw_status",
              f"state={resp.st} rx={resp.rx}/{resp.len} "
              f"signature_required={resp.sig}")


@case("m4fw rejects a bad digest", group="fw",
      requires=(REQ_SIGNED, REQ_UNLOCK), destructive=True)
async def m4fw_bad_digest(conn, g, is_error, fmt_error):
    """A negative that matters: bank 2 must not be touched by a corrupt image.

    Begins a transfer with a digest that cannot match, uploads one chunk, and
    expects the commit to be REFUSED. Then aborts, leaving the device as it was.
    """
    payload = b"\xa5" * 512
    wrong = bytes(32)  # all-zero SHA-256: cannot match anything

    begin = await conn.request(g.m4fw_begin(len=len(payload), sha=wrong))
    if is_error(begin):
        # Refusing at begin is also correct behaviour.
        await conn.request(g.m4fw_abort())
        return ok("m4fw rejects a bad digest", "refused at begin")

    chunk = await conn.request(g.m4fw_chunk(off=0, data=payload))
    if is_error(chunk):
        await conn.request(g.m4fw_abort())
        return ok("m4fw rejects a bad digest", "refused at chunk")

    commit = await conn.request(g.m4fw_commit(), timeout_s=30.0)
    await conn.request(g.m4fw_abort())
    if not is_error(commit):
        return fail("m4fw rejects a bad digest",
                    "COMMIT ACCEPTED an image whose digest cannot match -- "
                    "bank 2 may have been written with corrupt firmware")
    return ok("m4fw rejects a bad digest", "commit refused, bank 2 untouched")


@case("recovery availability", group="fw")
async def recovery_available(conn, g, is_error, fmt_error):
    """Can this device reach MCUboot serial recovery? Reads only -- arming it
    would reboot the unit out from under the suite."""
    resp = await conn.request(g.enter_recovery())
    if is_error(resp):
        return skip("recovery availability",
                    "0x00A5 not present (unsigned build, no bootloader)")
    if not resp.av:
        return fail("recovery availability",
                    "firmware reports recovery UNAVAILABLE -- this unit can "
                    "only be rescued over SWD with the case open")
    return ok("recovery availability", f"available (armed={resp.armed})")


# ------------------------------------------------------------------- soak ---


async def soak(conn, iterations: int, on_progress=None) -> tuple[int, list[float]]:
    """Tight-loop `os echo`, returning (errors, latencies_ms).

    Kept out of the case registry: it takes a count, runs for minutes, and is
    driven explicitly by `hpi test soak`.
    """
    from smpclient.requests.os_management import EchoWrite

    errors = 0
    lat: list[float] = []
    for i in range(iterations):
        t0 = time.monotonic()
        try:
            resp = await conn.request(EchoWrite(d="soak"))
            if getattr(resp, "r", None) != "soak":
                errors += 1
        except Exception:  # noqa: BLE001
            errors += 1
        lat.append((time.monotonic() - t0) * 1000.0)
        if on_progress and (i % 500 == 0):
            on_progress(i, errors)
    return errors, lat
