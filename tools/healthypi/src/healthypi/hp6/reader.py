# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT

"""Decode ``.HP6`` frames from a file or a live stream.

One decoder serves both, because the firmware emits byte-identical DBLK blocks
to the SD file and to CDC0. The only difference this module cares about is the
optional 256-byte file header, which is detected by magic rather than assumed.

Design rule: **never abort on bad data.** A stream is expected to have gaps --
the device drops on a full consumer ring rather than back-pressuring
acquisition -- so a corrupt or truncated block resyncs to the next ``DBLK`` and
the loss is counted, not raised.
"""

from __future__ import annotations

import io
import os
from dataclasses import dataclass, field
from typing import BinaryIO, Iterator

from .format import (
    DBLK_CRC_LEN,
    DBLK_HDR_LEN,
    DBLK_MAGIC,
    DBLK_MAX_LEN,
    DBLK_OVERHEAD,
    FILE_HDR_LEN,
    FILE_MAGIC,
    Channel,
    FileHeader,
    Hp6Error,
    Sample,
    _DBLK_HDR,
    block_crc,
    payload_for,
)


@dataclass(slots=True)
class Block:
    """One decoded DBLK block."""

    seq: int
    t_ms: int
    channel: int
    flags: int
    sample_count: int
    payload: bytes
    #: absolute offset in the source (file offset, header included)
    offset: int
    crc_ok: bool
    #: total bytes of this block, magic..crc inclusive
    block_len: int

    @property
    def end(self) -> int:
        """Absolute offset one past this block -- the truncation point."""
        return self.offset + self.block_len

    @property
    def sample_size(self) -> int:
        """Derived, not tabulated -- see HP6_FORMAT.md §3."""
        return len(self.payload) // self.sample_count if self.sample_count else 0

    @property
    def channel_name(self) -> str:
        try:
            return Channel(self.channel).name
        except ValueError:
            return f"UNKNOWN({self.channel})"

    def samples(self) -> list[Sample]:
        """Decode the payload. Empty for a channel with no known struct."""
        cls = payload_for(self.channel)
        if cls is None:
            return []
        n = min(self.sample_count, len(self.payload) // cls.SIZE)
        return [cls.unpack_from(self.payload, i * cls.SIZE) for i in range(n)]


@dataclass(slots=True)
class ReadStats:
    blocks: int = 0
    bytes_consumed: int = 0
    crc_errors: int = 0
    resync_bytes: int = 0
    seq_gaps: int = 0
    lost_blocks: int = 0
    size_mismatches: int = 0
    truncated_tail: int = 0
    per_channel: dict[str, int] = field(default_factory=dict)
    samples: dict[str, int] = field(default_factory=dict)

    @property
    def clean(self) -> bool:
        return (
            self.crc_errors == 0
            and self.resync_bytes == 0
            and self.seq_gaps == 0
            and self.size_mismatches == 0
            and self.truncated_tail == 0
        )


class _Scanner:
    """Incremental DBLK scanner over a byte source."""

    def __init__(
        self,
        src: BinaryIO,
        stats: ReadStats,
        chunk: int = 1 << 16,
        base_offset: int = 0,
    ):
        self._src = src
        self._buf = bytearray()
        # Absolute source offset of _buf[0]. Seeded with where the caller
        # started reading, so Block.offset is a real file offset even when the
        # scan begins after the 256-byte header -- repair truncates on it.
        self._base = base_offset
        self._chunk = chunk
        self._eof = False
        self.stats = stats

    def _fill(self, need: int) -> bool:
        while len(self._buf) < need and not self._eof:
            data = self._src.read(self._chunk)
            if not data:
                self._eof = True
                break
            self._buf += data
        return len(self._buf) >= need

    def _drop(self, n: int) -> None:
        del self._buf[:n]
        self._base += n

    def _resync(self) -> bool:
        """Advance to the next DBLK magic. Returns False at end of input."""
        while True:
            idx = self._buf.find(DBLK_MAGIC, 1)
            if idx >= 0:
                self.stats.resync_bytes += idx
                self._drop(idx)
                return True
            # Keep the last 3 bytes: the magic may straddle the boundary.
            keep = min(len(self._buf), 3)
            if len(self._buf) > keep:
                self.stats.resync_bytes += len(self._buf) - keep
                self._drop(len(self._buf) - keep)
            if self._eof:
                self.stats.resync_bytes += len(self._buf)
                self._drop(len(self._buf))
                return False
            if not self._fill(len(self._buf) + 1):
                self.stats.resync_bytes += len(self._buf)
                self._drop(len(self._buf))
                return False

    def blocks(self) -> Iterator[Block]:
        prev_seq: int | None = None
        while True:
            if not self._fill(DBLK_HDR_LEN):
                if self._buf:
                    self.stats.truncated_tail += len(self._buf)
                return
            if bytes(self._buf[:4]) != DBLK_MAGIC:
                if not self._resync():
                    return
                continue

            magic, block_len, seq, t_ms, ch, flags, n, _rsvd = _DBLK_HDR.unpack_from(
                self._buf, 0
            )
            if block_len < DBLK_OVERHEAD or block_len > DBLK_MAX_LEN:
                self.stats.crc_errors += 1
                if not self._resync():
                    return
                continue
            if not self._fill(block_len):
                # Trailing partial block: normal at the end of a live capture.
                self.stats.truncated_tail += len(self._buf)
                return

            raw = bytes(self._buf[:block_len])
            offset = self._base
            got = int.from_bytes(raw[-DBLK_CRC_LEN:], "little")
            crc_ok = got == block_crc(raw)
            if not crc_ok:
                self.stats.crc_errors += 1
                if not self._resync():
                    return
                continue

            payload = raw[DBLK_HDR_LEN : block_len - DBLK_CRC_LEN]
            blk = Block(
                seq=seq,
                t_ms=t_ms,
                channel=ch,
                flags=flags,
                sample_count=n,
                payload=payload,
                offset=offset,
                crc_ok=True,
                block_len=block_len,
            )

            cls = payload_for(ch)
            if cls is not None and n:
                if len(payload) // n != cls.SIZE:
                    self.stats.size_mismatches += 1

            if prev_seq is not None and seq != prev_seq + 1:
                if seq > prev_seq:
                    self.stats.seq_gaps += 1
                    self.stats.lost_blocks += seq - prev_seq - 1
            prev_seq = seq

            self.stats.blocks += 1
            self.stats.bytes_consumed += block_len
            name = blk.channel_name
            self.stats.per_channel[name] = self.stats.per_channel.get(name, 0) + 1
            self.stats.samples[name] = self.stats.samples.get(name, 0) + n

            self._drop(block_len)
            yield blk


def iter_blocks(
    src: BinaryIO, stats: ReadStats | None = None, base_offset: int = 0
) -> Iterator[Block]:
    """Yield every decodable block from an already-positioned byte source.

    ``base_offset`` is where in the source the scan starts, so ``Block.offset``
    stays absolute.
    """
    return _Scanner(
        src, stats if stats is not None else ReadStats(), base_offset=base_offset
    ).blocks()


def read_stream(data: bytes | BinaryIO, stats: ReadStats | None = None) -> Iterator[Block]:
    """Decode a bare frame sequence (a live CDC0 capture -- no file header)."""
    src = io.BytesIO(data) if isinstance(data, (bytes, bytearray)) else data
    return iter_blocks(src, stats)


def read_file(
    path: str | os.PathLike, stats: ReadStats | None = None
) -> tuple[FileHeader | None, Iterator[Block]]:
    """Open a ``.HP6`` file (or a headerless capture) for streaming decode.

    Returns ``(header_or_None, block_iterator)``. The iterator is lazy and holds
    the file open, so it is valid only while the caller consumes it -- that is
    what lets a 2 GiB recording be exported in constant memory.
    """
    fh = open(path, "rb")
    head = fh.read(4)
    if head == FILE_MAGIC:
        fh.seek(0)
        raw = fh.read(FILE_HDR_LEN)
        header = FileHeader.unpack(raw)
        data_start = header.header_size or FILE_HDR_LEN
        fh.seek(data_start)
    elif head == DBLK_MAGIC:
        header = None
        data_start = 0
        fh.seek(0)
    else:
        fh.close()
        raise Hp6Error(f"not a .HP6 file or stream: leading bytes {head!r}")
    return header, iter_blocks(fh, stats, base_offset=data_start)


@dataclass(slots=True)
class VerifyReport:
    path: str
    header: FileHeader | None
    stats: ReadStats
    header_crc_ok: bool
    sync_chain_ok: bool
    sync_markers: int
    counts_match: bool
    problems: list[str] = field(default_factory=list)

    @property
    def ok(self) -> bool:
        return not self.problems


def verify(path: str | os.PathLike) -> VerifyReport:
    """Full-file check: header CRC, every block CRC, seq continuity, sync chain
    and the header's sample counters against what was actually decoded."""
    from .format import SYNC_MAGIC, SyncSample

    stats = ReadStats()
    header, blocks = read_file(path, stats)
    problems: list[str] = []
    syncs = 0
    sync_ok = True
    prev_sync_seq: int | None = None

    for blk in blocks:
        if blk.channel == Channel.SYNC:
            syncs += 1
            for s in blk.samples():
                assert isinstance(s, SyncSample)
                if s.magic != SYNC_MAGIC:
                    sync_ok = False
                    problems.append(
                        f"sync marker at 0x{blk.offset:x} has magic 0x{s.magic:08X}"
                    )
                if prev_sync_seq is not None and s.seq != prev_sync_seq + 1:
                    sync_ok = False
                    problems.append(
                        f"sync seq jumped {prev_sync_seq} -> {s.seq} at 0x{blk.offset:x}"
                    )
                prev_sync_seq = s.seq

    if stats.crc_errors:
        problems.append(f"{stats.crc_errors} block(s) failed CRC")
    if stats.seq_gaps:
        problems.append(
            f"{stats.seq_gaps} sequence gap(s), {stats.lost_blocks} block(s) lost"
        )
    if stats.resync_bytes:
        problems.append(f"{stats.resync_bytes} byte(s) skipped resyncing")
    if stats.size_mismatches:
        problems.append(
            f"{stats.size_mismatches} block(s) whose per-sample size "
            "disagrees with the canonical struct -- firmware/host format skew"
        )
    if stats.truncated_tail:
        problems.append(f"{stats.truncated_tail} trailing byte(s) are a partial block")

    header_crc_ok = True
    counts_match = True
    if header is not None:
        header_crc_ok = header.crc_ok
        if not header.crc_ok:
            problems.append("file header CRC mismatch")
        if header.is_open:
            problems.append("file was never closed (timestamp_end unset) -- try repair")
        for name, declared in (
            ("ECG", header.ecg_samples),
            ("PPG", header.ppg_samples),
            ("VITALS", header.vitals_samples),
        ):
            seen = stats.samples.get(name, 0)
            if declared and declared != seen:
                counts_match = False
                problems.append(
                    f"{name}: header declares {declared} samples, decoded {seen}"
                )

    return VerifyReport(
        path=str(path),
        header=header,
        stats=stats,
        header_crc_ok=header_crc_ok,
        sync_chain_ok=sync_ok,
        sync_markers=syncs,
        counts_match=counts_match,
        problems=problems,
    )
