/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Sample bus -- the spine of the rewritten data path.
 *
 * One canonical frame for every signal (onboard sensors AND HealthyLink
 * modules). Producers publish; multiple consumers each get their own
 * lock-free ring with drop-on-full + a per-consumer dropped counter, so a
 * slow consumer (e.g. UI) never back-pressures the producer or other
 * consumers (recording, stream).
 *
 * Dependency rule: producers in core/ and healthylink/ include this header
 * and HAL only -- never services/, control/, transport/ or ui/.
 */

#ifndef HPI_CORE_SAMPLE_BUS_H
#define HPI_CORE_SAMPLE_BUS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "channel_registry.h"   /* hpi_channel_id_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Canonical sample frame. Payload layout is per-channel and matches the
 * .HP6 payload structs (one parser for file + stream). For batched
 * channels (ECG/PPG/EEG) `t_mono_us` is the first sample; the rest are
 * implicit at 1/sample_rate. */
struct hpi_sample_frame {
    hpi_channel_id_t channel;       /* see channel_registry.h */
    uint16_t         sample_rate;   /* Hz */
    uint16_t         sample_count;  /* samples in payload (1 for VITALS) */
    uint64_t         t_mono_us;     /* device-monotonic us of first sample */
    uint16_t         len;           /* payload bytes */
    uint16_t         flags;
    const void      *payload;       /* borrowed; valid only during publish */
};

/* Opaque per-consumer subscription handle. */
struct hpi_bus_sub;

/* Consumer registration. `ring_frames` sizes this consumer's ring; when
 * full, the oldest frame is dropped and the consumer's dropped counter is
 * incremented -- the producer is never blocked. */
struct hpi_bus_sub_cfg {
    const char       *name;          /* for diagnostics */
    uint32_t          channel_mask;  /* bitmask of hpi_channel_id_t to receive */
    uint16_t          ring_frames;   /* depth of this consumer's ring */
};

/* Initialize the bus. Call once at boot before any publish/subscribe. */
int hpi_bus_init(void);

/* Producer side: fan out a frame to every matching consumer. Lock-free,
 * never blocks. Returns the number of consumers the frame was delivered to
 * (0 if none subscribed). The frame's payload is copied into each
 * consumer's ring before return, so the caller may reuse its buffer. */
int hpi_bus_publish(const struct hpi_sample_frame *frame);

/* Consumer side. Returns NULL on allocation failure. */
struct hpi_bus_sub *hpi_bus_subscribe(const struct hpi_bus_sub_cfg *cfg);
void                hpi_bus_unsubscribe(struct hpi_bus_sub *sub);

/* Pull the next frame for this consumer. Non-blocking variant returns
 * -EAGAIN when empty; the _wait variant blocks up to timeout_ms.
 * On success, `out` and its payload are valid until the next pull. */
int hpi_bus_pull(struct hpi_bus_sub *sub, struct hpi_sample_frame *out);
int hpi_bus_pull_wait(struct hpi_bus_sub *sub, struct hpi_sample_frame *out,
                      uint32_t timeout_ms);

/* Per-consumer diagnostics. */
struct hpi_bus_sub_stats {
    uint32_t frames_delivered;
    uint32_t frames_dropped;   /* ring full at publish time */
    uint16_t ring_high_water;
};
void hpi_bus_sub_get_stats(const struct hpi_bus_sub *sub,
                           struct hpi_bus_sub_stats *out);

#ifdef __cplusplus
}
#endif

#endif /* HPI_CORE_SAMPLE_BUS_H */
