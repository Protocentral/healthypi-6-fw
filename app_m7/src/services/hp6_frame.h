/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * .HP6 "DBLK" data block -- the ONE frame for both the live stream (CDC0) and
 * the recorded file (one parser everywhere). Little-endian, byte-packed.
 *
 *   off size field
 *   0    4   magic "DBLK"
 *   4    4   block_len: total bytes of this frame (magic .. crc32 inclusive)
 *   8    4   seq: monotonic per stream/file (gap = loss)
 *   12   8   t_ms: device-monotonic ms of the first sample in this block
 *   20   1   channel: hpi_channel_id_t (1=ECG,2=PPG,3=RESP,4=VITALS,5=EEG)
 *   21   1   flags
 *   22   2   sample_count
 *   24   4   reserved (0)
 *   28   N   samples: sample_count x canonical payload struct (sample_formats.h)
 *   28+N 4   crc32 (IEEE/zlib) over bytes [0 .. 28+N-1]
 *
 * Per-sample byte size = (block_len - 28 - 4) / sample_count, so a reader needs
 * no per-channel size table; the canonical structs (hp6_ecg_sample=20,
 * hp6_ppg_sample=12, hp6_vitals=16, hp6_eeg_sample=36, hp6_infer_sample=16)
 * are carried verbatim from the sample bus. Those sizes are BUILD_ASSERTed in
 * core/sample_formats.h -- do not restate them anywhere a compiler cannot check.
 */

#ifndef HPI_SERVICES_HP6_FRAME_H
#define HPI_SERVICES_HP6_FRAME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HP6_DBLK_MAGIC0 'D'
#define HP6_DBLK_MAGIC1 'B'
#define HP6_DBLK_MAGIC2 'L'
#define HP6_DBLK_MAGIC3 'K'

#define HP6_DBLK_HDR_LEN  28u   /* magic..reserved */
#define HP6_DBLK_CRC_LEN   4u
#define HP6_DBLK_OVERHEAD (HP6_DBLK_HDR_LEN + HP6_DBLK_CRC_LEN)  /* 32 */

/* Header field byte offsets (for explicit little-endian writes). */
#define HP6_DBLK_OFF_MAGIC        0
#define HP6_DBLK_OFF_BLOCK_LEN    4
#define HP6_DBLK_OFF_SEQ          8
#define HP6_DBLK_OFF_T_MS         12
#define HP6_DBLK_OFF_CHANNEL      20
#define HP6_DBLK_OFF_FLAGS        21
#define HP6_DBLK_OFF_SAMPLE_COUNT 22
#define HP6_DBLK_OFF_RESERVED     24
#define HP6_DBLK_OFF_PAYLOAD      28

#ifdef __cplusplus
}
#endif

#endif /* HPI_SERVICES_HP6_FRAME_H */
