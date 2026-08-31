# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT

"""Binary layout of the ``.HP6`` container.

Transcribed from the firmware, which is the source of truth:

* ``app_m7/src/services/hp6_frame.h``      -- the DBLK block
* ``app_m7/src/core/sample_formats.h``     -- the canonical payload structs
* ``app_m7/src/core/channel_registry.h``   -- the channel ids
* ``app_m7/src/services/recording_service.c`` -- the 256 B file header, sync marker

The same DBLK block is emitted on the live CDC0 stream and written into the
recorded file; only the container around it differs (the file adds the 256-byte
header, in-band sync markers and sidecars -- the stream has none of those).
See docs/HP6_DATA_FORMAT.md.

Stdlib only, on purpose: decoding a recording must never require a device stack.
"""

from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass
from enum import IntEnum
from typing import ClassVar

# --- DBLK block ------------------------------------------------------------

DBLK_MAGIC = b"DBLK"
DBLK_HDR_LEN = 28
DBLK_CRC_LEN = 4
DBLK_OVERHEAD = DBLK_HDR_LEN + DBLK_CRC_LEN  # 32

#: Upper bound used when resyncing. The largest block the firmware emits today
#: is ECG at 16 x 20 B = 352 B; this is deliberately loose so a future batch
#: size does not trip it, but tight enough that a corrupt length is rejected
#: rather than causing a multi-megabyte read.
DBLK_MAX_LEN = 65536

_DBLK_HDR = struct.Struct("<4sIIQBBHI")  # magic, block_len, seq, t_ms, ch, flags, n, rsvd
assert _DBLK_HDR.size == DBLK_HDR_LEN

# --- file container --------------------------------------------------------

FILE_MAGIC = b"HPI6"
FILE_MAGIC_END = b"HP6E"
FILE_HDR_LEN = 256
FILE_VERSION = 0x0300
FILE_CRC_COVERS = 248  # header_crc32 is computed over bytes [0, 248)

#: timestamp_end while the file is open (i.e. was never cleanly closed).
TIMESTAMP_OPEN = 0xFFFFFFFFFFFFFFFF

IDX_MAGIC = b"HP6I"
IDX_HDR_LEN = 12
IDX_REC_LEN = 20

#: HP6_SYNC_MAGIC in recording_service.c
SYNC_MAGIC = 0xDEADBEEF

# Canonical rates. PPG follows CONFIG_AFE4400_PRF_HZ on the device; the header
# carries the real value per recording, so this is only a fallback for streams.
ECG_RATE_HZ = 500
PPG_RATE_HZ = 250
EEG_RATE_HZ = 250


class Channel(IntEnum):
    ECG = 1
    PPG = 2
    RESP = 3  # rides inside the ECG frame; never appears as its own block today
    VITALS = 4
    EEG = 5
    EVENT = 6
    SYNC = 7
    INFER = 8  # classified beats from a HealthyLink compute module


#: Slots in the file header's ``rate_hz`` / ``sample_count`` arrays. Indexed by
#: ``Channel - 1``, so slot 0 is ECG. Sized past the channels that exist so the
#: next one does not cost a format version.
HDR_CHANNEL_SLOTS = 8


class Hp6Error(Exception):
    """Malformed container."""


# --- canonical payloads ----------------------------------------------------


class Sample:
    """Base for the canonical per-sample structs."""

    _STRUCT: ClassVar[struct.Struct]
    SIZE: ClassVar[int]
    CSV_FIELDS: ClassVar[tuple[str, ...]]

    @classmethod
    def unpack_from(cls, buf: bytes, off: int = 0) -> "Sample":
        raise NotImplementedError

    def csv_row(self) -> list:
        raise NotImplementedError


#: ``EcgSample.lead_off`` bits. Electrodes, not AFE channels: the device maps
#: the ADS1294R's LOFF_STATP/LOFF_STATN bits onto these so a reader never has to
#: know the channel wiring. Always 0 in firmware 1.0.0, where the lead-off
#: comparators were never enabled.
LEAD_OFF_RA = 0x01
LEAD_OFF_LA = 0x02
LEAD_OFF_LL = 0x04
LEAD_OFF_V1 = 0x08

#: Bit -> electrode name, in mask order (for rendering "RA, LL").
LEAD_OFF_NAMES: dict[int, str] = {
    LEAD_OFF_RA: "RA",
    LEAD_OFF_LA: "LA",
    LEAD_OFF_LL: "LL",
    LEAD_OFF_V1: "V1",
}


def lead_off_names(mask: int) -> list[str]:
    """The electrodes flagged off in `mask`, e.g. ``["RA", "LL"]``."""
    return [n for bit, n in LEAD_OFF_NAMES.items() if mask & bit]


@dataclass(slots=True)
class EcgSample(Sample):
    """20 B. Respiration shares the ECG frame (ADS1294R CH1)."""

    resp: int  # uV
    lead_i: int  # uV
    lead_ii: int  # uV
    v1: int  # uV
    lead_off: int  # LEAD_OFF_* bits: RA, LA, LL, V1 (debounced by the device)
    flags: int

    _STRUCT: ClassVar[struct.Struct] = struct.Struct("<4iBBH")
    SIZE: ClassVar[int] = 20
    CSV_FIELDS: ClassVar[tuple[str, ...]] = (
        "lead_i_uv",
        "lead_ii_uv",
        "v1_uv",
        "resp_uv",
        "lead_off",
    )

    @classmethod
    def unpack_from(cls, buf: bytes, off: int = 0) -> "EcgSample":
        resp, li, lii, v1, lo, fl, _pad = cls._STRUCT.unpack_from(buf, off)
        return cls(resp, li, lii, v1, lo, fl)

    def pack(self) -> bytes:
        return self._STRUCT.pack(
            self.resp, self.lead_i, self.lead_ii, self.v1, self.lead_off, self.flags, 0
        )

    def csv_row(self) -> list:
        return [self.lead_i, self.lead_ii, self.v1, self.resp, self.lead_off]


@dataclass(slots=True)
class PpgSample(Sample):
    """12 B. AFE4400 raw counts, 22-bit sign-extended -- not a physical unit."""

    red: int
    ir: int
    lead_off: int

    _STRUCT: ClassVar[struct.Struct] = struct.Struct("<2iB3x")
    SIZE: ClassVar[int] = 12
    CSV_FIELDS: ClassVar[tuple[str, ...]] = ("red", "ir", "lead_off")

    @classmethod
    def unpack_from(cls, buf: bytes, off: int = 0) -> "PpgSample":
        red, ir, lo = cls._STRUCT.unpack_from(buf, off)
        return cls(red, ir, lo)

    def pack(self) -> bytes:
        return self._STRUCT.pack(self.red, self.ir, self.lead_off)

    def csv_row(self) -> list:
        return [self.red, self.ir, self.lead_off]


#: ``VitalsSample.flags`` bits -- where ``hr_bpm`` came from, and why it may be
#: missing. A heart rate with no provenance is ambiguous: the ECG QRS detector
#: and the PPG pulse detector are not the same measurement, and a floating ECG
#: electrode produces confident nonsense. Firmware >= 1.1.0; zero on older
#: recordings, which is indistinguishable from "ECG, leads on" -- check the
#: file's firmware version before drawing conclusions from a 0.
VIT_HR_FROM_PPG = 0x01
VIT_ECG_LEAD_OFF = 0x02
VIT_PPG_WEAK = 0x04


@dataclass(slots=True)
class VitalsSample(Sample):
    """12 B, ~1 Hz, computed on the M4.

    ``rr_bpm`` and ``temp_c_x100`` are structurally present but always 0 in
    firmware 1.0.0 -- respiration rate is not derived and no temperature sensor
    is wired. The ``*_or_none`` accessors exist so a caller renders them as
    absent rather than as a measured zero.

    ``flags`` (``VIT_*``) says which sensor produced ``hr_bpm`` and whether the
    PPG signal was good enough to trust.

    Widened in format ``0x0300``: the HRV fields were ``uint8`` milliseconds
    clamped at 255, which clipped silently and clipped where it matters (SDNN
    above 255 ms occurs in healthy young adults at rest and routinely in atrial
    fibrillation, and a clamped 255 could not be told from a measured one), and
    the frequency-domain result had no field at all. 12 B -> 16 B.
    """

    hr_bpm: int
    spo2_x10: int
    rr_bpm: int
    temp_c_x100: int
    hrv_sdnn: int  # ms
    hrv_rmssd: int  # ms
    hrv_lf_hf_x10: int = 0  # LF/HF ratio x10; 0 = not computed
    flags: int = 0  # VIT_*

    _STRUCT: ClassVar[struct.Struct] = struct.Struct("<HHHhHHHBx")
    SIZE: ClassVar[int] = 16
    CSV_FIELDS: ClassVar[tuple[str, ...]] = (
        "hr_bpm",
        "hr_source",
        "spo2_pct",
        "rr_bpm",
        "temp_c",
        "hrv_sdnn_ms",
        "hrv_rmssd_ms",
        "hrv_lf_hf",
        "ecg_lead_off",
    )

    @classmethod
    def unpack_from(cls, buf: bytes, off: int = 0) -> "VitalsSample":
        hr, spo2, rr, temp, sdnn, rmssd, lfhf, flags = cls._STRUCT.unpack_from(
            buf, off
        )
        return cls(hr, spo2, rr, temp, sdnn, rmssd, lfhf, flags)

    def pack(self) -> bytes:
        return self._STRUCT.pack(
            self.hr_bpm,
            self.spo2_x10,
            self.rr_bpm,
            self.temp_c_x100,
            self.hrv_sdnn,
            self.hrv_rmssd,
            self.hrv_lf_hf_x10,
            self.flags,
        )

    @property
    def hr_source(self) -> str | None:
        """``"ppg"``, ``"ecg"``, or None when there is no heart rate."""
        if not self.hr_bpm:
            return None
        return "ppg" if self.flags & VIT_HR_FROM_PPG else "ecg"

    @property
    def ecg_lead_off(self) -> bool:
        """At least one ECG electrode was off when this sample was produced."""
        return bool(self.flags & VIT_ECG_LEAD_OFF)

    @property
    def ppg_weak(self) -> bool:
        """PPG perfusion was low.

        Treat **both** ``spo2_x10`` and a PPG-sourced ``hr_bpm`` as provisional.
        The flag follows the PPG signal, not the heart-rate arbitration, so it
        can be set on a sample whose ``hr_bpm`` came from the ECG -- there, it
        qualifies the SpO2 alone.
        """
        return bool(self.flags & VIT_PPG_WEAK)

    @property
    def hr_or_none(self) -> int | None:
        return self.hr_bpm or None

    @property
    def spo2_or_none(self) -> float | None:
        return self.spo2_x10 / 10.0 if self.spo2_x10 else None

    @property
    def rr_or_none(self) -> int | None:
        return self.rr_bpm or None

    @property
    def temp_c_or_none(self) -> float | None:
        return self.temp_c_x100 / 100.0 if self.temp_c_x100 else None

    @property
    def hrv_lf_hf(self) -> float | None:
        """LF/HF ratio, or None when the frequency-domain pass did not run."""
        return self.hrv_lf_hf_x10 / 10.0 if self.hrv_lf_hf_x10 else None

    def csv_row(self) -> list:
        return [
            self.hr_or_none if self.hr_or_none is not None else "",
            self.hr_source or "",
            self.spo2_or_none if self.spo2_or_none is not None else "",
            self.rr_or_none if self.rr_or_none is not None else "",
            self.temp_c_or_none if self.temp_c_or_none is not None else "",
            self.hrv_sdnn or "",
            self.hrv_rmssd or "",
            self.hrv_lf_hf if self.hrv_lf_hf is not None else "",
            int(self.ecg_lead_off),
        ]


#: ``InferSample.flags`` bits.
INF_STUB = 0x01
INF_LOW_CONF = 0x02
INF_ECG_SUSPECT = 0x04

#: ``InferSample.class_id`` -- the AAMI beat classes, in score order.
INFER_CLASSES = ("N", "S", "V", "F", "Q")


@dataclass(slots=True)
class InferSample(Sample):
    """16 B. One classified beat from a HealthyLink compute module.

    Event-rate, not sampled: one block per beat, and the file header carries no
    rate for this channel.

    **Check :attr:`is_stub` before using a result.** The compute module's
    inference path is unfinished -- ``RUN_INFERENCE`` returns five zero bytes --
    and a producer that cannot prove it ran a network sets ``INF_STUB``. A stub
    result means "no classification"; it does not mean class N.
    """

    ts_ms: int
    model_id: int
    class_id: int
    confidence: int  # winning score + 128; see `confidence_score`
    scores: tuple[int, ...]
    flags: int = 0

    _STRUCT: ClassVar[struct.Struct] = struct.Struct("<IHBB5bB2x")
    SIZE: ClassVar[int] = 16
    CSV_FIELDS: ClassVar[tuple[str, ...]] = (
        "ts_ms",
        "model_id",
        "beat_class",
        "confidence",
        "stub",
        *[f"score_{c}" for c in INFER_CLASSES],
    )

    @classmethod
    def unpack_from(cls, buf: bytes, off: int = 0) -> "InferSample":
        v = cls._STRUCT.unpack_from(buf, off)
        return cls(v[0], v[1], v[2], v[3], tuple(v[4:9]), v[9])

    def pack(self) -> bytes:
        return self._STRUCT.pack(
            self.ts_ms,
            self.model_id,
            self.class_id,
            self.confidence,
            *self.scores,
            self.flags,
        )

    @property
    def is_stub(self) -> bool:
        """True when this did not come from a real inference."""
        return bool(self.flags & INF_STUB)

    @property
    def confidence_score(self) -> int:
        """``confidence`` back as the signed score the network returned."""
        return self.confidence - 128

    @property
    def beat_class(self) -> str | None:
        """AAMI class letter, or None for a stub or an out-of-range id."""
        if self.is_stub or self.class_id >= len(INFER_CLASSES):
            return None
        return INFER_CLASSES[self.class_id]

    def csv_row(self) -> list:
        return [
            self.ts_ms,
            self.model_id,
            self.beat_class or "",
            self.confidence,
            int(self.is_stub),
            *self.scores,
        ]


@dataclass(slots=True)
class EegSample(Sample):
    """36 B. HealthyLink EEG module (ADS1299), 8 channels, uV."""

    ch: tuple[int, ...]
    lead_off: int

    _STRUCT: ClassVar[struct.Struct] = struct.Struct("<8iB3x")
    SIZE: ClassVar[int] = 36
    CSV_FIELDS: ClassVar[tuple[str, ...]] = tuple(
        [f"ch{i}_uv" for i in range(8)] + ["lead_off"]
    )

    @classmethod
    def unpack_from(cls, buf: bytes, off: int = 0) -> "EegSample":
        vals = cls._STRUCT.unpack_from(buf, off)
        return cls(tuple(vals[:8]), vals[8])

    def pack(self) -> bytes:
        return self._STRUCT.pack(*self.ch, self.lead_off)

    def csv_row(self) -> list:
        return [*self.ch, self.lead_off]


class EventType(IntEnum):
    """``hp6_event.type`` -- see ``core/sample_formats.h``."""

    USER_MARK = 1


@dataclass(slots=True)
class EventSample(Sample):
    """8 B point-in-time marker: the operator marked this instant.

    Not a sampled signal. It is written in-band, between the samples it sits
    among, which is what makes it survive an interrupted recording -- unlike the
    ``.IDX`` sidecar, whose counts are only written when the file closes
    cleanly. ``seq`` is 1-based per session, so a gap means a dropped frame
    rather than a missing mark.
    """

    ts_ms: int
    type: int
    seq: int

    _STRUCT: ClassVar[struct.Struct] = struct.Struct("<IHH")
    SIZE: ClassVar[int] = 8
    CSV_FIELDS: ClassVar[tuple[str, ...]] = ("ts_ms", "type", "seq")

    @classmethod
    def unpack_from(cls, buf: bytes, off: int = 0) -> "EventSample":
        return cls(*cls._STRUCT.unpack_from(buf, off))

    def pack(self) -> bytes:
        return self._STRUCT.pack(self.ts_ms, self.type, self.seq)

    def csv_row(self) -> list:
        return [self.ts_ms, self.type, self.seq]

    @property
    def is_user_mark(self) -> bool:
        return self.type == EventType.USER_MARK


@dataclass(slots=True)
class SyncSample(Sample):
    """40 B in-band marker, written every 5 s -- file only, never streamed.

    ``running_crc32`` covers the frame bytes since the *previous* sync, which is
    what makes crash recovery decidable: truncate to the last sync whose running
    CRC validates.
    """

    magic: int
    seq: int
    wall_ms: int
    ecg_count: int
    ppg_count: int
    eeg_count: int
    vitals_count: int
    events_since_last_sync: int
    running_crc32: int

    _STRUCT: ClassVar[struct.Struct] = struct.Struct("<IIQIIIIII")
    SIZE: ClassVar[int] = 40
    CSV_FIELDS: ClassVar[tuple[str, ...]] = ("seq", "wall_ms", "running_crc32")

    @classmethod
    def unpack_from(cls, buf: bytes, off: int = 0) -> "SyncSample":
        return cls(*cls._STRUCT.unpack_from(buf, off))

    def pack(self) -> bytes:
        return self._STRUCT.pack(
            self.magic,
            self.seq,
            self.wall_ms,
            self.ecg_count,
            self.ppg_count,
            self.eeg_count,
            self.vitals_count,
            self.events_since_last_sync,
            self.running_crc32,
        )

    def csv_row(self) -> list:
        return [self.seq, self.wall_ms, self.running_crc32]


#: channel -> payload class. A channel absent here is decodable as raw bytes;
#: the reader deliberately does not fail on an unknown channel.
PAYLOADS: dict[int, type[Sample]] = {
    Channel.ECG: EcgSample,
    Channel.PPG: PpgSample,
    Channel.VITALS: VitalsSample,
    Channel.EEG: EegSample,
    Channel.EVENT: EventSample,
    Channel.SYNC: SyncSample,
    Channel.INFER: InferSample,
}


def payload_for(channel: int) -> type[Sample] | None:
    return PAYLOADS.get(channel)


# --- file header -----------------------------------------------------------

_FILE_HDR = struct.Struct(
    "<4sHHQQI32s64sI8H8IIQ16s8s16s20sI4s"
)
assert _FILE_HDR.size == FILE_HDR_LEN


def _cstr(raw: bytes) -> str:
    return raw.split(b"\0", 1)[0].decode("utf-8", "replace")


@dataclass(slots=True)
class FileHeader:
    version: int
    header_size: int
    timestamp_start: int  # unix ms, 0 if the RTC was unset
    timestamp_end: int  # TIMESTAMP_OPEN while open
    duration_ms: int
    patient_id: str
    session_name: str
    channels: int
    rate_hz: tuple[int, ...]  # HDR_CHANNEL_SLOTS entries, indexed by Channel - 1
    sample_count: tuple[int, ...]  # same indexing
    event_count: int
    events_offset: int
    firmware_version: str
    board_variant: str
    serial_number: str
    header_crc32: int
    crc_ok: bool

    @property
    def is_open(self) -> bool:
        """True if the file was never cleanly closed (card yank, crash)."""
        return self.timestamp_end == TIMESTAMP_OPEN

    def rate_of(self, channel: int) -> int:
        """Recorded sample rate for a channel, 0 if event-rate or absent."""
        i = int(channel) - 1
        return self.rate_hz[i] if 0 <= i < len(self.rate_hz) else 0

    def samples_of(self, channel: int) -> int:
        """Recorded sample count for a channel, 0 if absent."""
        i = int(channel) - 1
        return self.sample_count[i] if 0 <= i < len(self.sample_count) else 0

    def has_channel(self, channel: int) -> bool:
        return bool(self.channels & (1 << int(channel)))

    # Named views, kept because callers and the CLI read them by name.
    @property
    def ecg_rate(self) -> int:
        return self.rate_of(Channel.ECG)

    @property
    def ppg_rate(self) -> int:
        return self.rate_of(Channel.PPG)

    @property
    def vitals_rate(self) -> int:
        return self.rate_of(Channel.VITALS)

    @property
    def eeg_rate(self) -> int:
        return self.rate_of(Channel.EEG)

    @property
    def ecg_samples(self) -> int:
        return self.samples_of(Channel.ECG)

    @property
    def ppg_samples(self) -> int:
        return self.samples_of(Channel.PPG)

    @property
    def vitals_samples(self) -> int:
        return self.samples_of(Channel.VITALS)

    @property
    def eeg_samples(self) -> int:
        return self.samples_of(Channel.EEG)

    @property
    def infer_samples(self) -> int:
        return self.samples_of(Channel.INFER)

    def set_samples(self, channel: int, count: int) -> None:
        """Set a channel's recorded sample count (repair rewrites these)."""
        i = int(channel) - 1
        if not 0 <= i < len(self.sample_count):
            raise ValueError(f"channel {channel} has no header slot")
        counts = list(self.sample_count)
        counts[i] = count
        self.sample_count = tuple(counts)

    def set_rate(self, channel: int, hz: int) -> None:
        i = int(channel) - 1
        if not 0 <= i < len(self.rate_hz):
            raise ValueError(f"channel {channel} has no header slot")
        rates = list(self.rate_hz)
        rates[i] = hz
        self.rate_hz = tuple(rates)

    @classmethod
    def unpack(cls, buf: bytes) -> "FileHeader":
        if len(buf) < FILE_HDR_LEN:
            raise Hp6Error(f"file header short: {len(buf)} < {FILE_HDR_LEN}")
        f = _FILE_HDR.unpack_from(buf, 0)
        if f[0] != FILE_MAGIC:
            raise Hp6Error(f"bad file magic {f[0]!r}, expected {FILE_MAGIC!r}")
        if f[32] != FILE_MAGIC_END:
            raise Hp6Error(f"bad trailing magic {f[32]!r}, expected {FILE_MAGIC_END!r}")
        crc = zlib.crc32(buf[:FILE_CRC_COVERS]) & 0xFFFFFFFF
        return cls(
            version=f[1],
            header_size=f[2],
            timestamp_start=f[3],
            timestamp_end=f[4],
            duration_ms=f[5],
            patient_id=_cstr(f[6]),
            session_name=_cstr(f[7]),
            channels=f[8],
            rate_hz=tuple(f[9 : 9 + HDR_CHANNEL_SLOTS]),
            sample_count=tuple(f[17 : 17 + HDR_CHANNEL_SLOTS]),
            event_count=f[25],
            events_offset=f[26],
            firmware_version=_cstr(f[27]),
            board_variant=_cstr(f[28]),
            serial_number=_cstr(f[29]),
            header_crc32=f[31],
            crc_ok=(crc == f[31]),
        )

    def pack(self) -> bytes:
        body = _FILE_HDR.pack(
            FILE_MAGIC,
            self.version,
            self.header_size,
            self.timestamp_start,
            self.timestamp_end,
            self.duration_ms,
            self.patient_id.encode()[:31],
            self.session_name.encode()[:63],
            self.channels,
            *self.rate_hz,
            *self.sample_count,
            self.event_count,
            self.events_offset,
            self.firmware_version.encode()[:15],
            self.board_variant.encode()[:7],
            self.serial_number.encode()[:15],
            b"",
            0,
            FILE_MAGIC_END,
        )
        crc = zlib.crc32(body[:FILE_CRC_COVERS]) & 0xFFFFFFFF
        return body[:FILE_CRC_COVERS] + struct.pack("<I", crc) + FILE_MAGIC_END


def block_crc(block: bytes) -> int:
    """CRC-32 (IEEE) over everything but the trailing 4 CRC bytes."""
    return zlib.crc32(block[:-DBLK_CRC_LEN]) & 0xFFFFFFFF
