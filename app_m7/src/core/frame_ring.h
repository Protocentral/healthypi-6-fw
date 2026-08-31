/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * frame_ring -- the pure-C core of each sample-bus consumer.
 *
 * A fixed-capacity FIFO of sample frames with inline payload. Deliberately
 * dependency-free (stdint/string only); exercised by core/bus_selftest.c at
 * boot under CONFIG_HPI_BUS_SELFTEST=y. Concurrency is the caller's
 * responsibility -- sample_bus.c serialises push/pop under a spinlock.
 * Policy: drop-on-full overwrites the OLDEST frame (keeps the live tail)
 * and increments a dropped counter.
 */

#ifndef HPI_CORE_FRAME_RING_H
#define HPI_CORE_FRAME_RING_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "channel_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Max payload bytes carried inline per frame. Sized for the largest batched
 * frame: EEG 8ch x 16 samples x 36 B = 576 B, rounded up. */
#define HPI_FRAME_MAX_PAYLOAD 640

/* A frame as stored in a ring (payload copied inline, not borrowed). */
struct hpi_frame_stored {
    hpi_channel_id_t channel;
    uint16_t         sample_rate;
    uint16_t         sample_count;
    uint64_t         t_mono_us;
    uint16_t         len;            /* valid bytes in data[] */
    uint16_t         flags;
    uint8_t          data[HPI_FRAME_MAX_PAYLOAD];
};

struct hpi_frame_ring {
    struct hpi_frame_stored *slots;  /* caller-provided, >= capacity entries */
    uint16_t capacity;
    uint16_t head;                   /* next write index */
    uint16_t tail;                   /* next read index */
    uint16_t count;                  /* frames currently queued */
    uint16_t high_water;             /* max count ever observed */
    uint32_t dropped;                /* frames discarded due to overflow */
};

/* Initialise a ring over caller-provided slot storage. */
static inline void hpi_fr_init(struct hpi_frame_ring *r,
                               struct hpi_frame_stored *slots,
                               uint16_t capacity)
{
    r->slots = slots;
    r->capacity = capacity;
    r->head = r->tail = r->count = r->high_water = 0;
    r->dropped = 0;
}

/* Push a frame (copied). On full, the oldest frame is discarded and `dropped`
 * is incremented. Returns true if a frame was dropped to make room. */
static inline bool hpi_fr_push(struct hpi_frame_ring *r,
                               const struct hpi_frame_stored *f)
{
    bool dropped = false;

    if (r->capacity == 0U) {
        r->dropped++;
        return true;
    }
    if (r->count == r->capacity) {
        /* discard oldest */
        r->tail = (uint16_t)((r->tail + 1U) % r->capacity);
        r->count--;
        r->dropped++;
        dropped = true;
    }
    r->slots[r->head] = *f;
    r->head = (uint16_t)((r->head + 1U) % r->capacity);
    r->count++;
    if (r->count > r->high_water) {
        r->high_water = r->count;
    }
    return dropped;
}

/* Pop the oldest frame into `out`. Returns true if a frame was returned,
 * false if the ring was empty. */
static inline bool hpi_fr_pop(struct hpi_frame_ring *r,
                              struct hpi_frame_stored *out)
{
    if (r->count == 0U) {
        return false;
    }
    *out = r->slots[r->tail];
    r->tail = (uint16_t)((r->tail + 1U) % r->capacity);
    r->count--;
    return true;
}

static inline bool hpi_fr_empty(const struct hpi_frame_ring *r)
{
    return r->count == 0U;
}

#ifdef __cplusplus
}
#endif

#endif /* HPI_CORE_FRAME_RING_H */
