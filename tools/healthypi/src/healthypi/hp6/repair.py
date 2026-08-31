# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT

"""Recover an unclean ``.HP6`` file.

A card yank or a crash leaves ``timestamp_end`` unset and the header counters
stale. The in-band sync markers make recovery decidable: every 5 s the firmware
writes an ``HPI_CH_SYNC`` frame, so **the last valid sync marker is the last
point the file is known-good**. Everything after it is kept only if it decodes
cleanly; the moment decoding breaks, that is where the file is truncated.

Output is written to a new file -- the damaged original is never modified,
because it is the only copy of whatever the truncated tail contained.
"""

from __future__ import annotations

import os
import shutil
from dataclasses import dataclass

from .format import Channel, FILE_HDR_LEN, SYNC_MAGIC, SyncSample
from .reader import ReadStats, read_file


@dataclass(slots=True)
class RepairResult:
    src: str
    dst: str
    truncated_at: int
    dropped_bytes: int
    last_sync_seq: int | None
    last_sync_offset: int | None
    blocks_kept: int
    was_clean: bool
    stats: ReadStats


def repair(
    path: str | os.PathLike, out_path: str | os.PathLike | None = None
) -> RepairResult:
    """Recover ``path`` into ``<path>.repaired`` (or ``out_path``)."""
    src = str(path)
    dst = str(out_path) if out_path else src + ".repaired"

    stats = ReadStats()
    header, blocks = read_file(src, stats)

    good_end = FILE_HDR_LEN if header is not None else 0
    last_sync_seq: int | None = None
    last_sync_off: int | None = None
    kept = 0
    ecg = ppg = vit = eeg = 0

    for blk in blocks:
        # A block only counts as recovered once it has been fully decoded with a
        # valid CRC, which iter_blocks has already established by yielding it.
        good_end = blk.end
        kept += 1
        if blk.channel == Channel.ECG:
            ecg += blk.sample_count
        elif blk.channel == Channel.PPG:
            ppg += blk.sample_count
        elif blk.channel == Channel.VITALS:
            vit += blk.sample_count
        elif blk.channel == Channel.EEG:
            eeg += blk.sample_count
        elif blk.channel == Channel.SYNC:
            for s in blk.samples():
                if isinstance(s, SyncSample) and s.magic == SYNC_MAGIC:
                    last_sync_seq = s.seq
                    last_sync_off = blk.offset

    total = os.path.getsize(src)
    dropped = total - good_end
    was_clean = (
        dropped == 0
        and stats.crc_errors == 0
        and stats.truncated_tail == 0
        and (header is None or not header.is_open)
    )

    shutil.copyfile(src, dst)
    with open(dst, "r+b") as fh:
        fh.truncate(good_end)
        if header is not None:
            header.set_samples(Channel.ECG, ecg)
            header.set_samples(Channel.PPG, ppg)
            header.set_samples(Channel.VITALS, vit)
            header.set_samples(Channel.EEG, eeg)
            if header.is_open:
                # No trustworthy end wall-clock exists; anchor it to the start
                # plus the decoded duration rather than inventing one.
                header.timestamp_end = header.timestamp_start + header.duration_ms
            fh.seek(0)
            fh.write(header.pack())

    return RepairResult(
        src=src,
        dst=dst,
        truncated_at=good_end,
        dropped_bytes=dropped,
        last_sync_seq=last_sync_seq,
        last_sync_offset=last_sync_off,
        blocks_kept=kept,
        was_clean=was_clean,
        stats=stats,
    )
