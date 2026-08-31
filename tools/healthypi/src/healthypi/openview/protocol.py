# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT

"""OpenView packets -- the Wi-Fi streaming format.

The M7 speaks only ``.HP6``; the ESP32-C6 co-processor repacks samples into
OpenView frames for Wi-Fi consumers, over TCP (port 5000) and UDP broadcast
(port 5001).

**v2 (50 bytes) is the current format.** v1 (46 bytes) has no sequence number,
is marked legacy in the co-processor firmware and is emitted by nothing; it is
recognised here only so a v1 stream produces a clear diagnosis instead of
garbage. Layout transcribed from the co-processor's ``openview_protocol.h`` and
``openview_pack_packet()``.

Stdlib only.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

SYNC0 = 0x0A
SYNC1 = 0xFA
FOOTER0 = 0x00
FOOTER1 = 0x0B

VERSION_V1 = 0x00  # legacy 46-byte format, not decoded
VERSION_V2 = 0x02

TYPE_DATA = 0x02
TYPE_HRV = 0x03
TYPE_EEG = 0x04

#: 5 header + 4 sequence + 39 payload + 2 footer
PACKET_LEN = 50
PACKET_LEN_V1 = 46
#: 5 header + 20 payload + 2 footer
HRV_PACKET_LEN = 27
#: 5 header + 46 payload + 2 footer
EEG_PACKET_LEN = 51
EEG_CHANNELS = 8

#: byte 2 = frame length, 3 = version, 4 = type
_META = struct.Struct("<BBBBB")

#: seq u32 | ecg1 ecg2 ecg3 resp ppg_red ppg_ir (6 x i32) | ppg_valid u8
#: | hr u16 | spo2 u8 | resp_rate u8 | temp u16 | adc1 adc2 (2 x i32)
_BODY = struct.Struct("<I6iBHBBH2i")
assert _BODY.size == 43  # 4 seqnum + 39 payload


class OpenViewError(Exception):
    pass


@dataclass(slots=True)
class Packet:
    """One decoded v2 data packet."""

    seq: int
    ecg1: int
    ecg2: int
    ecg3: int
    respiration: int
    ppg_red: int
    ppg_ir: int
    ppg_valid: bool
    heart_rate: int
    spo2: int
    respiration_rate: int
    temperature_cc: int  # centidegrees C: 3700 = 37.00 C
    adc_ch1: int
    adc_ch2: int

    @property
    def temperature_c(self) -> float | None:
        return self.temperature_cc / 100.0 if self.temperature_cc else None


def find_sync(buf: bytes, start: int = 0) -> int:
    """Index of the next packet start, or -1."""
    i, end = start, len(buf) - 1
    while i < end:
        if buf[i] == SYNC0 and buf[i + 1] == SYNC1:
            return i
        i += 1
    return -1


def peek(buf: bytes, off: int = 0) -> tuple[int, int, int]:
    """``(frame_length, version, type)`` of the header at ``off``."""
    if len(buf) - off < 5:
        raise OpenViewError("truncated header")
    s0, s1, length, version, ftype = _META.unpack_from(buf, off)
    if s0 != SYNC0 or s1 != SYNC1:
        raise OpenViewError("bad sync bytes")
    return length, version, ftype


def packet_length(version: int, ftype: int) -> int:
    if ftype == TYPE_HRV:
        return HRV_PACKET_LEN
    if ftype == TYPE_EEG:
        return EEG_PACKET_LEN
    return PACKET_LEN_V1 if version == VERSION_V1 else PACKET_LEN


def unpack(buf: bytes, off: int = 0) -> Packet:
    """Decode one v2 data packet starting at ``off``."""
    _length, version, ftype = peek(buf, off)
    if version == VERSION_V1:
        raise OpenViewError(
            "OpenView v1 (46-byte) packet: this is pre-v2 co-processor firmware; "
            "only v2 is supported"
        )
    if version != VERSION_V2:
        raise OpenViewError(f"unsupported OpenView version 0x{version:02X}")
    if ftype != TYPE_DATA:
        raise OpenViewError(f"not a data packet (type 0x{ftype:02X})")
    if len(buf) - off < PACKET_LEN:
        raise OpenViewError("short packet")
    if buf[off + PACKET_LEN - 2] != FOOTER0 or buf[off + PACKET_LEN - 1] != FOOTER1:
        raise OpenViewError("bad footer")

    (
        seq,
        ecg1,
        ecg2,
        ecg3,
        resp,
        red,
        ir,
        ppg_valid,
        hr,
        spo2,
        rr,
        temp,
        adc1,
        adc2,
    ) = _BODY.unpack_from(buf, off + 5)
    return Packet(
        seq=seq,
        ecg1=ecg1,
        ecg2=ecg2,
        ecg3=ecg3,
        respiration=resp,
        ppg_red=red,
        ppg_ir=ir,
        ppg_valid=ppg_valid == 0xFF,
        heart_rate=hr,
        spo2=spo2,
        respiration_rate=rr,
        temperature_cc=temp,
        adc_ch1=adc1,
        adc_ch2=adc2,
    )


def pack(p: Packet) -> bytes:
    """Encode a v2 data packet -- the inverse of :func:`unpack` (tests, replay)."""
    head = _META.pack(SYNC0, SYNC1, 0x2B, VERSION_V2, TYPE_DATA)
    body = _BODY.pack(
        p.seq,
        p.ecg1,
        p.ecg2,
        p.ecg3,
        p.respiration,
        p.ppg_red,
        p.ppg_ir,
        0xFF if p.ppg_valid else 0x00,
        p.heart_rate,
        p.spo2,
        p.respiration_rate,
        p.temperature_cc,
        p.adc_ch1,
        p.adc_ch2,
    )
    return head + body + bytes((FOOTER0, FOOTER1))


def iter_packets(buf: bytes):
    """Yield decoded packets, resyncing past anything unparseable."""
    pos = 0
    while pos < len(buf):
        idx = find_sync(buf, pos)
        if idx < 0 or len(buf) - idx < PACKET_LEN:
            return
        try:
            yield unpack(buf, idx)
            pos = idx + PACKET_LEN
        except OpenViewError:
            pos = idx + 1
