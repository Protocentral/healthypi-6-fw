# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT

"""Export decoded ``.HP6`` data to CSV.

Streaming throughout: the SRS requires this to run on 2 GiB files, so nothing
here accumulates samples in memory. One CSV per channel, written as blocks are
decoded.

Timestamps: a block carries ``t_ms`` for its *first* sample only, so per-sample
time is interpolated from the channel's rate. That is exact for a contiguous
block and drifts only across a gap -- where the samples genuinely do not exist.
"""

from __future__ import annotations

import csv
import os
from dataclasses import dataclass
from typing import Iterable

from .format import Channel, payload_for
from .reader import Block, ReadStats, read_file

#: fallback rates when no file header is present (a bare capture)
_DEFAULT_RATE = {
    Channel.ECG: 500,
    Channel.PPG: 250,
    Channel.VITALS: 1,
    Channel.EEG: 250,
}


@dataclass(slots=True)
class ExportResult:
    files: dict[str, str]
    rows: dict[str, int]
    stats: ReadStats


def export_csv(
    path: str | os.PathLike,
    outdir: str | os.PathLike,
    *,
    channels: Iterable[int] | None = None,
    prefix: str | None = None,
) -> ExportResult:
    """Write ``<prefix>_<channel>.csv`` for each channel present."""
    outdir = str(outdir)
    os.makedirs(outdir, exist_ok=True)
    src = str(path)
    if prefix is None:
        prefix = os.path.splitext(os.path.basename(src))[0]

    wanted = set(channels) if channels is not None else None
    stats = ReadStats()
    header, blocks = read_file(src, stats)

    rates = dict(_DEFAULT_RATE)
    if header is not None:
        # Driven off the header's per-channel array rather than four named
        # lookups, so a channel added to the format is picked up here without
        # anyone remembering to come back. A 0 means event-rate (INFER, EVENT)
        # or absent, and leaves the fallback in place.
        for ch in Channel:
            hz = header.rate_of(ch)
            if hz:
                rates[ch] = hz

    handles: dict[int, tuple] = {}
    rows: dict[str, int] = {}
    files: dict[str, str] = {}

    try:
        for blk in blocks:
            if wanted is not None and blk.channel not in wanted:
                continue
            cls = payload_for(blk.channel)
            if cls is None:
                continue
            samples = blk.samples()
            if not samples:
                continue

            if blk.channel not in handles:
                name = blk.channel_name.lower()
                fpath = os.path.join(outdir, f"{prefix}_{name}.csv")
                fh = open(fpath, "w", newline="")
                w = csv.writer(fh)
                w.writerow(["t_ms", *cls.CSV_FIELDS])
                handles[blk.channel] = (fh, w)
                files[blk.channel_name] = fpath
                rows[blk.channel_name] = 0

            _fh, w = handles[blk.channel]
            rate = rates.get(blk.channel, 0) or 0
            step = 1000.0 / rate if rate else 0.0
            for i, s in enumerate(samples):
                t = blk.t_ms + (i * step)
                w.writerow([round(t, 3) if step else blk.t_ms, *s.csv_row()])
            rows[blk.channel_name] += len(samples)
    finally:
        for fh, _w in handles.values():
            fh.close()

    return ExportResult(files=files, rows=rows, stats=stats)
