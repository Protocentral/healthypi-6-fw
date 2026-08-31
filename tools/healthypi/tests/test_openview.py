# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT

"""OpenView v2 packet tests.

The layout is transcribed from the co-processor's ``openview_protocol.h`` and
``openview_pack_packet()``; these tests pin it to exact byte offsets so a
transcription slip cannot pass silently.
"""

from __future__ import annotations

import struct

import pytest

from healthypi import openview as ov
from healthypi.openview.monitor import Stats, _Decoder


def _pkt(seq=0, **kw):
    base = dict(
        ecg1=-1000,
        ecg2=2000,
        ecg3=-3000,
        respiration=44,
        ppg_red=167000,
        ppg_ir=160000,
        ppg_valid=True,
        heart_rate=72,
        spo2=97,
        respiration_rate=14,
        temperature_cc=3700,
        adc_ch1=-5,
        adc_ch2=6,
    )
    base.update(kw)
    return ov.Packet(seq=seq, **base)


def test_packet_is_fifty_bytes_with_the_documented_header():
    raw = ov.pack(_pkt(seq=7))
    assert len(raw) == ov.PACKET_LEN == 50
    # sync, frame length 0x2B, version 0x02, type 0x02
    assert raw[:5] == bytes((0x0A, 0xFA, 0x2B, 0x02, 0x02))
    assert raw[-2:] == bytes((0x00, 0x0B))


def test_field_offsets_match_the_firmware_packer():
    """Offsets from openview_pack_packet() -- pinned so a slip cannot hide."""
    raw = ov.pack(_pkt(seq=0x11223344))
    assert struct.unpack_from("<I", raw, 5)[0] == 0x11223344
    assert struct.unpack_from("<i", raw, 9)[0] == -1000  # ecg1
    assert struct.unpack_from("<i", raw, 13)[0] == 2000  # ecg2
    assert struct.unpack_from("<i", raw, 17)[0] == -3000  # ecg3
    assert struct.unpack_from("<i", raw, 21)[0] == 44  # respiration
    assert struct.unpack_from("<i", raw, 25)[0] == 167000  # ppg_red
    assert struct.unpack_from("<i", raw, 29)[0] == 160000  # ppg_ir
    assert raw[33] == 0xFF  # ppg_valid
    assert struct.unpack_from("<H", raw, 34)[0] == 72  # heart_rate
    assert raw[36] == 97  # spo2
    assert raw[37] == 14  # respiration_rate
    assert struct.unpack_from("<H", raw, 38)[0] == 3700  # temperature
    assert struct.unpack_from("<i", raw, 40)[0] == -5  # adc_ch1
    assert struct.unpack_from("<i", raw, 44)[0] == 6  # adc_ch2


def test_round_trip():
    p = _pkt(seq=9)
    assert ov.unpack(ov.pack(p)) == p


def test_temperature_scaling():
    assert ov.unpack(ov.pack(_pkt(temperature_cc=3700))).temperature_c == 37.0
    # 0 means "not measured", not 0 C
    assert ov.unpack(ov.pack(_pkt(temperature_cc=0))).temperature_c is None


def test_ppg_valid_flag():
    assert ov.unpack(ov.pack(_pkt(ppg_valid=False))).ppg_valid is False
    assert ov.unpack(ov.pack(_pkt(ppg_valid=True))).ppg_valid is True


def test_v1_packets_get_a_clear_diagnosis():
    """A 46-byte v1 stream must say so, not decode into nonsense."""
    raw = bytearray(ov.pack(_pkt()))
    raw[3] = ov.VERSION_V1
    with pytest.raises(ov.OpenViewError, match="v1"):
        ov.unpack(bytes(raw))


def test_bad_footer_is_rejected():
    raw = bytearray(ov.pack(_pkt()))
    raw[-1] = 0xFF
    with pytest.raises(ov.OpenViewError, match="footer"):
        ov.unpack(bytes(raw))


def test_peek_reports_header_fields():
    length, version, ftype = ov.peek(ov.pack(_pkt()))
    assert (length, version, ftype) == (0x2B, 0x02, 0x02)


def test_iter_packets_resyncs_past_noise():
    stream = ov.pack(_pkt(seq=1)) + b"\x00\x11\x22" + ov.pack(_pkt(seq=2))
    got = list(ov.iter_packets(stream))
    assert [p.seq for p in got] == [1, 2]


# --- monitor ---------------------------------------------------------------


def test_decoder_counts_gaps_and_loss():
    stats = Stats()
    dec = _Decoder(stats)
    blob = b"".join(ov.pack(_pkt(seq=s)) for s in (10, 11, 15, 16))
    got = list(dec.feed(blob))
    assert [p.seq for p in got] == [10, 11, 15, 16]
    assert stats.packets == 4
    assert stats.gaps == 1
    assert stats.lost == 3
    assert stats.expected == 7
    assert stats.loss_pct == pytest.approx(300 / 7)


def test_decoder_handles_split_packets():
    """A TCP read can land mid-packet; the decoder must buffer, not drop."""
    stats = Stats()
    dec = _Decoder(stats)
    blob = ov.pack(_pkt(seq=1)) + ov.pack(_pkt(seq=2))
    assert list(dec.feed(blob[:30])) == []
    got = list(dec.feed(blob[30:]))
    assert [p.seq for p in got] == [1, 2]
    assert stats.resync_bytes == 0


def test_decoder_counts_resync_bytes():
    stats = Stats()
    dec = _Decoder(stats)
    list(dec.feed(b"\xde\xad\xbe\xef" + ov.pack(_pkt(seq=1))))
    assert stats.packets == 1
    assert stats.resync_bytes == 4


def test_summary_shape():
    stats = Stats()
    dec = _Decoder(stats)
    list(dec.feed(ov.pack(_pkt(seq=1))))
    s = stats.summary()
    assert set(s) == {
        "packets",
        "bytes",
        "seconds",
        "rate_hz",
        "expected",
        "lost",
        "gaps",
        "loss_pct",
        "bad_packets",
        "resync_bytes",
    }
    assert s["packets"] == 1 and s["lost"] == 0
