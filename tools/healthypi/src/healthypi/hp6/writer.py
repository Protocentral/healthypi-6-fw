# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT

"""Encode ``.HP6`` blocks and files.

Two real uses, neither of them "write fake data":

1. ``hpi stream capture`` -- a live CDC0 capture is a *bare frame sequence*, not
   a valid ``.HP6`` file. Turning one into a file means synthesizing the 256-byte
   header around the frames the device actually sent.
2. Test fixtures -- an encoder that is exactly the inverse of the decoder is how
   a round-trip test proves the decoder without needing hardware attached.
"""

from __future__ import annotations

import struct
import zlib
from typing import Iterable, Sequence

from .format import (
    DBLK_CRC_LEN,
    DBLK_HDR_LEN,
    DBLK_MAGIC,
    FILE_HDR_LEN,
    FILE_VERSION,
    HDR_CHANNEL_SLOTS,
    Channel,
    FileHeader,
    Sample,
    TIMESTAMP_OPEN,
    _DBLK_HDR,
)

# HPI_STREAM_CH_* bitfield (stream_service.h). This is the STREAM's channel
# mask, used by the group-64 stream-control command. It is NOT the file header's
# `channels` field: that became a channel-id bitmask in format 0x0300, so use
# `1 << Channel.X` there. The two used to share these constants and did not mean
# the same thing. There is deliberately no VITALS bit in the stream mask:
# vitals ride along whenever the M4 produces them.
STREAM_CH_ECG = 0x01
STREAM_CH_PPG = 0x02
STREAM_CH_RESP = 0x04
STREAM_CH_EEG = 0x08


def encode_block(
    channel: int,
    seq: int,
    t_ms: int,
    samples: Sequence[Sample] | bytes,
    *,
    flags: int = 0,
    sample_count: int | None = None,
) -> bytes:
    """Build one DBLK block, CRC included."""
    if isinstance(samples, (bytes, bytearray)):
        payload = bytes(samples)
        n = sample_count if sample_count is not None else 0
    else:
        payload = b"".join(s.pack() for s in samples)  # type: ignore[attr-defined]
        n = len(samples) if sample_count is None else sample_count

    block_len = DBLK_HDR_LEN + len(payload) + DBLK_CRC_LEN
    head = _DBLK_HDR.pack(
        DBLK_MAGIC, block_len, seq, t_ms, channel, flags, n, 0
    )
    body = head + payload
    return body + struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF)


def _slots(rates: dict) -> tuple[int, ...]:
    """Build the header's per-channel rate array from a {Channel: Hz} map."""
    out = [0] * HDR_CHANNEL_SLOTS
    for ch, hz in rates.items():
        out[int(ch) - 1] = hz
    return tuple(out)


def new_header(
    *,
    session_name: str = "",
    patient_id: str = "",
    firmware_version: str = "",
    board_variant: str = "",
    serial_number: str = "",
    timestamp_start: int = 0,
    channels: int | None = None,
    ecg_rate: int = 500,
    ppg_rate: int = 250,
    vitals_rate: int = 1,
    eeg_rate: int = 0,
) -> FileHeader:
    """A header for a file that is still open (``timestamp_end`` unset)."""
    return FileHeader(
        version=FILE_VERSION,
        header_size=FILE_HDR_LEN,
        timestamp_start=timestamp_start,
        timestamp_end=TIMESTAMP_OPEN,
        duration_ms=0,
        patient_id=patient_id,
        session_name=session_name,
        channels=(
            channels
            if channels is not None
            else (1 << Channel.ECG) | (1 << Channel.PPG) | (1 << Channel.VITALS)
        ),
        rate_hz=_slots({
            Channel.ECG: ecg_rate,
            Channel.PPG: ppg_rate,
            Channel.VITALS: vitals_rate,
            Channel.EEG: eeg_rate,
        }),
        sample_count=(0,) * HDR_CHANNEL_SLOTS,
        event_count=0,
        events_offset=0,
        firmware_version=firmware_version,
        board_variant=board_variant,
        serial_number=serial_number,
        header_crc32=0,
        crc_ok=True,
    )


#: Channels that get a per-channel count in the header. EVENT and SYNC do not:
#: EVENT has its own `event_count`, and SYNC markers are structure, not data.
_COUNTED = (Channel.ECG, Channel.PPG, Channel.RESP, Channel.VITALS,
            Channel.EEG, Channel.INFER)


class FileWriter:
    """Write a ``.HP6`` file, keeping the header's counters honest.

    The header is written twice -- once as a placeholder so the data starts at
    offset 256, once at close with the real counters, duration and
    ``timestamp_end``. That is exactly what the firmware does, and it is why an
    unclosed file is detectable.
    """

    def __init__(self, path, header: FileHeader | None = None):
        self._fh = open(path, "wb")
        self._hdr = header or new_header()
        self._fh.write(self._hdr.pack())
        self._first_t: int | None = None
        self._last_t: int = 0

    def write_block(self, block: bytes) -> None:
        self._fh.write(block)

    def add(
        self, channel: int, seq: int, t_ms: int, samples: Sequence[Sample]
    ) -> None:
        self.write_block(encode_block(channel, seq, t_ms, samples))
        if self._first_t is None:
            self._first_t = t_ms
        self._last_t = t_ms
        if channel in _COUNTED:
            i = int(channel) - 1
            counts = list(self._hdr.sample_count)
            counts[i] += len(samples)
            self._hdr.sample_count = tuple(counts)
            self._hdr.channels |= 1 << int(channel)

    def close(self, *, timestamp_end: int | None = None) -> None:
        if self._first_t is not None:
            self._hdr.duration_ms = self._last_t - self._first_t
        if timestamp_end is not None:
            self._hdr.timestamp_end = timestamp_end
        elif self._hdr.timestamp_start:
            self._hdr.timestamp_end = self._hdr.timestamp_start + self._hdr.duration_ms
        else:
            self._hdr.timestamp_end = self._hdr.duration_ms
        self._fh.seek(0)
        self._fh.write(self._hdr.pack())
        self._fh.close()

    def __enter__(self) -> "FileWriter":
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        if exc_type is None:
            self.close()
        else:
            self._fh.close()


def wrap_capture(
    frames: bytes | Iterable[bytes], out_path, header: FileHeader | None = None
) -> None:
    """Turn a raw CDC0 capture into a valid ``.HP6`` file.

    The counters are recomputed by decoding what was captured rather than
    trusted from anywhere -- a capture is lossy by design, so the device's own
    idea of how many samples it sent does not describe this file.
    """
    from .reader import ReadStats, read_stream

    blob = frames if isinstance(frames, (bytes, bytearray)) else b"".join(frames)
    stats = ReadStats()
    first_t: int | None = None
    last_t = 0
    for blk in read_stream(bytes(blob), stats):
        if first_t is None:
            first_t = blk.t_ms
        last_t = blk.t_ms

    hdr = header or new_header()
    # Driven off the Channel enum rather than a hand-written list, so a channel
    # added to the format lands here without anyone remembering to come back.
    for ch in _COUNTED:
        n = stats.samples.get(ch.name, 0)
        hdr.set_samples(ch, n)
        if n:
            hdr.channels |= 1 << int(ch)
    hdr.duration_ms = (last_t - first_t) if first_t is not None else 0
    hdr.timestamp_end = hdr.timestamp_start + hdr.duration_ms

    with open(out_path, "wb") as fh:
        fh.write(hdr.pack())
        fh.write(bytes(blob))
