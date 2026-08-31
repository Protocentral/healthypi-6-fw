# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT

"""``healthypi`` / ``hpi`` -- the command line.

Thin by construction: every verb is a few lines over a library call, mirroring
the firmware's own rule that a capability is implemented once and adapted
thinly. If a verb here contains logic, it belongs in a module instead.

The command surface is bounded by what the firmware actually implements -- see
``healthypi.smp.catalog``. Commands the device declares but never routes get no
verb, because a verb that can only return ENOTSUP is worse than no verb.
"""

from __future__ import annotations

import argparse
import json
import sys
from typing import Any

from .. import __version__

# ---------------------------------------------------------------- output ----


def _emit(args, payload: dict[str, Any], text: str | None = None) -> None:
    if getattr(args, "json", False):
        print(json.dumps(payload, indent=2, default=str))
    elif text is not None:
        print(text)
    else:
        width = max((len(k) for k in payload), default=0)
        for k, v in payload.items():
            print(f"{k:<{width}}  {v}")


def _fail(msg: str, code: int = 1) -> int:
    print(f"error: {msg}", file=sys.stderr)
    return code


def _model_dict(resp) -> dict:
    """CBOR payload of an SMP response, without the transport bookkeeping."""
    return resp.model_dump(exclude={"header", "version", "sequence", "smp_data"})


# ------------------------------------------------------------ device I/O ----


def _run_device(args, coro_factory) -> int:
    """Open CDC 1, run one coroutine, translate SMP errors into exit codes.

    ``coro_factory`` is called as ``factory(conn, g)`` -- the generated command
    set is passed in rather than imported by the caller, so that a missing
    device stack is reported as one line here instead of escaping as a traceback
    from whichever verb happened to import it first.
    """
    import asyncio

    try:
        from ..smp.group64 import fmt_error, g, is_error
        from ..transport import serial_smp
    except ImportError:
        return _fail(
            "this command needs the device stack, which is not installed.\n"
            "  Run: pip install 'healthypi[device]'",
            4,
        )

    async def _main() -> int:
        try:
            async with serial_smp.session(
                args.port, baud=args.baud, frame_size=args.frame_size
            ) as conn:
                resp = await coro_factory(conn, g)
                if resp is None:
                    return 0
                if is_error(resp):
                    return _fail(fmt_error(resp), 2)
                _emit(args, _model_dict(resp))
                return 0
        except serial_smp.NoDeviceError as exc:
            return _fail(str(exc), 3)
        except (TimeoutError, asyncio.TimeoutError):
            # Either the port is not a HealthyPi, or it is CDC 0 -- which
            # accepts the open and then never answers.
            where = args.port or "the detected port"
            return _fail(
                f"no response from {where}. Is it CDC 1 (the control port)? "
                "CDC 0 carries the sample stream and never answers.",
                3,
            )
        except OSError as exc:
            return _fail(f"could not open {args.port or 'the port'}: {exc}", 3)

    try:
        return asyncio.run(_main())
    except KeyboardInterrupt:
        return 130


def _simple(command: str, /, *arg_keys: str):
    """A verb that sends one group-64 command and prints the reply.

    ``arg_keys`` name the CBOR keys taken from parsed arguments; an argument
    left at ``None`` is omitted, which is how optional request fields work.
    (Positional-only, so a key called ``name`` cannot collide with a parameter.)
    """

    def handler(args) -> int:
        payload = {
            k: getattr(args, k) for k in arg_keys if getattr(args, k, None) is not None
        }
        return _run_device(args, lambda conn, g: conn.request(g[command](**payload)))

    return handler


_CONN_RADIO_BITS = {"wifi": 0x01, "ble": 0x02}


def _conn_enable(args) -> int:
    """Power the co-processor and start the named radios.

    ``--radios none`` is not the same as ``conn disable``: it powers the C6 and
    starts nothing, which is the state you need in order to flash it (the M7
    holds its EN line low by default, and a part in reset does not enumerate its
    USB-Serial/JTAG port). ``conn disable`` puts it back into reset.
    """
    text = (args.radios or "").strip().lower()
    mask = 0
    if text not in ("", "none"):
        for name in text.split(","):
            name = name.strip()
            if name not in _CONN_RADIO_BITS:
                print(
                    f"unknown radio {name!r}: expected wifi, ble, none, "
                    "or a comma-separated combination",
                    file=sys.stderr,
                )
                return 2
            mask |= _CONN_RADIO_BITS[name]
    return _run_device(args, lambda conn, g: conn.request(g["conn_enable"](radios=mask)))


def cmd_device_datetime(args) -> int:
    """Read, or set, the on-device RTC.

    Not a group-64 command: date and time live in the stock MCUmgr *os* group,
    which the firmware serves with a hook that recomputes tm_wday (the STM32 RTC
    stores the weekday separately and rejects an inconsistent one).

    Worth setting once per board. The RTC is LSE-backed and VBAT keeps it across
    resets, but the STM32 driver reports no time at all until the calendar has
    been written once -- so an unset device shows "--" for the clock and stamps
    every recording timestamp_start=0.
    """
    import asyncio
    from datetime import datetime, timezone

    try:
        from smpclient.requests.os_management import DateTimeRead, DateTimeWrite

        from ..transport import serial_smp
    except ImportError:
        return _fail(
            "this command needs the device stack, which is not installed.\n"
            "  Run: pip install 'healthypi[device]'",
            4,
        )

    if args.set is None:
        when = None
    elif args.set == "now":
        # Local time with an explicit offset: the device stores what it is told,
        # and a bare naive timestamp would silently mean UTC on the wire.
        when = datetime.now(timezone.utc).astimezone()
    else:
        try:
            when = datetime.fromisoformat(args.set)
        except ValueError:
            return _fail(
                f"could not parse {args.set!r}. Use 'now' or an ISO-8601 "
                "timestamp such as 2026-08-14T11:42:00",
                1,
            )

    async def _main() -> int:
        try:
            async with serial_smp.session(
                args.port, baud=args.baud, frame_size=args.frame_size
            ) as conn:
                if when is not None:
                    resp = await conn.request(
                        DateTimeWrite(datetime=when.isoformat(timespec="seconds"))
                    )
                    if _is_smp_error(resp):
                        return _fail(f"could not set the clock: {_why(resp)}", 2)
                resp = await conn.request(DateTimeRead())
                if _is_smp_error(resp):
                    return _fail(f"could not read the clock: {_why(resp)}", 2)
                dt = getattr(resp, "datetime", None)
                _emit(args, {"datetime": dt}, f"RTC: {dt}")
                return 0
        except serial_smp.NoDeviceError as exc:
            return _fail(str(exc), 3)
        except (TimeoutError, asyncio.TimeoutError):
            where = args.port or "the detected port"
            return _fail(
                f"no response from {where}. Is it CDC 1 (the control port)?", 3
            )
        except OSError as exc:
            return _fail(f"could not open {args.port or 'the port'}: {exc}", 3)

    try:
        return asyncio.run(_main())
    except KeyboardInterrupt:
        return 130


def _is_smp_error(resp) -> bool:
    """True for an SMP error reply.

    smpclient's own predicate, not a guess at which fields are present. The
    first version of this checked for a `datetime` attribute, which is right for
    a read and WRONG for a write: DateTimeWrite answers with an empty CBOR map
    on success, so every successful set was reported as a failure while the
    clock was in fact being set.
    """
    from smpclient.generics import error

    return error(resp)


def _why(resp) -> str:
    """A sentence, not a packet dump.

    smpclient renders an error reply as its full Header plus raw CBOR, which is
    unreadable in a terminal and buries the one thing that matters. RTC_NOT_SET
    is the expected answer on a device whose clock has never been written, and
    it has an obvious next step, so say it.
    """
    err = getattr(resp, "err", None)
    rc = getattr(err, "rc", None)
    name = getattr(rc, "name", None)

    if name == "RTC_NOT_SET":
        return (
            "the device clock has never been set.\n"
            "  Set it from this machine with: healthypi device datetime --set now"
        )
    if name:
        return f"{name} (rc={int(rc)})"
    return str(resp)


# ------------------------------------------------------------------ hp6 ----


def cmd_hp6_info(args) -> int:
    from .. import hp6

    header, blocks = hp6.read_file(args.file)
    stats = hp6.ReadStats()
    header, blocks = hp6.read_file(args.file, stats)
    for _ in blocks:
        pass

    out: dict[str, Any] = {"file": args.file}
    if header is None:
        out["container"] = "raw stream (no file header)"
    else:
        out.update(
            container="HP6 file",
            version=f"0x{header.version:04X}",
            session=header.session_name or "-",
            firmware=header.firmware_version or "-",
            serial=header.serial_number or "-",
            board=header.board_variant or "-",
            started_unix_ms=header.timestamp_start or "unset",
            duration_ms=header.duration_ms,
            closed_cleanly=not header.is_open,
            header_crc_ok=header.crc_ok,
        )
    out.update(
        blocks=stats.blocks,
        bytes=stats.bytes_consumed,
        samples=stats.samples,
        crc_errors=stats.crc_errors,
        sequence_gaps=stats.seq_gaps,
        lost_blocks=stats.lost_blocks,
    )
    _emit(args, out)
    return 0


def cmd_hp6_verify(args) -> int:
    from .. import hp6

    rep = hp6.verify(args.file)
    payload = {
        "file": rep.path,
        "ok": rep.ok,
        "blocks": rep.stats.blocks,
        "samples": rep.stats.samples,
        "sync_markers": rep.sync_markers,
        "problems": rep.problems,
    }
    if getattr(args, "json", False):
        _emit(args, payload)
    elif rep.ok:
        print(
            f"OK  {rep.path}\n"
            f"    {rep.stats.blocks} blocks, {rep.stats.samples}, "
            f"{rep.sync_markers} sync markers, all CRCs valid"
        )
    else:
        print(f"FAIL  {rep.path}")
        for p in rep.problems:
            print(f"    - {p}")
    return 0 if rep.ok else 1


def cmd_hp6_to_csv(args) -> int:
    from .. import hp6

    res = hp6.export_csv(args.file, args.outdir)
    if not res.files:
        return _fail("no decodable channels found")
    _emit(
        args,
        {"rows": res.rows, "files": res.files},
        "\n".join(f"{n:<8} {c:>10} rows  {p}" for n, (c, p) in
                  ((k, (res.rows[k], res.files[k])) for k in res.files)),
    )
    return 0


def cmd_hp6_repair(args) -> int:
    from .. import hp6

    res = hp6.repair(args.file, args.out)
    if res.was_clean:
        print(f"{res.src} was already clean; copied to {res.dst}")
        return 0
    _emit(
        args,
        {
            "src": res.src,
            "repaired": res.dst,
            "blocks_kept": res.blocks_kept,
            "dropped_bytes": res.dropped_bytes,
            "truncated_at": res.truncated_at,
            "last_sync_seq": res.last_sync_seq,
        },
        f"repaired {res.src}\n"
        f"    -> {res.dst}\n"
        f"    kept {res.blocks_kept} blocks, dropped {res.dropped_bytes} trailing bytes",
    )
    return 0


def cmd_hp6_events(args) -> int:
    from .. import hp6

    path = args.file
    if path.upper().endswith(".HP6"):
        path = hp6.idx_path_for(path)
    idx = hp6.read_idx(path)
    payload = {
        "file": path,
        "version": f"0x{idx.version:04X}",
        "sync_count_declared": idx.sync_count,
        "sync_records": len(idx.entries),
        "complete": idx.complete,
        "events": idx.event_count,
    }
    if getattr(args, "json", False):
        payload["entries"] = [
            {"ts_ms": e.ts_ms, "offset": e.file_off, "seq": e.seq, "crc": e.crc}
            for e in idx.entries
        ]
        _emit(args, payload)
        return 0
    _emit(args, payload)
    if not idx.complete:
        print("\nnote: the index is incomplete -- the recording was not closed")
        print("      cleanly. Scan the in-band sync markers instead (hp6 verify).")
    for e in idx.entries[:20]:
        print(f"  ts={e.ts_ms:>10} ms  offset={e.file_off:>12}  seq={e.seq}")
    if len(idx.entries) > 20:
        print(f"  … {len(idx.entries) - 20} more")
    return 0


# --------------------------------------------------------------- device ----


def cmd_stream_capture(args) -> int:
    """Drain CDC 0 into a .HP6 file. A raw capture is not a file until wrapped."""
    import time

    from .. import hp6

    try:
        import serial
    except ImportError:
        return _fail("pyserial not installed. Run: pip install 'healthypi[device]'")

    chunks: list[bytes] = []
    deadline = time.monotonic() + args.seconds
    try:
        with serial.Serial(args.stream_port, args.baud, timeout=0.2) as ser:
            while time.monotonic() < deadline:
                data = ser.read(4096)
                if data:
                    chunks.append(data)
    except KeyboardInterrupt:
        pass
    except serial.SerialException as exc:
        return _fail(str(exc))

    raw = b"".join(chunks)
    if not raw:
        return _fail(
            f"no data on {args.stream_port}. Is streaming enabled "
            "(hpi stream start) and is this CDC 0?"
        )
    hp6.wrap_capture(raw, args.out, hp6.new_header(session_name=args.name or "capture"))
    rep = hp6.verify(args.out)
    _emit(
        args,
        {
            "file": args.out,
            "bytes": len(raw),
            "blocks": rep.stats.blocks,
            "samples": rep.stats.samples,
            "dropped_blocks": rep.stats.lost_blocks,
        },
        f"captured {len(raw)} B -> {args.out}\n"
        f"    {rep.stats.blocks} blocks, {rep.stats.samples}, "
        f"{rep.stats.lost_blocks} lost to gaps",
    )
    return 0


def cmd_wifi_stream_monitor(args) -> int:
    from ..openview import monitor

    def show(pkt):
        print(
            f"seq={pkt.seq:<8} ecg={pkt.ecg1:>9} resp={pkt.respiration:>7} "
            f"red={pkt.ppg_red:>8} ir={pkt.ppg_ir:>8} "
            f"hr={pkt.heart_rate:>3} spo2={pkt.spo2:>3}"
        )

    cb = show if args.verbose else None
    try:
        if args.udp:
            stats = monitor.monitor_udp(
                port=args.port or monitor.UDP_PORT, duration=args.seconds, on_packet=cb
            )
        else:
            stats = monitor.monitor_tcp(
                host=args.host,
                port=args.port or monitor.TCP_PORT,
                duration=args.seconds,
                on_packet=cb,
            )
    except OSError as exc:
        return _fail(f"could not reach the stream: {exc}", 3)

    s = stats.summary()
    _emit(
        args,
        s,
        f"{s['packets']} packets in {s['seconds']}s  ({s['rate_hz']} Hz)\n"
        f"  expected {s['expected']}, lost {s['lost']} in {s['gaps']} gap(s) "
        f"= {s['loss_pct']}%\n"
        f"  {s['bad_packets']} undecodable, {s['resync_bytes']} bytes resynced",
    )
    return 0 if stats.packets else 1


# ------------------------------------------------------------------- fw ----


def _bundle_or_fail(args):
    """Open and digest-check a bundle, reporting failure as one line."""
    from ..fw import Bundle, BundleError

    try:
        return Bundle(args.bundle), None
    except BundleError as exc:
        return None, _fail(str(exc))


def cmd_fw_info(args) -> int:
    from ..fw import BundleError

    bundle, err = _bundle_or_fail(args)
    if bundle is None:
        return err
    try:
        bundle.verify(args.pubkey)
    except BundleError as exc:
        return _fail(str(exc))
    if getattr(args, "json", False):
        _emit(args, bundle.manifest)
        return 0
    print(bundle.describe())
    if args.pubkey:
        print(f"  manifest signature verified against {args.pubkey}")
    else:
        print("  digests OK (pass --pubkey to check the manifest signature too)")
    return 0


def cmd_fw_bundle_create(args) -> int:
    from pathlib import Path

    from ..fw import BundleError, ImageSpec, create

    specs = [
        # The M7 image already carries its own MCUboot signature; the manifest
        # entry is for the host's benefit (what version, what digest).
        ImageSpec("m7", Path(args.m7), args.m7_version, "mcumgr-img"),
        # The M4 image has no container of its own, so its device-verifiable
        # signature travels in the manifest and is handed over in the group-64
        # begin command.
        ImageSpec("m4", Path(args.m4), args.m4_version, "hpi-g64", sign=True),
    ]
    if args.esp32c6:
        specs.append(
            ImageSpec("esp32c6", Path(args.esp32c6), args.esp32c6_version,
                      "esp-ota-http")
        )
    try:
        out = create(
            Path(args.out),
            specs,
            release=args.release,
            hw_rev=args.hw_rev,
            key_path=Path(args.key),
            created=args.created,
        )
    except BundleError as exc:
        return _fail(str(exc))
    _emit(
        args,
        {"bundle": str(out), "bytes": out.stat().st_size},
        f"  wrote {out} ({out.stat().st_size} B)",
    )
    return 0


def _fw_device(args, coro_factory) -> int:
    """Run one firmware operation, translating its errors into exit codes.

    Not ``_run_device``: the update flow owns its own connection because a
    mid-flow reset replaces it, and it prints progress as it goes rather than
    returning a single response to render.
    """
    import asyncio

    try:
        from ..fw.update import Target, UpdateError
    except ImportError:
        return _fail(
            "this command needs the device stack, which is not installed.\n"
            "  Run: pip install 'healthypi[device]'",
            4,
        )
    from ..transport import serial_smp

    target = Target(port=args.port, baud=args.baud, frame_size=args.frame_size)
    try:
        return asyncio.run(coro_factory(target))
    except UpdateError as exc:
        return _fail(str(exc), 2)
    except serial_smp.NoDeviceError as exc:
        return _fail(str(exc), 3)
    except KeyboardInterrupt:
        return _fail(
            "interrupted — no flash region is left half-written: the device "
            "only commits after a complete, verified image.",
            130,
        )


def cmd_fw_update(args) -> int:
    from ..fw import BundleError

    bundle, err = _bundle_or_fail(args)
    if bundle is None:
        return err

    async def run(target) -> int:
        from ..fw.update import apply_bundle

        try:
            res = await apply_bundle(
                bundle,
                target,
                pubkey=args.pubkey,
                only=args.only,
                force=args.force,
                dry_run=args.dry_run,
            )
        except BundleError as exc:
            return _fail(str(exc))
        return 0 if res.ok else 1

    return _fw_device(args, run)


def cmd_fw_enter_recovery(args) -> int:
    async def run(target) -> int:
        from ..fw.recovery import enter_recovery

        await enter_recovery(target)
        return 0

    return _fw_device(args, run)


def cmd_fw_recover(args) -> int:
    from ..fw import BundleError

    bundle, err = _bundle_or_fail(args)
    if bundle is None:
        return err

    async def run(target) -> int:
        from ..fw.recovery import recover

        try:
            await recover(bundle, target, pubkey=args.pubkey)
        except BundleError as exc:
            return _fail(str(exc))
        return 0

    return _fw_device(args, run)


# ------------------------------------------------------------------- hl ----


def cmd_hl_eeprom_generate(args) -> int:
    from pathlib import Path

    from ..hw import eeprom

    try:
        image = eeprom.create_image(
            module_id=eeprom.resolve_module_id(args.module_id),
            name=args.name,
            manufacturer=args.manufacturer,
            serial=args.serial,
            hw_rev_major=args.hw_major,
            hw_rev_minor=args.hw_minor,
            fw_compat_min=args.fw_compat,
            capabilities=eeprom.resolve_capabilities(args.capabilities),
            stack_position=args.stack_position,
        )
    except (eeprom.EepromError, ValueError) as exc:
        return _fail(str(exc))

    Path(args.output).write_bytes(image)
    info = eeprom.parse_image(image)
    if getattr(args, "json", False):
        _emit(args, {"file": args.output, **info.to_dict()})
        return 0
    print(f"wrote {args.output} ({len(image)} B)\n")
    print(info.describe())
    return 0


def cmd_hl_eeprom_read(args) -> int:
    from pathlib import Path

    from ..hw import eeprom

    try:
        info = eeprom.parse_image(Path(args.input).read_bytes())
    except (eeprom.EepromError, OSError) as exc:
        return _fail(str(exc))
    if getattr(args, "json", False):
        _emit(args, info.to_dict())
    else:
        print(info.describe())
    return 0 if info.crc_valid else 1


def cmd_hl_eeprom_stack(args) -> int:
    from pathlib import Path

    from ..hw import eeprom

    try:
        entries = eeprom.build_stack(args.modules, base_serial=args.base_serial)
    except (eeprom.EepromError, ValueError) as exc:
        return _fail(str(exc))

    outdir = Path(args.output_dir)
    outdir.mkdir(parents=True, exist_ok=True)
    written = []
    for e in entries:
        (outdir / e.filename).write_bytes(e.image)
        written.append(
            {
                "index": e.index,
                "file": str(outdir / e.filename),
                "module_id": f"0x{e.module_id:04X}",
                "name": e.name,
                "serial": e.serial,
                "i2c_address": f"0x{0x50 + e.index:02X}",
            }
        )
    if getattr(args, "json", False):
        _emit(args, {"modules": written})
        return 0
    for w in written:
        print(f"  [{w['index']}] {w['file']}: {w['name']} "
              f"(id={w['module_id']}, I2C {w['i2c_address']})")
    print(f"\n{len(written)} image(s) in {outdir}/")
    print("Strap each module's address pins to match its position:")
    for w in written:
        print(f"  position {w['index']} -> I2C {w['i2c_address']}")
    return 0


# ----------------------------------------------------------------- test ----


def cmd_test_list(args) -> int:
    from .. import testing

    rows = [
        {
            "name": c.name,
            "group": c.group,
            "requires": ",".join(c.requires) or "-",
            "destructive": c.destructive,
        }
        for c in testing.registry()
    ]
    if getattr(args, "json", False):
        _emit(args, {"cases": rows})
        return 0
    w = max(len(r["name"]) for r in rows)
    for r in rows:
        flag = " (destructive)" if r["destructive"] else ""
        print(f"  {r['name']:<{w}}  {r['group']:<10} needs={r['requires']}{flag}")
    print(f"\n{len(rows)} cases in {len(testing.groups())} groups: "
          f"{', '.join(testing.groups())}")
    return 0


def cmd_test_run(args) -> int:
    import asyncio

    try:
        from .. import testing
        from ..transport import serial_smp
    except ImportError:
        return _fail(
            "this command needs the device stack, which is not installed.\n"
            "  Run: pip install 'protocentral-healthypi[device]'",
            4,
        )

    def show(res) -> None:
        if not getattr(args, "json", False):
            mark = {"PASS": "PASS", "FAIL": "FAIL", "SKIP": "skip"}[res.outcome.value]
            print(f"  [{mark}] {res.name:<28} {res.detail}")

    async def _main() -> int:
        async with serial_smp.session(
            args.port, baud=args.baud, frame_size=args.frame_size
        ) as conn:
            if not getattr(args, "json", False):
                print(f"HealthyPi 6 group-64 suite on {conn.port}\n")
            report = await testing.run(
                conn,
                group=args.group,
                include_destructive=args.destructive,
                on_result=show,
            )
            if getattr(args, "json", False):
                _emit(args, report.to_dict())
            else:
                print(f"\n{report.summary()}")
                if not report.ok:
                    print("\nfailures:")
                    for r in report.results:
                        if r.outcome is testing.Outcome.FAIL:
                            print(f"  {r.name}: {r.detail}")
            return 0 if report.ok else 1

    try:
        return asyncio.run(_main())
    except serial_smp.NoDeviceError as exc:
        return _fail(str(exc), 3)
    except KeyboardInterrupt:
        return 130


def cmd_test_soak(args) -> int:
    """Tight-loop echo with latency stats -- the stability gate."""
    import asyncio
    import statistics

    try:
        from ..testing.cases import soak
        from ..transport import serial_smp
    except ImportError:
        return _fail("this command needs the device stack", 4)

    def progress(i, errors):
        if not getattr(args, "json", False):
            print(f"\r  {i}/{args.iterations}  errors={errors}", end="", flush=True)

    async def _main() -> int:
        async with serial_smp.session(
            args.port, baud=args.baud, frame_size=args.frame_size
        ) as conn:
            errors, lat = await soak(conn, args.iterations, on_progress=progress)
            if not getattr(args, "json", False):
                print()
            lat.sort()
            p99 = lat[int(len(lat) * 0.99)] if lat else 0.0
            payload = {
                "iterations": args.iterations,
                "errors": errors,
                "mean_ms": round(statistics.mean(lat), 2) if lat else 0,
                "p99_ms": round(p99, 2),
                "max_ms": round(max(lat), 2) if lat else 0,
            }
            # The historical gate: zero errors and p99 under 50 ms.
            good = errors == 0 and p99 < 50.0
            _emit(
                args,
                {**payload, "pass": good},
                f"  {args.iterations} iterations, {errors} errors\n"
                f"  mean {payload['mean_ms']} ms, p99 {payload['p99_ms']} ms, "
                f"max {payload['max_ms']} ms\n"
                f"  {'PASS' if good else 'FAIL'} (gate: 0 errors, p99 < 50 ms)",
            )
            return 0 if good else 1

    try:
        return asyncio.run(_main())
    except serial_smp.NoDeviceError as exc:
        return _fail(str(exc), 3)
    except KeyboardInterrupt:
        return 130


def cmd_catalog(args) -> int:
    """Print the group-64 surface this build knows about."""
    from ..smp import catalog

    if getattr(args, "json", False):
        _emit(
            args,
            {
                "group_id": catalog.GROUP_ID,
                "schema_version": catalog.SCHEMA_VERSION,
                "commands": [
                    {
                        "id": f"0x{c.cmd_id:04X}",
                        "name": c.name,
                        "ops": [o.value for o in c.ops],
                        "status": c.status.value,
                        "unlock": bool(c.meta.get("unlock")),
                        "signed_build": bool(c.meta.get("signed_build")),
                    }
                    for c in catalog.COMMANDS
                ],
                "errors": {k: v[0] for k, v in catalog.ERRORS.items()},
            },
        )
        return 0
    print(catalog.HPI_GROUP.describe())
    print(
        f"\n{len(catalog.live())} live, "
        f"{len(catalog.dispatchable()) - len(catalog.live())} stub, "
        f"{len(catalog.HPI_GROUP.unreachable())} reserved (not API)"
    )
    return 0


# ---------------------------------------------------------------- parser ----


def _add_device_opts(p: argparse.ArgumentParser) -> None:
    p.add_argument("--port", help="CDC 1 serial device (autodetected if omitted)")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--frame-size", type=int, default=256,
                   help="max encoded SMP frame; must not exceed the device MTU")
    p.add_argument("--json", action="store_true", help="machine-readable output")


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(
        prog="healthypi",
        description="HealthyPi 6 host tool: data, control and firmware update.",
    )
    ap.add_argument("--version", action="version", version=f"healthypi {__version__}")
    sub = ap.add_subparsers(dest="group", metavar="<group>")

    # -- hp6 (offline) ------------------------------------------------------
    hp6p = sub.add_parser("hp6", help="work with .HP6 recordings and captures")
    hp6s = hp6p.add_subparsers(dest="verb", metavar="<verb>")

    p = hp6s.add_parser("info", help="header and per-channel statistics")
    p.add_argument("file")
    p.add_argument("--json", action="store_true")
    p.set_defaults(func=cmd_hp6_info)

    p = hp6s.add_parser("verify", help="check every CRC, gap and counter")
    p.add_argument("file")
    p.add_argument("--json", action="store_true")
    p.set_defaults(func=cmd_hp6_verify)

    p = hp6s.add_parser("to-csv", help="export per-channel CSVs (streaming)")
    p.add_argument("file")
    p.add_argument("outdir")
    p.add_argument("--json", action="store_true")
    p.set_defaults(func=cmd_hp6_to_csv)

    p = hp6s.add_parser("repair", help="recover a recording that ended badly")
    p.add_argument("file")
    p.add_argument("--out", help="default: <file>.repaired")
    p.add_argument("--json", action="store_true")
    p.set_defaults(func=cmd_hp6_repair)

    p = hp6s.add_parser("events", help="read the .IDX sidecar")
    p.add_argument("file", help=".IDX, or the .HP6 next to it")
    p.add_argument("--json", action="store_true")
    p.set_defaults(func=cmd_hp6_events)

    # -- device -------------------------------------------------------------
    dev = sub.add_parser("device", help="identity and telemetry")
    devs = dev.add_subparsers(dest="verb", metavar="<verb>")
    for verb, cmd, helptext in (
        ("info", "device_info", "serial, firmware and uptime"),
        ("telemetry", "telemetry", "battery and power state"),
        ("versions", "fw_versions", "firmware version of every processor"),
    ):
        p = devs.add_parser(verb, help=helptext)
        _add_device_opts(p)
        p.set_defaults(func=_simple(cmd))

    p = devs.add_parser("datetime", help="read or set the on-device clock")
    _add_device_opts(p)
    p.add_argument(
        "--set",
        metavar="WHEN",
        help="'now' for this host's clock, or an ISO-8601 timestamp. "
             "Omit to read. The RTC keeps time across resets on VBAT, but "
             "reports nothing until it has been set once -- which is why an "
             "unset device shows '--' and records timestamp_start=0.",
    )
    p.set_defaults(func=cmd_device_datetime)

    # -- stream -------------------------------------------------------------
    st = sub.add_parser("stream", help="live sample streaming on CDC 0")
    sts = st.add_subparsers(dest="verb", metavar="<verb>")

    p = sts.add_parser("start", help="enable streaming")
    _add_device_opts(p)
    p.add_argument("--ch", type=lambda x: int(x, 0), default=0x03,
                   help="channel mask: 0x01 ECG, 0x02 PPG, 0x08 EEG (default 0x03)")
    p.add_argument("--ann", type=lambda x: int(x, 0), default=0,
                   help="annotation mask (reserved in firmware 1.0.0)")
    p.set_defaults(func=_simple("stream_start", "ch", "ann"))

    p = sts.add_parser("stop", help="disable streaming (idempotent)")
    _add_device_opts(p)
    p.set_defaults(func=_simple("stream_stop"))

    p = sts.add_parser("status", help="active channels, frames sent and dropped")
    _add_device_opts(p)
    p.set_defaults(func=_simple("stream_status"))

    p = sts.add_parser("capture", help="record CDC 0 to a .HP6 file")
    p.add_argument("stream_port", help="the CDC 0 device (the stream port)")
    p.add_argument("out", help="output .HP6")
    p.add_argument("--seconds", type=float, default=10.0)
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--name", help="session name for the file header")
    p.add_argument("--json", action="store_true")
    p.set_defaults(func=cmd_stream_capture)

    # -- record -------------------------------------------------------------
    rec = sub.add_parser("record", help="record to the SD card")
    recs = rec.add_subparsers(dest="verb", metavar="<verb>")

    p = recs.add_parser("start", help="begin a recording")
    _add_device_opts(p)
    p.add_argument("--name", help="session name")
    p.set_defaults(func=_simple("sd_record_start", "name"))

    p = recs.add_parser("stop", help="end the recording")
    _add_device_opts(p)
    p.set_defaults(func=_simple("sd_record_stop"))

    p = recs.add_parser("status", help="recording state and sample counts")
    _add_device_opts(p)
    p.set_defaults(func=_simple("sd_status"))

    rec.epilog = (
        "Recordings are retrieved over USB mass storage: `hpi transfer arm`, "
        "copy from the mounted disk, then `hpi transfer disarm`. Firmware 1.0.0 "
        "has no file-download command."
    )

    # -- transfer -----------------------------------------------------------
    tr = sub.add_parser("transfer", help="expose the SD card as a USB disk")
    trs = tr.add_subparsers(dest="verb", metavar="<verb>")
    p = trs.add_parser("status", help="is Transfer Mode armed?")
    _add_device_opts(p)
    p.set_defaults(func=_simple("transfer_mode"))
    for verb, on in (("arm", True), ("disarm", False)):
        p = trs.add_parser(verb, help=f"{'expose' if on else 'hide'} the SD card")
        _add_device_opts(p)
        p.set_defaults(func=_transfer(on))
    tr.epilog = (
        "Arming re-enumerates USB: the serial ports disappear and a disk appears, "
        "so the control connection drops. That is expected."
    )

    # -- wifi ---------------------------------------------------------------
    wf = sub.add_parser("wifi", help="Wi-Fi status and provisioning")
    wfs = wf.add_subparsers(dest="verb", metavar="<verb>")
    p = wfs.add_parser("status", help="link state, RSSI, SSID and IP")
    _add_device_opts(p)
    p.set_defaults(func=_simple("wifi_status"))
    p = wfs.add_parser("set", help="store credentials (requires unlock)")
    _add_device_opts(p)
    p.add_argument("ssid")
    p.add_argument("pw")
    p.set_defaults(func=_simple("wifi_set", "ssid", "pw"))
    p = wfs.add_parser("forget", help="clear stored credentials")
    _add_device_opts(p)
    p.set_defaults(func=_simple("wifi_forget"))
    p = wfs.add_parser("softap", help="open the provisioning portal")
    _add_device_opts(p)
    p.set_defaults(func=_simple("wifi_softap"))

    # -- conn ---------------------------------------------------------------
    # The device boots with the ESP32-C6 held in reset and both radios down, so
    # these are how anything gets turned on from a host.
    cn = sub.add_parser("conn", help="co-processor power and radio control")
    cns = cn.add_subparsers(dest="verb", metavar="<verb>")
    p = cns.add_parser("status", help="link state, radios, Wi-Fi and BLE")
    _add_device_opts(p)
    p.set_defaults(func=_simple("conn_status"))
    p = cns.add_parser("enable", help="power the co-processor and start radios")
    _add_device_opts(p)
    p.add_argument(
        "--radios",
        default="wifi",
        help="comma-separated: wifi, ble, or none. 'none' powers the "
        "co-processor without starting a radio -- which is what you need "
        "before flashing it, since it cannot enumerate its USB port while "
        "the M7 holds it in reset. Default: wifi",
    )
    p.set_defaults(func=_conn_enable)
    p = cns.add_parser("disable", help="radios down, co-processor into reset")
    _add_device_opts(p)
    p.set_defaults(func=_simple("conn_disable"))

    # -- module / diag / lock ----------------------------------------------
    md = sub.add_parser("module", help="HealthyLink expansion modules")
    mds = md.add_subparsers(dest="verb", metavar="<verb>")
    p = mds.add_parser("list", help="what is in each slot")
    _add_device_opts(p)
    p.set_defaults(func=_simple("module_list"))
    p = mds.add_parser("power", help="power a slot on or off")
    _add_device_opts(p)
    p.add_argument("slot", type=int)
    p.add_argument("state", choices=("on", "off"))
    p.set_defaults(func=_module_power)

    dg = sub.add_parser("diag", help="diagnostics")
    dgs = dg.add_subparsers(dest="verb", metavar="<verb>")
    p = dgs.add_parser("selftest", help="run the built-in self test")
    _add_device_opts(p)
    p.set_defaults(func=_simple("diag_run_selftest"))
    p = dgs.add_parser(
        "lead-off",
        help="ECG electrode state + which sensor the heart rate came from",
    )
    _add_device_opts(p)
    p.set_defaults(func=_simple("diag_lead_off"))

    lk = sub.add_parser("lock", help="the access-control gate")
    lks = lk.add_subparsers(dest="verb", metavar="<verb>")
    p = lks.add_parser("status", help="locked or unlocked")
    _add_device_opts(p)
    p.set_defaults(func=_simple("lock_state"))
    p = lks.add_parser("lock", help="re-lock the device")
    _add_device_opts(p)
    p.set_defaults(func=_simple("lock"))

    # -- wifi-stream --------------------------------------------------------
    ws = sub.add_parser("wifi-stream", help="watch the Wi-Fi (OpenView) stream")
    wss = ws.add_subparsers(dest="verb", metavar="<verb>")
    p = wss.add_parser("monitor", help="rate, sequence gaps and packet loss")
    p.add_argument("--host", default="192.168.4.1", help="TCP host (SoftAP default)")
    p.add_argument("--port", type=int, help="default: 5000 TCP, 5001 UDP")
    p.add_argument("--udp", action="store_true", help="listen for UDP broadcast")
    p.add_argument("--seconds", type=float, default=30.0)
    p.add_argument("--verbose", action="store_true", help="print every packet")
    p.add_argument("--json", action="store_true")
    p.set_defaults(func=cmd_wifi_stream_monitor)

    # -- fw -----------------------------------------------------------------
    fw = sub.add_parser("fw", help="firmware bundles, update and recovery")
    fws = fw.add_subparsers(dest="verb", metavar="<verb>")

    p = fws.add_parser("info", help="describe a bundle (no device needed)")
    p.add_argument("--bundle", required=True)
    p.add_argument("--pubkey", help="PEM key to verify the manifest signature")
    p.add_argument("--json", action="store_true")
    p.set_defaults(func=cmd_fw_info)

    p = fws.add_parser("update", help="bring a device up to a bundle")
    _add_device_opts(p)
    p.add_argument("--bundle", required=True)
    p.add_argument("--pubkey", help="PEM key to verify the manifest signature")
    p.add_argument("--only", choices=("esp32c6", "m4", "m7"),
                   help="restrict to one processor")
    p.add_argument("--force", action="store_true",
                   help="reapply even when versions already match")
    p.add_argument("--dry-run", action="store_true",
                   help="show the plan, write nothing")
    p.set_defaults(func=cmd_fw_update)

    p = fws.add_parser("enter-recovery",
                       help="reboot into MCUboot serial recovery")
    _add_device_opts(p)
    p.set_defaults(func=cmd_fw_enter_recovery)

    p = fws.add_parser("recover",
                       help="write the M7 to a device already in recovery")
    _add_device_opts(p)
    p.add_argument("--bundle", required=True)
    p.add_argument("--pubkey")
    p.set_defaults(func=cmd_fw_recover)

    bnd = fws.add_parser("bundle", help="build a release bundle")
    bnds = bnd.add_subparsers(dest="bundle_verb", metavar="<verb>")
    p = bnds.add_parser("create", help="pack signed images into a .hpifw")
    p.add_argument("out", help="output .hpifw path")
    p.add_argument("--m7", required=True, help="MCUboot-signed M7 image")
    p.add_argument("--m7-version", required=True)
    p.add_argument("--m4", required=True, help="raw M4 image")
    p.add_argument("--m4-version", required=True)
    p.add_argument("--esp32c6", help="optional C6 image")
    p.add_argument("--esp32c6-version", default="unknown")
    p.add_argument("--release", required=True, help="release version string")
    p.add_argument("--hw-rev", nargs="+", default=["v5"])
    p.add_argument("--key", required=True, help="ECDSA P-256 signing key (PEM)")
    p.add_argument("--created", required=True,
                   help="ISO timestamp; passed in so the build is reproducible")
    p.add_argument("--json", action="store_true")
    p.set_defaults(func=cmd_fw_bundle_create)

    fw.epilog = (
        "Update order is esp32c6 → m4 → m7; the M7 goes last because it applies "
        "the others. The C6 is skipped over USB — it updates itself over Wi-Fi. "
        "Use the CDC 1 (control) port."
    )

    # -- hl (HealthyLink module artifacts) ----------------------------------
    hl = sub.add_parser("hl", help="HealthyLink module artifacts")
    hls = hl.add_subparsers(dest="verb", metavar="<verb>")
    ee = hls.add_parser("eeprom", help="module identification EEPROM images")
    ees = ee.add_subparsers(dest="eeprom_verb", metavar="<verb>")

    p = ees.add_parser("generate", help="build a 256-byte EEPROM image")
    p.add_argument("--module-id", "-m", required=True,
                   help="name (EEG-8CH), 0x0001, or a decimal id")
    p.add_argument("--name", "-n", required=True, help="module name (max 31 chars)")
    p.add_argument("--manufacturer", default="ProtoCentral")
    p.add_argument("--serial", "-s", type=int, default=1)
    p.add_argument("--hw-major", type=int, default=1)
    p.add_argument("--hw-minor", type=int, default=0)
    p.add_argument("--fw-compat", type=lambda x: int(x, 0), default=0x0110)
    p.add_argument("--caps", "--capabilities", dest="capabilities", nargs="*",
                   help="capability flags by name, 0x… or decimal")
    p.add_argument("--stack-position", type=int, default=0,
                   help="0 = base, 1-3 = stacked")
    p.add_argument("--output", "-o", default="eeprom.bin")
    p.add_argument("--json", action="store_true")
    p.set_defaults(func=cmd_hl_eeprom_generate)

    p = ees.add_parser("read", help="decode an image; non-zero on a bad CRC")
    p.add_argument("input")
    p.add_argument("--json", action="store_true")
    p.set_defaults(func=cmd_hl_eeprom_read)

    p = ees.add_parser("stack", help="build a set for stacked modules")
    p.add_argument("--modules", nargs="+", required=True,
                   help="ID[:Name[:Manufacturer[:Serial]]], in stack order")
    p.add_argument("--base-serial", type=int, default=10000)
    p.add_argument("--output-dir", "-o", default="./stack_eeproms")
    p.add_argument("--json", action="store_true")
    p.set_defaults(func=cmd_hl_eeprom_stack)

    ee.epilog = (
        "Writing an image to a real 24AA02 needs an external programmer "
        "(FT232H, Raspberry Pi I2C, CH341A, Bus Pirate) and its own tool — "
        "this builds the file."
    )

    # -- test ---------------------------------------------------------------
    ts = sub.add_parser("test", help="the group-64 acceptance suite")
    tss = ts.add_subparsers(dest="verb", metavar="<verb>")

    p = tss.add_parser("list", help="the cases, and what each one needs")
    p.add_argument("--json", action="store_true")
    p.set_defaults(func=cmd_test_list)

    p = tss.add_parser("run", help="run the suite against a device")
    _add_device_opts(p)
    p.add_argument("--group", help="restrict to one group (see `test list`)")
    p.add_argument("--destructive", action="store_true",
                   help="include cases that change device state")
    p.set_defaults(func=cmd_test_run)

    p = tss.add_parser("soak", help="tight-loop echo with latency stats")
    _add_device_opts(p)
    p.add_argument("--iterations", type=int, default=60000,
                   help="default 60000 (~5 min at ~200/s)")
    p.set_defaults(func=cmd_test_soak)

    ts.epilog = (
        "Read-only by default: cases that change device state are skipped "
        "unless --destructive is given. Requirements (unlock, signed build, SD "
        "card) are probed from the device, and an unmet one is reported as SKIP "
        "with the reason — never as a pass. Exit code is non-zero if any case "
        "fails."
    )

    # -- catalog ------------------------------------------------------------
    p = sub.add_parser("catalog", help="the group-64 command surface")
    p.add_argument("--json", action="store_true")
    p.set_defaults(func=cmd_catalog)

    return ap


def _transfer(on: bool):
    def handler(args) -> int:
        return _run_device(
            args, lambda conn, g: conn.request(g.transfer_mode_write(on=on))
        )

    return handler


def _module_power(args) -> int:
    return _run_device(
        args,
        lambda conn, g: conn.request(
            g.module_power(slot=args.slot, on=args.state == "on")
        ),
    )


def main(argv: list[str] | None = None) -> int:
    ap = build_parser()
    args = ap.parse_args(argv)
    if not getattr(args, "func", None):
        # `hpi`, or `hpi hp6` with no verb: show the relevant help.
        if getattr(args, "group", None):
            for action in ap._subparsers._group_actions[0].choices.items():  # type: ignore[union-attr]
                if action[0] == args.group:
                    action[1].print_help()
                    return 1
        ap.print_help()
        return 1
    return args.func(args)


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
