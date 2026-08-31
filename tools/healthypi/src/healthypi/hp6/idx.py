# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT

"""The ``.IDX`` sidecar -- sync/event table of contents.

Layout (recording_service.c ``idx_open`` / ``idx_append_sync`` / ``idx_finalize``):

    12 B header : "HP6I" | version u16 | event_count u16 | sync_count u32
    N x 20 B    : ts_ms u32 | file_off u64 | seq u32 | crc u32
     4 B footer : footer_crc32

Two caveats a host must respect:

* ``sync_count`` is patched at finalize, so it reads 0 in a file that was never
  closed -- the exact case where the index is most wanted.
* the footer CRC is **currently written as a zero placeholder** by the firmware.

Both mean the same thing: ``.IDX`` is advisory. Recovery falls back to scanning
the in-band ``HPI_CH_SYNC`` markers, which are always authoritative.
"""

from __future__ import annotations

import os
import struct
from dataclasses import dataclass

from .format import IDX_HDR_LEN, IDX_MAGIC, IDX_REC_LEN, Hp6Error

_HDR = struct.Struct("<4sHHI")
_REC = struct.Struct("<IQII")
assert _HDR.size == IDX_HDR_LEN and _REC.size == IDX_REC_LEN


@dataclass(slots=True)
class IdxEntry:
    ts_ms: int
    file_off: int
    seq: int
    crc: int


@dataclass(slots=True)
class Idx:
    version: int
    event_count: int
    sync_count: int
    entries: list[IdxEntry]
    footer_crc32: int | None
    trusted_count: bool

    @property
    def complete(self) -> bool:
        """False when the recording was not closed cleanly."""
        return self.trusted_count and self.sync_count == len(self.entries)


def read_idx(path: str | os.PathLike) -> Idx:
    with open(path, "rb") as fh:
        raw = fh.read()
    if len(raw) < IDX_HDR_LEN:
        raise Hp6Error(f"{path}: too short to be an .IDX")
    magic, version, event_count, sync_count = _HDR.unpack_from(raw, 0)
    if magic != IDX_MAGIC:
        raise Hp6Error(f"{path}: bad magic {magic!r}, expected {IDX_MAGIC!r}")

    body = raw[IDX_HDR_LEN:]
    footer = None
    n_rec = len(body) // IDX_REC_LEN
    if len(body) % IDX_REC_LEN == 4:
        footer = int.from_bytes(body[-4:], "little")
        body = body[:-4]
        n_rec = len(body) // IDX_REC_LEN

    entries = [IdxEntry(*_REC.unpack_from(body, i * IDX_REC_LEN)) for i in range(n_rec)]
    return Idx(
        version=version,
        event_count=event_count,
        sync_count=sync_count,
        entries=entries,
        footer_crc32=footer,
        trusted_count=sync_count > 0,
    )


def idx_path_for(hp6_path: str | os.PathLike) -> str:
    base, _ = os.path.splitext(str(hp6_path))
    return base + ".IDX"
