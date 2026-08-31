# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT

"""``.HP6`` decode/encode tests -- no hardware required.

The encoder is the exact inverse of the decoder, so a round-trip proves the
layout; the damage tests prove the reader's promise that it never aborts on bad
data, which is the property that matters on a stream the device drops from.
"""

from __future__ import annotations

import struct
import zlib

import pytest

from healthypi import hp6
from healthypi.hp6 import format as fmt


# --- struct sizes: the firmware BUILD_ASSERTs, mirrored ---------------------


@pytest.mark.parametrize(
    "cls,size",
    [
        (hp6.EcgSample, 20),
        (hp6.PpgSample, 12),
        (hp6.VitalsSample, 16),
        (hp6.InferSample, 16),
        (hp6.EegSample, 36),
        (hp6.EventSample, 8),
        (hp6.SyncSample, 40),
    ],
)
def test_payload_sizes(cls, size):
    assert cls.SIZE == size
    assert cls._STRUCT.size == size


def test_event_roundtrip():
    ev = hp6.EventSample(ts_ms=1234, type=hp6.EventType.USER_MARK, seq=3)
    raw = ev.pack()
    assert len(raw) == hp6.EventSample.SIZE
    assert hp6.EventSample.unpack_from(raw) == ev
    assert ev.is_user_mark


def test_event_channel_is_decoded():
    """Channel 6 was documented Reserved; a reader must now decode it."""
    assert fmt.payload_for(hp6.Channel.EVENT) is hp6.EventSample


def test_frame_and_file_header_sizes():
    assert fmt._DBLK_HDR.size == fmt.DBLK_HDR_LEN == 28
    assert fmt.DBLK_OVERHEAD == 32
    assert fmt._FILE_HDR.size == fmt.FILE_HDR_LEN == 256


# --- round trips -----------------------------------------------------------


def _ecg(n=16, base=0):
    return [
        hp6.EcgSample(
            resp=base + i,
            lead_i=1000 + i,
            lead_ii=-2000 - i,
            v1=3000 + i,
            lead_off=0b0101,
            flags=0,
        )
        for i in range(n)
    ]


def _ppg(n=16):
    return [hp6.PpgSample(red=100000 + i, ir=-200000 - i, lead_off=0) for i in range(n)]


def _vitals():
    return [
        hp6.VitalsSample(
            hr_bpm=72, spo2_x10=975, rr_bpm=0, temp_c_x100=0, hrv_sdnn=42, hrv_rmssd=31
        )
    ]


def test_block_round_trip():
    samples = _ecg()
    blob = hp6.encode_block(hp6.Channel.ECG, seq=7, t_ms=1234, samples=samples)
    assert len(blob) == 32 + 16 * 20
    (blk,) = list(hp6.read_stream(blob))
    assert blk.seq == 7 and blk.t_ms == 1234
    assert blk.channel == hp6.Channel.ECG and blk.channel_name == "ECG"
    assert blk.sample_count == 16 and blk.sample_size == 20 and blk.crc_ok
    assert blk.samples() == samples


def test_signed_fields_round_trip():
    """int32 negatives and the int16 temp must survive -- a u/i slip here would
    silently mangle every recording."""
    s = hp6.EcgSample(-1, -2147483648, 2147483647, -5, 0x0F, 0xAA)
    assert hp6.EcgSample.unpack_from(s.pack()) == s
    v = hp6.VitalsSample(
        hr_bpm=65535, spo2_x10=1000, rr_bpm=12, temp_c_x100=-1234, hrv_sdnn=255,
        hrv_rmssd=0,
    )
    assert hp6.VitalsSample.unpack_from(v.pack()) == v
    assert v.temp_c_or_none == pytest.approx(-12.34)


def test_vitals_unset_fields_read_as_absent():
    v = hp6.VitalsSample(72, 0, 0, 0, 0, 0)
    assert v.hr_or_none == 72
    assert v.spo2_or_none is None  # not 0.0 %
    assert v.rr_or_none is None  # not derived in 1.0.0
    assert v.temp_c_or_none is None  # no sensor wired


def test_vitals_flags_carry_hr_provenance():
    """An HR with no provenance is ambiguous: the ECG QRS detector and the PPG
    pulse detector are different measurements, and a floating ECG electrode
    produces confident nonsense. The flags byte is what lets a reader tell."""
    ecg = hp6.VitalsSample(72, 0, 0, 0, 0, 0)  # lf_hf and flags default 0
    assert ecg.hr_source == "ecg"
    assert not ecg.ecg_lead_off and not ecg.ppg_weak

    ppg = hp6.VitalsSample(
        68, 970, 0, 0, 0, 0, 0,
        flags=hp6.VIT_HR_FROM_PPG | hp6.VIT_ECG_LEAD_OFF | hp6.VIT_PPG_WEAK,
    )
    assert ppg.hr_source == "ppg"
    assert ppg.ecg_lead_off and ppg.ppg_weak
    assert hp6.VitalsSample.unpack_from(ppg.pack()) == ppg
    assert len(ppg.pack()) == hp6.VitalsSample.SIZE == 16  # 12 B before 0x0300

    # No rate at all: the source is None, not a stale label.
    assert hp6.VitalsSample(
        0, 0, 0, 0, 0, 0, 0, flags=hp6.VIT_ECG_LEAD_OFF
    ).hr_source is None


def test_hrv_survives_past_the_old_uint8_clamp():
    """The HRV fields were uint8 ms, clamped at 255. SDNN above that occurs in
    healthy young adults at rest and routinely in AF, and a clamped 255 could not
    be told from a measured one -- so 0x0300 widened them."""
    v = hp6.VitalsSample(60, 0, 0, 0, hrv_sdnn=412, hrv_rmssd=298, hrv_lf_hf_x10=15)
    rt = hp6.VitalsSample.unpack_from(v.pack())
    assert rt.hrv_sdnn == 412 and rt.hrv_rmssd == 298
    assert rt.hrv_lf_hf == 1.5
    # 0 means "not computed", not "a ratio of zero".
    assert hp6.VitalsSample(60, 0, 0, 0, 0, 0, 0).hrv_lf_hf is None


def test_infer_stub_is_not_a_classification():
    """The compute module returns five zero bytes today. A stub result must not
    be readable as class N, or a bring-up recording looks like a clinical one."""
    stub = hp6.InferSample(
        ts_ms=1000, model_id=0, class_id=0, confidence=0,
        scores=(0, 0, 0, 0, 0), flags=hp6.INF_STUB,
    )
    assert stub.is_stub
    assert stub.beat_class is None          # NOT "N"
    assert hp6.InferSample.unpack_from(stub.pack()) == stub
    assert len(stub.pack()) == hp6.InferSample.SIZE == 16

    real = hp6.InferSample(
        ts_ms=2000, model_id=1, class_id=2, confidence=200,
        scores=(-40, -10, 120, -30, -50), flags=0,
    )
    assert not real.is_stub and real.beat_class == "V"
    assert hp6.InferSample.unpack_from(real.pack()) == real


def test_infer_channel_is_decoded():
    assert fmt.payload_for(hp6.Channel.INFER) is hp6.InferSample


def test_lead_off_names_are_electrodes():
    """The mask names electrodes, not AFE channels -- a reader must never have
    to know the ADS1294R channel wiring to render the warning."""
    assert hp6.lead_off_names(0) == []
    assert hp6.lead_off_names(hp6.LEAD_OFF_RA) == ["RA"]
    assert hp6.lead_off_names(hp6.LEAD_OFF_RA | hp6.LEAD_OFF_LL) == ["RA", "LL"]
    assert hp6.lead_off_names(0x0F) == ["RA", "LA", "LL", "V1"]


def test_mixed_stream_stats():
    parts = []
    seq = 0
    for i in range(4):
        parts.append(hp6.encode_block(hp6.Channel.ECG, seq, i * 32, _ecg()))
        seq += 1
        parts.append(hp6.encode_block(hp6.Channel.PPG, seq, i * 32, _ppg()))
        seq += 1
    parts.append(hp6.encode_block(hp6.Channel.VITALS, seq, 128, _vitals()))
    stats = hp6.ReadStats()
    blocks = list(hp6.read_stream(b"".join(parts), stats))
    assert len(blocks) == 9
    assert stats.clean
    assert stats.samples == {"ECG": 64, "PPG": 64, "VITALS": 1}
    assert stats.per_channel == {"ECG": 4, "PPG": 4, "VITALS": 1}


def test_file_round_trip(tmp_path):
    path = tmp_path / "REC0001.HP6"
    hdr = hp6.new_header(
        session_name="unit-test",
        firmware_version="1.0.0",
        board_variant="v5",
        serial_number="SN123",
        timestamp_start=1_700_000_000_000,
    )
    with hp6.FileWriter(path, hdr) as w:
        seq = 0
        for i in range(3):
            w.add(hp6.Channel.ECG, seq, i * 32, _ecg())
            seq += 1
            w.add(hp6.Channel.PPG, seq, i * 32, _ppg())
            seq += 1

    header, blocks = hp6.read_file(path)
    assert header is not None
    assert header.crc_ok
    assert header.version == hp6.FILE_VERSION
    assert header.session_name == "unit-test"
    assert header.board_variant == "v5"
    assert header.serial_number == "SN123"
    assert not header.is_open
    assert header.ecg_samples == 48 and header.ppg_samples == 48
    assert len(list(blocks)) == 6

    report = hp6.verify(path)
    assert report.ok, report.problems
    assert report.counts_match and report.header_crc_ok


def test_verify_flags_a_corrupt_block(tmp_path):
    path = tmp_path / "bad.HP6"
    with hp6.FileWriter(path) as w:
        w.add(hp6.Channel.ECG, 0, 0, _ecg())
        w.add(hp6.Channel.ECG, 1, 32, _ecg())
    raw = bytearray(path.read_bytes())
    raw[fmt.FILE_HDR_LEN + 40] ^= 0xFF  # flip a payload byte
    path.write_bytes(raw)

    report = hp6.verify(path)
    assert not report.ok
    assert any("CRC" in p for p in report.problems)


def test_reader_resyncs_past_garbage():
    good = hp6.encode_block(hp6.Channel.ECG, 0, 0, _ecg())
    good2 = hp6.encode_block(hp6.Channel.ECG, 1, 32, _ecg())
    stats = hp6.ReadStats()
    blocks = list(hp6.read_stream(good + b"\xde\xad\xbe\xef" * 8 + good2, stats))
    assert len(blocks) == 2  # never aborted
    assert stats.resync_bytes == 32
    assert not stats.clean


def test_truncated_tail_is_reported_not_raised():
    blob = hp6.encode_block(hp6.Channel.PPG, 0, 0, _ppg())
    stats = hp6.ReadStats()
    blocks = list(hp6.read_stream(blob[:-10], stats))
    assert blocks == []
    assert stats.truncated_tail > 0


def test_sequence_gap_is_counted():
    a = hp6.encode_block(hp6.Channel.ECG, 10, 0, _ecg())
    b = hp6.encode_block(hp6.Channel.ECG, 14, 32, _ecg())
    stats = hp6.ReadStats()
    list(hp6.read_stream(a + b, stats))
    assert stats.seq_gaps == 1
    assert stats.lost_blocks == 3


def test_bogus_block_len_does_not_hang():
    blob = bytearray(hp6.encode_block(hp6.Channel.ECG, 0, 0, _ecg()))
    struct.pack_into("<I", blob, 4, 0xFFFFFFF0)
    stats = hp6.ReadStats()
    assert list(hp6.read_stream(bytes(blob), stats)) == []
    assert stats.crc_errors == 1


def test_unknown_channel_is_kept_as_raw_bytes():
    blob = hp6.encode_block(99, 0, 0, b"\x01\x02\x03\x04", sample_count=1)
    (blk,) = list(hp6.read_stream(blob))
    assert blk.channel_name == "UNKNOWN(99)"
    assert blk.samples() == []
    assert blk.payload == b"\x01\x02\x03\x04"


def test_size_mismatch_is_flagged():
    """A future firmware growing hp6_ecg_sample must not decode silently."""
    blob = hp6.encode_block(hp6.Channel.ECG, 0, 0, b"\x00" * 24, sample_count=1)
    stats = hp6.ReadStats()
    list(hp6.read_stream(blob, stats))
    assert stats.size_mismatches == 1


# --- capture wrapping, export, repair --------------------------------------


def test_wrap_capture_makes_a_valid_file(tmp_path):
    parts = [
        hp6.encode_block(hp6.Channel.ECG, i, i * 32, _ecg()) for i in range(5)
    ]
    out = tmp_path / "capture.HP6"
    hp6.wrap_capture(b"".join(parts), out, hp6.new_header(session_name="cap"))

    header, blocks = hp6.read_file(out)
    assert header is not None and header.crc_ok
    assert header.ecg_samples == 80
    assert len(list(blocks)) == 5
    assert hp6.verify(out).ok


def test_export_csv(tmp_path):
    path = tmp_path / "REC.HP6"
    with hp6.FileWriter(path) as w:
        w.add(hp6.Channel.ECG, 0, 1000, _ecg(4))
        w.add(hp6.Channel.VITALS, 1, 1000, _vitals())

    res = hp6.export_csv(path, tmp_path / "csv")
    assert res.rows == {"ECG": 4, "VITALS": 1}

    ecg_lines = (tmp_path / "csv" / "REC_ecg.csv").read_text().splitlines()
    assert ecg_lines[0] == "t_ms,lead_i_uv,lead_ii_uv,v1_uv,resp_uv,lead_off"
    assert len(ecg_lines) == 5
    # 500 Hz -> 2 ms apart
    assert ecg_lines[1].startswith("1000.0,")
    assert ecg_lines[2].startswith("1002.0,")

    vit = (tmp_path / "csv" / "REC_vitals.csv").read_text().splitlines()
    assert vit[0] == (
        "t_ms,hr_bpm,hr_source,spo2_pct,rr_bpm,temp_c,"
        "hrv_sdnn_ms,hrv_rmssd_ms,hrv_lf_hf,ecg_lead_off"
    )
    # unset rr/temp are empty, not zero -- looked up by name so an added column
    # cannot silently shift what is being asserted
    cols = vit[0].split(",")
    row = vit[1].split(",")
    assert row[cols.index("rr_bpm")] == "" and row[cols.index("temp_c")] == ""
    # provenance travels with the rate: 1.0.0-era flags=0 reads as an ECG rate
    assert row[cols.index("hr_source")] == "ecg"
    assert row[cols.index("ecg_lead_off")] == "0"


def _sync(seq, wall_ms, counts=(0, 0, 0, 0)):
    return hp6.SyncSample(
        magic=hp6.SYNC_MAGIC,
        seq=seq,
        wall_ms=wall_ms,
        ecg_count=counts[0],
        ppg_count=counts[1],
        eeg_count=counts[2],
        vitals_count=counts[3],
        events_since_last_sync=0,
        running_crc32=0,
    )


def test_repair_truncates_at_the_damage(tmp_path):
    path = tmp_path / "yanked.HP6"
    with hp6.FileWriter(path) as w:
        seq = 0
        for i in range(3):
            w.add(hp6.Channel.ECG, seq, i * 32, _ecg())
            seq += 1
        w.add(hp6.Channel.SYNC, seq, 96, [_sync(0, 1_700_000_000_000)])
        seq += 1
        w.add(hp6.Channel.ECG, seq, 128, _ecg())

    # Simulate a yank: mark the header open and staple a partial block on.
    raw = bytearray(path.read_bytes())
    struct.pack_into("<Q", raw, 16, fmt.TIMESTAMP_OPEN)
    crc = zlib.crc32(bytes(raw[:248])) & 0xFFFFFFFF
    struct.pack_into("<I", raw, 248, crc)
    raw += hp6.encode_block(hp6.Channel.ECG, 5, 160, _ecg())[:100]
    path.write_bytes(raw)

    assert not hp6.verify(path).ok

    res = hp6.repair(path)
    assert not res.was_clean
    assert res.dropped_bytes == 100
    assert res.blocks_kept == 5
    assert res.last_sync_seq == 0

    report = hp6.verify(res.dst)
    assert report.ok, report.problems
    assert report.header is not None and not report.header.is_open
    assert report.header.ecg_samples == 64


def test_repair_is_a_no_op_on_a_clean_file(tmp_path):
    path = tmp_path / "clean.HP6"
    with hp6.FileWriter(path) as w:
        w.add(hp6.Channel.ECG, 0, 0, _ecg())
    res = hp6.repair(path)
    assert res.was_clean
    assert res.dropped_bytes == 0
    assert hp6.verify(res.dst).ok


# --- .IDX sidecar ----------------------------------------------------------


def test_idx_round_trip(tmp_path):
    path = tmp_path / "REC.IDX"
    body = struct.pack("<4sHHI", fmt.IDX_MAGIC, 0x0200, 0, 2)
    body += struct.pack("<IQII", 5000, 256, 0, 0xAABBCCDD)
    body += struct.pack("<IQII", 10000, 4096, 1, 0x11223344)
    body += struct.pack("<I", 0)  # footer placeholder, as the firmware writes it
    path.write_bytes(body)

    idx = hp6.read_idx(path)
    assert idx.version == 0x0200
    assert idx.sync_count == 2 and len(idx.entries) == 2
    assert idx.entries[1].file_off == 4096
    assert idx.footer_crc32 == 0  # advisory only
    assert idx.complete


def test_idx_from_an_unclosed_recording_is_not_trusted(tmp_path):
    path = tmp_path / "open.IDX"
    body = struct.pack("<4sHHI", fmt.IDX_MAGIC, 0x0200, 0, 0)  # never patched
    body += struct.pack("<IQII", 5000, 256, 0, 0)
    path.write_bytes(body)
    idx = hp6.read_idx(path)
    assert idx.sync_count == 0 and len(idx.entries) == 1
    assert not idx.trusted_count and not idx.complete
