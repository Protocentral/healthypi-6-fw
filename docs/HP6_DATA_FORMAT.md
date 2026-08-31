# The `.HP6` data format

How to read HealthyPi 6 data in your own code — from a recorded file on the SD
card, or from the live USB stream. Both carry **the same frames**: write one
parser and it handles both.

- [1. Overview](#1-overview)
- [2. Stream vs. file](#2-stream-vs-file)
- [3. The `DBLK` frame](#3-the-dblk-frame)
- [4. Channels and sample payloads](#4-channels-and-sample-payloads)
- [5. The file header](#5-the-file-header)
- [6. Sync markers and sidecars](#6-sync-markers-and-sidecars)
- [7. How to write a reader](#7-how-to-write-a-reader)
- [8. A complete parser in 60 lines](#8-a-complete-parser-in-60-lines)
- [9. Things that will bite you](#9-things-that-will-bite-you)

Format version **`0x0300`**, firmware 1.0.0. All multi-byte fields are
**little-endian**; all structures are **packed** — no padding beyond the
explicit pad bytes shown.

---

## 1. Overview

A `.HP6` stream is a sequence of self-describing blocks:

```
"DBLK" | length | seq | timestamp | channel | count | samples… | CRC-32
```

Each block holds N samples of one signal (ECG, PPG, vitals, EEG). A recorded
file is the same sequence with a 256-byte header in front and periodic sync
markers mixed in. That's the whole format.

For a quick look without writing code:

```bash
pip install protocentral-healthypi
healthypi hp6 to-csv REC0001.HP6 ./out/     # per-channel CSVs
healthypi hp6 verify REC0001.HP6            # CRCs, gaps, sample counts
```

---

## 2. Stream vs. file

| | Live stream (USB CDC 0) | Recorded file (`.HP6` on SD) |
|---|---|---|
| 256-byte file header | no — blocks start at byte 0 | yes |
| `DBLK` blocks | **identical** | **identical** |
| Sync markers every 5 s | no | yes, in-band |
| `.IDX` / `.TXT` sidecars | no | yes, alongside the file |
| Gaps | expected | only if the recording ended badly |

Detect which one you have from the first four bytes: `"HPI6"` means a file —
parse the header, then read blocks from offset 256; `"DBLK"` means a bare
stream — read blocks immediately.

**A saved stream capture is not a valid `.HP6` file** — it has no header.
It is still readable, but prepend a header if other tools must accept it.

---

## 3. The `DBLK` frame

| Offset | Size | Type | Field | Meaning |
|---|---|---|---|---|
| 0 | 4 | char[4] | `magic` | Always `"DBLK"` (`0x44 0x42 0x4C 0x4B`) |
| 4 | 4 | uint32 | `block_len` | **Total** block size including magic and CRC = `32 + N` |
| 8 | 4 | uint32 | `seq` | Increments by 1 per block. A jump means blocks were lost |
| 12 | 8 | uint64 | `t_ms` | Milliseconds since device boot, of the **first** sample in this block |
| 20 | 1 | uint8 | `channel` | Which signal — see §4 |
| 21 | 1 | uint8 | `flags` | Producer flags; reserved, currently 0 |
| 22 | 2 | uint16 | `sample_count` (`n`) | Samples in this block |
| 24 | 4 | uint32 | `reserved` | Zero |
| 28 | N | — | `payload` | `n` × the sample struct for this channel |
| 28+N | 4 | uint32 | `crc32` | CRC-32 (IEEE 802.3, same as `zlib.crc32`) over bytes `[0, 28+N)` |

**Sample size is derived, not looked up:** `S = (block_len − 32) / sample_count`.
Your parser keeps working when an unknown channel appears, and for known
channels a mismatch between `S` and the expected size means firmware and parser
disagree — stop rather than misinterpret.

Typical sizes at the shipped batch size of 16 samples per block: ECG
`32 + 16×20` = 352 B (~31 blocks/s at 500 Hz), PPG `32 + 16×12` = 224 B
(~16 blocks/s at 250 Hz), VITALS `32 + 1×16` = 48 B (~1 block/s).

**`t_ms` is milliseconds since boot, not wall-clock.** Only the first sample is
stamped; derive the rest as `sample_time(i) = t_ms + i × (1000 / rate_hz)`.
For wall-clock time a file gives `timestamp_start` in the header and a fresh
`wall_ms` in every sync marker; a live stream carries no wall clock — stamp on
arrival if you need one.

---

## 4. Channels and sample payloads

| ID | Channel | Struct | Size | Rate |
|---|---|---|---|---|
| 1 | ECG | `ecg_sample` | 20 B | 500 Hz |
| 2 | PPG | `ppg_sample` | 12 B | 250 Hz (configurable 62–2000) |
| 3 | RESP | — | — | Respiration is carried **inside** the ECG sample; there are no RESP blocks |
| 4 | VITALS | `vitals_sample` | 16 B | ~1 Hz |
| 5 | EEG | `eeg_sample` | 36 B | 250 Hz (expansion module) |
| 6 | EVENT | `event` | 8 B | Only when something happens (not sampled) |
| 7 | SYNC | `sync_marker` | 40 B | Every 5 s, files only |
| 8 | INFER | `infer_sample` | 16 B | One per classified beat (not sampled) |

### ECG — 20 bytes

```c
struct ecg_sample {
    int32_t  resp;      /* respiration (thoracic impedance), microvolts */
    int32_t  lead_i;    /* Lead I  = LA − RA, microvolts */
    int32_t  lead_ii;   /* Lead II = LL − RA, microvolts */
    int32_t  v1;        /* V1 precordial, microvolts */
    uint8_t  lead_off;  /* bit0=RA  bit1=LA  bit2=LL  bit3=V1  (1 = electrode off) */
    uint8_t  flags;
    uint16_t _pad;
};
```

Python: `struct.unpack("<4iBB2x", buf)` → `(resp, lead_i, lead_ii, v1, lead_off, flags)`

Leads III, aVR, aVL and aVF are derived, not transmitted: `III = II − I`,
`aVR = −(I + II)/2`, `aVL = I − II/2`, `aVF = II − I/2`.

`lead_off` names **electrodes** (debounced ~100 ms), not AFE channels. A set
bit means the trace on that lead is a floating-input artefact, not a signal.
**It is always 0 in firmware 1.0.0**, where the lead-off comparators were never
enabled — treat a 0 from a 1.0.0 recording as "unknown", not "all on".

### PPG — 12 bytes

```c
struct ppg_sample {
    int32_t red;        /* LED1, raw ADC counts */
    int32_t ir;         /* LED2, raw ADC counts */
    uint8_t lead_off;
    uint8_t _pad[3];
};
```

Python: `struct.unpack("<2iB3x", buf)`

**Raw counts, not a physical unit** — 22-bit signed values sign-extended into
int32. Do not scale them; compute ratios.

### VITALS — 12 bytes

```c
struct vitals_sample {
    uint16_t hr_bpm;         /* heart rate, bpm */
    uint16_t spo2_x10;       /* SpO2 percent × 10  (975 = 97.5 %) */
    uint16_t rr_bpm;         /* respiration rate — always 0 in 1.0.0 */
    int16_t  temp_c_x100;    /* °C × 100 — always 0 in 1.0.0 */
    uint16_t hrv_sdnn_ms;    /* SDNN, milliseconds */
    uint16_t hrv_rmssd_ms;   /* RMSSD, milliseconds */
    uint16_t hrv_lf_hf_x10;  /* LF/HF ratio × 10; 0 = not computed */
    uint8_t  flags;          /* see below */
    uint8_t  _pad;
};
```

Python: `struct.unpack("<HHHhHHHBx", buf)` → `(hr_bpm, spo2_x10, rr_bpm, temp_c_x100, hrv_sdnn_ms, hrv_rmssd_ms, hrv_lf_hf_x10, flags)`

> **Changed in `0x0300`.** The HRV fields were `uint8` milliseconds, clamped at
> 255, and there was no field for the frequency-domain result. The clamp was
> silent and it bit exactly where the number matters: SDNN above 255 ms occurs
> in healthy young adults at rest and routinely in atrial fibrillation, and a
> clamped 255 could not be distinguished from a measured one. The payload grew
> 12 B → 16 B.

**Zero means "not available", not "measured zero."** In 1.0.0, `rr_bpm` and
`temp_c_x100` always read 0 (no producer / no sensor). Render unavailable values
as blank — never as a measurement.

#### `flags` — where `hr_bpm` came from

| Bit | Name | Meaning |
|-----|------|---------|
| 0 | `HR_FROM_PPG` | `hr_bpm` is a **PPG pulse rate**, not an ECG heart rate |
| 1 | `ECG_LEAD_OFF` | at least one ECG electrode was off; ECG-derived rates are suppressed while set |
| 2 | `PPG_WEAK` | PPG perfusion was low — **`spo2_x10` and any PPG-sourced `hr_bpm` are provisional**. Set whenever the PPG signal was poor, whichever sensor supplied `hr_bpm` |

The device prefers the ECG rate and falls back to the PPG pulse rate when the
ECG one is unavailable (leads off, or no beats for 5 s). **A rate with
`HR_FROM_PPG` set must not be labelled or analysed as an ECG heart rate.**

### EEG — 36 bytes

```c
struct eeg_sample {
    int32_t ch[8];      /* channels 1..8, microvolts */
    uint8_t lead_off;   /* bit N = channel N+1 electrode off */
    uint8_t _pad[3];
};
```

Python: `struct.unpack("<8iB3x", buf)`

Only present when an EEG expansion module is fitted and enabled.

### INFER — 16 bytes

```c
struct infer_sample {
    uint32_t ts_ms;        /* beat instant, ms of DEVICE UPTIME (see note) */
    uint16_t model_id;     /* which network produced this (0 = unknown) */
    uint8_t  class_id;     /* argmax of scores[] — see the class table */
    uint8_t  confidence;   /* winning score + 128 (reversible; 128 = score 0) */
    int8_t   scores[5];    /* raw per-class scores, as the network returned */
    uint8_t  flags;        /* see below */
    uint8_t  _pad[2];
};
```

Python: `struct.unpack("<IHBB5bB2x", buf)`

One block per classified beat from a HealthyLink compute module — **event-rate,
not sampled**, so the block header's `sample_rate` is 0 and the file header
carries no rate for channel 8.

`confidence` is the winning score **offset by +128**, not scaled, so it inverts
exactly: `score = confidence - 128`. The unmodified per-class scores are in
`scores[]` regardless.

> **`ts_ms` here is device uptime, not session-relative** — unlike `event.ts_ms`,
> which is measured from the start of the recording. The producer runs in a layer
> that cannot see the session epoch. Use it to order an inference against the
> samples around it; for wall-clock, use the enclosing block's timestamp and the
> file header's `timestamp_start`.

| `class_id` | AAMI class | Meaning |
|---|---|---|
| 0 | N | normal, or bundle-branch block |
| 1 | S | supraventricular ectopic |
| 2 | V | ventricular ectopic |
| 3 | F | fusion of ventricular and normal |
| 4 | Q | unclassifiable / paced |

| Bit | Name | Meaning |
|-----|------|---------|
| 0 | `STUB` | **not a real inference** — see below |
| 1 | `LOW_CONF` | below the model's usable confidence |
| 2 | `ECG_SUSPECT` | the input beat came from a poor-quality trace |

> **Always check `STUB` before using a result.** The compute module's inference
> path is not finished: `RUN_INFERENCE` currently returns five zero bytes. A
> producer that cannot prove it ran a network must set this bit, so that a
> recording made during bring-up can be told apart from a clinical one after the
> fact. **Treat a set `STUB` bit as "no classification", not as class N.**

### EVENT — 8 bytes

```c
struct event {
    uint32_t ts_ms;   /* ms since the recording started */
    uint16_t type;    /* 1 = user mark; other values reserved */
    uint16_t seq;     /* 1-based, per recording */
};
```

Python: `struct.unpack("<IHH", buf)`

Appears only when something happens, always with `sample_count = 1`; the only
type today is **1, user mark**. Read events **from the stream** — the in-band
block survives an interrupted recording, unlike the `.IDX` sidecar. The
header's `event_count`/`events_offset` (offsets 158/162) are conveniences
filled on clean close (`events_offset` points at the first EVENT block for
seeking); both read 0 in a cut-short file, but the blocks are still there.
Skip unknown `type` values and keep going — new types arrive without a version
bump.

---

## 5. The file header

The first 256 bytes of a `.HP6` file. Absent from live streams.

| Offset | Size | Type | Field |
|---|---|---|---|
| 0 | 4 | char[4] | `"HPI6"` |
| 4 | 2 | uint16 | `version` — `0x0300` |
| 6 | 2 | uint16 | `header_size` — 256 |
| 8 | 8 | uint64 | `timestamp_start` — Unix ms; **0 if the clock was never set** |
| 16 | 8 | uint64 | `timestamp_end` — Unix ms; `0xFFFFFFFFFFFFFFFF` if still open |
| 24 | 4 | uint32 | `duration_ms` |
| 28 | 32 | char[32] | `patient_id` (NUL-padded) |
| 60 | 64 | char[64] | `session_name` |
| 124 | 4 | uint32 | `channels` — bit *N* set means channel *N* appears in the file (bit 1 = ECG, … bit 8 = INFER) |
| 128 | 2×8 | uint16 | `rate_hz[8]` — Hz, indexed by **channel id − 1**; 0 = event-rate or absent |
| 144 | 4×8 | uint32 | `sample_count[8]` — same indexing |
| 176 | 4 | uint32 | `event_count` |
| 180 | 8 | uint64 | `events_offset` |
| 188 | 16 | char[16] | `firmware_version` |
| 204 | 8 | char[8] | `board_variant` |
| 212 | 16 | char[16] | `serial_number` |
| 228 | 20 | uint8[20] | reserved (zero) |
| 248 | 4 | uint32 | `header_crc32` — CRC-32 over bytes `[0, 248)` |
| 252 | 4 | char[4] | `"HP6E"` |

Python: `struct.unpack("<4sHHQQI32s64sI8H8IIQ16s8s16s20sI4s", buf)`

> **Changed in `0x0300`.** `channels` used to be an ad-hoc bitmask
> (`0x01` ECG, `0x02` PPG, `0x04` RESP, `0x08` EEG) that did not match the
> channel ids used everywhere else in this format, and the per-channel rates and
> counts were five *named* fields. That was full at five entries, so every new
> channel cost a format version and a hand-edit in three codebases. Both are now
> indexed by channel id, with eight slots — enough for the channels that exist
> plus room to add without another break.

Validate: `"HPI6"` at 0, `"HP6E"` at 252, CRC over the first 248 bytes.
**Use the header's rates, not the defaults in this document** — the PPG rate in
particular is configurable, and the header records what was actually used.

`timestamp_end == 0xFFFFFFFFFFFFFFFF` means the file was never closed (card
removed, power lost, reset). The sample counters and `duration_ms` are then
stale too; data up to the last valid sync marker is still good (§6).

---

## 6. Sync markers and sidecars

### Sync markers (in-band)

Every 5 seconds a recording gets a block on channel 7 carrying a 40-byte marker:

```c
struct sync_marker {
    uint32_t magic;                    /* 0xDEADBEEF */
    uint32_t seq;                      /* marker sequence, from 0 */
    uint64_t wall_ms;                  /* Unix ms at this point */
    uint32_t ecg_count, ppg_count, eeg_count, vitals_count;
    uint32_t events_since_last_sync;
    uint32_t running_crc32;            /* CRC of frame bytes since the previous marker */
};
```

Python: `struct.unpack("<IIQIIIIII", buf)`

They make an interrupted file recoverable — **everything up to the last marker
whose data validates is known-good** — and provide wall-clock re-anchoring and
running totals for cheap seeking in large recordings.

### `.IDX` — the index sidecar

A `REC0001.IDX` next to `REC0001.HP6`:

```
12 B header : "HP6I" | uint16 version | uint16 event_count | uint32 sync_count
N × 20 B    : uint32 ts_ms | uint64 file_offset | uint32 seq | uint32 crc
 4 B footer : uint32 footer_crc32
```

**Advisory only.** `sync_count` is written on clean close (0 for an interrupted
file — exactly when you want an index), the footer CRC is a zero placeholder,
and `event_count` is **always 0** (marks live in-band on channel 6). The
in-band markers in the `.HP6` are authoritative; fall back to scanning them.

### `.TXT` — human-readable summary

Written alongside each recording. Not intended for parsing.

---

## 7. How to write a reader

1. **Read 4 bytes.** `"HPI6"` → parse the 256-byte header, verify its CRC, seek
   to `header_size`. `"DBLK"` → seek back to 0. Anything else → not our data.
2. **Read a 28-byte block header.** Check `magic == "DBLK"`.
3. **Sanity-check `block_len`** — at least 32, and below a bound you pick
   (64 KB is generous). A bad length is corruption, not a huge block.
4. **Read the rest of the block** and verify the CRC-32 over everything but the
   last 4 bytes.
5. **Decode** `sample_count` payload structs; derive `S = (block_len − 32) / n`
   and check it against the expected size for known channels.
6. **On any failure — bad magic, implausible length, CRC mismatch — do not
   abort.** Scan forward for the next `"DBLK"`, count the skipped bytes, carry
   on. On a live stream the device drops frames rather than stall acquisition;
   gaps and partial blocks are normal, not errors.
7. **Track `seq`.** A gap means blocks were lost. Report it; those samples do
   not exist anywhere.

---

## 8. A complete parser in 60 lines

```python
import struct, zlib

DBLK = b"DBLK"
HDR = struct.Struct("<4sIIQBBHI")     # magic, len, seq, t_ms, ch, flags, n, rsvd
LAYOUT = {                              # channel -> (struct, size)
    1: (struct.Struct("<4iBB2x"), 20),    # ECG:    resp, I, II, V1, lead_off, flags
    2: (struct.Struct("<2iB3x"), 12),     # PPG:    red, ir, lead_off
    4: (struct.Struct("<HHHhHHHBx"), 18), # VITALS: hr, spo2x10, rr, tempx100,
                                          #         sdnn_ms, rmssd_ms, lf_hf_x10, flags
    5: (struct.Struct("<8iB3x"), 36),     # EEG:    ch0..7, lead_off
    7: (struct.Struct("<IIQIIIIII"), 40), # SYNC
    8: (struct.Struct("<IHBB5bB2x"), 16), # INFER:  ts_ms, model, class, conf,
                                          #         scores[5], flags
}

def read_hp6(path):
    """Yield (channel, seq, t_ms, [sample tuples]) for every valid block."""
    data = open(path, "rb").read()
    pos = 0

    if data[:4] == b"HPI6":                       # a file, not a raw stream
        assert data[252:256] == b"HP6E", "bad header terminator"
        assert zlib.crc32(data[:248]) & 0xFFFFFFFF == \
               struct.unpack_from("<I", data, 248)[0], "bad header CRC"
        pos = struct.unpack_from("<H", data, 6)[0]   # header_size

    while pos + 28 <= len(data):
        if data[pos:pos + 4] != DBLK:             # resync
            nxt = data.find(DBLK, pos + 1)
            if nxt < 0:
                return
            pos = nxt
            continue

        _, block_len, seq, t_ms, ch, _flags, n, _ = HDR.unpack_from(data, pos)
        if not (32 <= block_len <= 65536) or pos + block_len > len(data):
            nxt = data.find(DBLK, pos + 1)
            if nxt < 0:
                return
            pos = nxt
            continue

        blk = data[pos:pos + block_len]
        if zlib.crc32(blk[:-4]) & 0xFFFFFFFF != \
           struct.unpack_from("<I", blk, block_len - 4)[0]:
            nxt = data.find(DBLK, pos + 1)         # corrupt: skip it
            if nxt < 0:
                return
            pos = nxt
            continue

        payload = blk[28:block_len - 4]
        entry = LAYOUT.get(ch)
        samples = []
        if entry and n:
            s, size = entry
            if len(payload) // n == size:          # format check
                samples = [s.unpack_from(payload, i * size) for i in range(n)]
        yield ch, seq, t_ms, samples
        pos += block_len


for ch, seq, t_ms, samples in read_hp6("REC0001.HP6"):
    if ch == 1:                                    # ECG
        for i, (resp, lead_i, lead_ii, v1, lead_off, flags) in enumerate(samples):
            print(t_ms + i * 2, lead_i, lead_ii, v1)     # 500 Hz -> 2 ms/sample
```

For multi-gigabyte recordings, read incrementally with the same logic — or use
the `healthypi` Python package, which does.

---

## 9. Things that will bite you

- **Sequence gaps are normal on a live stream.** The device drops rather than
  back-pressuring acquisition. Many gaps = your reader isn't draining the USB
  endpoint fast enough; read continuously in a dedicated thread.
- **`t_ms` is uptime, not wall time.** Anchor with `timestamp_start` and the
  sync markers.
- **Zero is not a measurement.** `rr_bpm`, `temp_c_x100`, and the HRV fields
  read 0 when unavailable — in 1.0.0, always for the first two.
- **PPG values are raw counts.** Not a physical unit; not comparable across
  devices.
- **Signed fields are signed.** ECG/EEG/PPG values, `temp_c_x100`, and the
  `ibat_ma` telemetry field. Reading them as unsigned produces
  plausible-looking garbage on negative excursions — for ECG, half the
  waveform.
- **`board_variant` is unreliable in firmware 1.0.0** — it reports `v4`
  regardless of the board. Don't branch on it; fixed in a later release.
- **Don't trust the `.IDX` for an interrupted file.** See §6.
- **Nothing enforces block ordering between channels.** Blocks interleave in
  production order; sort by `t_ms` per channel, don't assume a pattern.

---

## See also

- [HOST_INTERFACE.md](HOST_INTERFACE.md) — the USB transport, the control
  protocol, and how to start and stop a stream
- [MCUMGR_COMMANDS.md](MCUMGR_COMMANDS.md) — the device command reference
- [`protocentral-healthypi` on PyPI](https://pypi.org/project/protocentral-healthypi/) — reference
  implementation of everything in this document
