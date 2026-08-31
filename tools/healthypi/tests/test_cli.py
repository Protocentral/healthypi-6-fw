# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT

"""CLI tests -- parser wiring and the offline verbs.

Device verbs are covered here only for argument handling and failure modes; the
request bytes they send are already covered by test_group64.py, and actually
talking to a device is the hardware gate.
"""

from __future__ import annotations

import json
import struct

import pytest

from healthypi import hp6
from healthypi.cli.main import build_parser, main


def _ecg(n=16):
    return [hp6.EcgSample(i, 1000 + i, -2000 - i, 3000 + i, 0, 0) for i in range(n)]


@pytest.fixture
def recording(tmp_path):
    path = tmp_path / "REC0001.HP6"
    hdr = hp6.new_header(
        session_name="cli-test",
        firmware_version="1.0.0",
        timestamp_start=1_700_000_000_000,
    )
    with hp6.FileWriter(path, hdr) as w:
        seq = 0
        for b in range(3):
            w.add(hp6.Channel.ECG, seq, b * 32, _ecg())
            seq += 1
            w.add(hp6.Channel.VITALS, seq, b * 32, [hp6.VitalsSample(72, 975, 0, 0, 0, 0)])
            seq += 1
    return path


# --- parser ----------------------------------------------------------------


def test_every_group_is_reachable():
    ap = build_parser()
    groups = ap._subparsers._group_actions[0].choices  # type: ignore[union-attr]
    assert {
        "hp6",
        "device",
        "stream",
        "record",
        "transfer",
        "wifi",
        "module",
        "diag",
        "lock",
        "catalog",
    } <= set(groups)


def test_cli_only_references_routed_commands():
    """The CLI surface must not exceed what the firmware implements.

    Scans the CLI source for the group-64 command names it dispatches, and
    asserts every one of them is actually routed by the device. A verb for an
    unreachable command could only ever return ENOTSUP.
    """
    import importlib
    import re
    from pathlib import Path

    from healthypi.smp import catalog

    # `healthypi.cli.main` is both a module and a re-exported function; take the
    # module explicitly.
    cli_main = importlib.import_module("healthypi.cli.main")
    src = Path(cli_main.__file__).read_text()
    referenced = set(re.findall(r'_simple\(\s*"([a-z0-9_]+)"', src))
    referenced |= set(re.findall(r'\bg\.([a-z0-9_]+)\(', src))
    referenced |= set(re.findall(r'\bg\[\s*"([a-z0-9_]+)"\s*\]', src))
    referenced -= {"m4fw_status"}  # not yet exposed; guard against typos below

    assert referenced, "found no command references -- the scan pattern broke"

    routed = {c.name for c in catalog.dispatchable()}
    routed |= {f"{c.name}_write" for c in catalog.dispatchable()}
    unknown = referenced - routed
    assert not unknown, f"CLI dispatches commands the device does not route: {unknown}"


def test_bare_invocation_prints_help(capsys):
    assert main([]) == 1
    assert "HealthyPi 6 host tool" in capsys.readouterr().out


def test_group_without_verb_prints_group_help(capsys):
    assert main(["hp6"]) == 1
    assert "verify" in capsys.readouterr().out


# --- catalog ---------------------------------------------------------------


def test_catalog_text(capsys):
    assert main(["catalog"]) == 0
    out = capsys.readouterr().out
    assert "group 64 (hpi)" in out
    assert "31 live, 1 stub, 11 reserved" in out


def test_catalog_json(capsys):
    assert main(["catalog", "--json"]) == 0
    doc = json.loads(capsys.readouterr().out)
    assert doc["group_id"] == 64
    assert len(doc["commands"]) == 43
    assert {c["name"] for c in doc["commands"] if c["status"] == "stub"} == {"wifi_scan"}
    assert doc["errors"]["267"] == "NO_MEDIA"


# --- hp6 verbs -------------------------------------------------------------


def test_hp6_info(recording, capsys):
    assert main(["hp6", "info", str(recording)]) == 0
    out = capsys.readouterr().out
    assert "cli-test" in out and "HP6 file" in out
    assert "'ECG': 48" in out


def test_hp6_info_on_a_bare_stream(tmp_path, recording, capsys):
    raw = tmp_path / "capture.bin"
    raw.write_bytes(recording.read_bytes()[hp6.FILE_HDR_LEN :])
    assert main(["hp6", "info", str(raw)]) == 0
    assert "raw stream (no file header)" in capsys.readouterr().out


def test_hp6_verify_ok(recording, capsys):
    assert main(["hp6", "verify", str(recording)]) == 0
    assert capsys.readouterr().out.startswith("OK")


def test_hp6_verify_reports_damage(recording, capsys):
    raw = bytearray(recording.read_bytes())
    raw[hp6.FILE_HDR_LEN + 40] ^= 0xFF
    recording.write_bytes(raw)
    assert main(["hp6", "verify", str(recording)]) == 1  # non-zero for scripting
    out = capsys.readouterr().out
    assert out.startswith("FAIL") and "CRC" in out


def test_hp6_verify_json(recording, capsys):
    assert main(["hp6", "verify", str(recording), "--json"]) == 0
    doc = json.loads(capsys.readouterr().out)
    assert doc["ok"] is True and doc["problems"] == []


def test_hp6_to_csv(recording, tmp_path, capsys):
    out = tmp_path / "csv"
    assert main(["hp6", "to-csv", str(recording), str(out)]) == 0
    assert (out / "REC0001_ecg.csv").exists()
    assert (out / "REC0001_vitals.csv").exists()
    assert "ECG" in capsys.readouterr().out


def test_hp6_repair(recording, tmp_path, capsys):
    raw = bytearray(recording.read_bytes())
    struct.pack_into("<Q", raw, 16, hp6.format.TIMESTAMP_OPEN)
    import zlib

    struct.pack_into("<I", raw, 248, zlib.crc32(bytes(raw[:248])) & 0xFFFFFFFF)
    raw += b"\x00" * 37  # a partial block
    recording.write_bytes(raw)

    assert main(["hp6", "repair", str(recording)]) == 0
    assert "repaired" in capsys.readouterr().out
    fixed = recording.with_suffix(".HP6.repaired")
    assert fixed.exists()
    assert hp6.verify(fixed).ok


def test_hp6_repair_on_a_clean_file(recording, capsys):
    assert main(["hp6", "repair", str(recording)]) == 0
    assert "already clean" in capsys.readouterr().out


def test_hp6_events_reads_the_idx(tmp_path, capsys):
    idx = tmp_path / "REC0001.IDX"
    body = struct.pack("<4sHHI", b"HP6I", 0x0200, 0, 2)
    body += struct.pack("<IQII", 5000, 256, 0, 0)
    body += struct.pack("<IQII", 10000, 4096, 1, 0)
    idx.write_bytes(body + struct.pack("<I", 0))
    assert main(["hp6", "events", str(idx)]) == 0
    out = capsys.readouterr().out
    assert "sync_records" in out and "offset=        4096" in out


def test_hp6_events_warns_on_an_incomplete_index(tmp_path, capsys):
    idx = tmp_path / "open.IDX"
    idx.write_bytes(
        struct.pack("<4sHHI", b"HP6I", 0x0200, 0, 0)
        + struct.pack("<IQII", 5000, 256, 0, 0)
    )
    assert main(["hp6", "events", str(idx)]) == 0
    assert "not closed" in capsys.readouterr().out


def test_hp6_events_accepts_the_hp6_path(tmp_path, recording, capsys):
    (tmp_path / "REC0001.IDX").write_bytes(struct.pack("<4sHHI", b"HP6I", 0x0200, 0, 0))
    assert main(["hp6", "events", str(recording)]) == 0
    assert "REC0001.IDX" in capsys.readouterr().out


# --- device verbs: arguments and failure modes -----------------------------


def test_stream_start_defaults_to_ecg_and_ppg():
    ap = build_parser()
    args = ap.parse_args(["stream", "start"])
    assert args.ch == 0x03 and args.ann == 0


def test_stream_start_accepts_hex_masks():
    ap = build_parser()
    assert ap.parse_args(["stream", "start", "--ch", "0x09"]).ch == 0x09
    assert ap.parse_args(["stream", "start", "--ch", "9"]).ch == 9


def test_frame_size_default_matches_the_validated_value():
    """A larger frame than the device MTU is silently dropped, not rejected."""
    ap = build_parser()
    assert ap.parse_args(["device", "info"]).frame_size == 256


def test_module_power_maps_on_off_to_bool():
    ap = build_parser()
    assert ap.parse_args(["module", "power", "0", "on"]).state == "on"
    with pytest.raises(SystemExit):
        ap.parse_args(["module", "power", "0", "maybe"])


def test_missing_device_is_a_clean_error(capsys, monkeypatch):
    from healthypi.transport import serial_smp

    monkeypatch.setattr(serial_smp, "candidates", lambda: [])
    rc = main(["device", "info"])
    assert rc == 3
    assert "Is the device plugged in?" in capsys.readouterr().err


def test_record_help_explains_how_to_retrieve_files():
    """Firmware 1.0.0 has no download command; the CLI must say so rather than
    leaving the user hunting for a `get` verb."""
    ap = build_parser()
    groups = ap._subparsers._group_actions[0].choices  # type: ignore[union-attr]
    assert "mass storage" in groups["record"].epilog
    assert "get" not in groups["record"]._subparsers._group_actions[0].choices  # type: ignore[union-attr]


def test_transfer_help_warns_the_connection_drops():
    ap = build_parser()
    groups = ap._subparsers._group_actions[0].choices  # type: ignore[union-attr]
    assert "re-enumerates" in groups["transfer"].epilog


def test_device_verb_without_the_device_stack(capsys, monkeypatch):
    """Installed without the `device` extra, a device verb must say so in one
    line -- not raise ImportError out of whichever verb imported it first."""
    import builtins

    real_import = builtins.__import__

    def blocked(name, *a, **kw):
        if name.startswith("smp") or "group64" in name:
            raise ImportError("No module named 'smpclient'")
        return real_import(name, *a, **kw)

    monkeypatch.setattr(builtins, "__import__", blocked)
    for mod in [m for m in list(__import__("sys").modules) if "group64" in m]:
        monkeypatch.delitem(__import__("sys").modules, mod, raising=False)

    rc = main(["device", "info", "--port", "/dev/null"])
    assert rc == 4
    err = capsys.readouterr().err
    assert "healthypi[device]" in err
    assert "Traceback" not in err
