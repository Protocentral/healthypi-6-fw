/*
 * Copyright (c) 2026 ProtoCentral Electronics
 * SPDX-License-Identifier: MIT
 */

/**
 * @file ring_buffer.c
 * @brief Implementation of zero-copy ring buffer for M4-M7 data sharing
 */

#include "ring_buffer.h"
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(ring_buffer, LOG_LEVEL_DBG);

/**
 * @brief Global ring buffer instance in SRAM2
 * 
 * This is placed in shared memory accessible by both cores.
 * The linker script must define the .sram2 section.
 */
struct shared_ring_buffer g_sensor_ring_buffer __attribute__((section(".sram2")));

void ring_buffer_init(struct shared_ring_buffer *rb)
{
    if (rb == NULL) {
        LOG_ERR("ring_buffer_init: NULL pointer");
        return;
    }
    
    /* Clear all fields */
    memset((void *)rb, 0, sizeof(*rb));
    
    LOG_INF("Ring buffer initialized at %p, size=%zu bytes",
            rb, sizeof(*rb));
    LOG_INF("Entry size: %zu bytes, capacity: %u entries",
            sizeof(struct ring_buffer_entry), RING_BUFFER_SIZE);
}

bool ring_buffer_write(struct shared_ring_buffer *rb,
                       const struct ring_buffer_entry *entry)
{
    if (rb == NULL || entry == NULL) {
        return false;
    }
    
    /* Check if buffer is full */
    uint32_t next_write = (rb->write_idx + 1) & RING_BUFFER_MASK;
    uint32_t next_wrap = (next_write == 0) ? (rb->write_wrap_count + 1) : rb->write_wrap_count;
    
    if ((next_write == rb->read_idx) && (next_wrap != rb->read_wrap_count)) {
        /* Buffer full - overrun */
        __atomic_fetch_add(&rb->overruns, 1, __ATOMIC_RELAXED);
        return false;
    }
    
    /* Write entry */
    memcpy((void *)&rb->samples[rb->write_idx], entry, sizeof(*entry));
    
    /* Memory barrier to ensure data is written before index update */
    __sync_synchronize();
    
    /* Update write index */
    rb->write_idx = next_write;
    if (next_write == 0) {
        rb->write_wrap_count = next_wrap;
    }
    
    /* Update statistics */
    __atomic_fetch_add(&rb->total_writes, 1, __ATOMIC_RELAXED);
    
    /* Track max fill level */
    uint32_t fill = ring_buffer_available(rb);
    if (fill > rb->max_fill_level) {
        rb->max_fill_level = fill;
    }
    
    return true;
}

bool ring_buffer_read(struct shared_ring_buffer *rb,
                      struct ring_buffer_entry *entry)
{
    if (rb == NULL || entry == NULL) {
        return false;
    }
    
    /* Check if buffer is empty */
    if ((rb->read_idx == rb->write_idx) &&
        (rb->read_wrap_count == rb->write_wrap_count)) {
        /* Buffer empty - underrun */
        __atomic_fetch_add(&rb->underruns, 1, __ATOMIC_RELAXED);
        return false;
    }
    
    /* Read entry */
    memcpy(entry, (void *)&rb->samples[rb->read_idx], sizeof(*entry));
    
    /* Memory barrier to ensure data is read before index update */
    __sync_synchronize();
    
    /* Update read index */
    uint32_t next_read = (rb->read_idx + 1) & RING_BUFFER_MASK;
    rb->read_idx = next_read;
    if (next_read == 0) {
        rb->read_wrap_count++;
    }
    
    /* Update statistics */
    __atomic_fetch_add(&rb->total_reads, 1, __ATOMIC_RELAXED);
    
    return true;
}

void ring_buffer_reset_stats(struct shared_ring_buffer *rb)
{
    if (rb == NULL) {
        return;
    }
    
    rb->overruns = 0;
    rb->underruns = 0;
    rb->total_writes = 0;
    rb->total_reads = 0;
    rb->max_fill_level = 0;
}

void ring_buffer_get_stats(const struct shared_ring_buffer *rb,
                           struct ring_buffer_stats *stats)
{
    if (rb == NULL || stats == NULL) {
        return;
    }
    
    stats->total_writes = rb->total_writes;
    stats->total_reads = rb->total_reads;
    stats->overruns = rb->overruns;
    stats->underruns = rb->underruns;
    stats->max_fill_level = rb->max_fill_level;
    stats->current_fill = ring_buffer_available(rb);
}

void ring_buffer_print_stats(const struct shared_ring_buffer *rb,
                             const char *label)
{
    if (rb == NULL) {
        return;
    }
    
    struct ring_buffer_stats stats;
    ring_buffer_get_stats(rb, &stats);
    
    LOG_INF("=== Ring Buffer Stats: %s ===", label ? label : "");
    LOG_INF("Total writes:   %u", stats.total_writes);
    LOG_INF("Total reads:    %u", stats.total_reads);
    LOG_INF("Overruns:       %u", stats.overruns);
    LOG_INF("Underruns:      %u", stats.underruns);
    LOG_INF("Max fill level: %u / %u (%.1f%%)",
            stats.max_fill_level, RING_BUFFER_SIZE,
            (float)stats.max_fill_level / RING_BUFFER_SIZE * 100.0f);
    LOG_INF("Current fill:   %u / %u (%.1f%%)",
            stats.current_fill, RING_BUFFER_SIZE,
            (float)stats.current_fill / RING_BUFFER_SIZE * 100.0f);
    
    if (stats.total_writes > 0) {
        float loss_rate = (float)stats.overruns / stats.total_writes * 100.0f;
        LOG_INF("Loss rate:      %.3f%%", loss_rate);
    }
    
    LOG_INF("================================");
}

/**
 * @brief Periodic statistics thread (optional debugging)
 */
#ifdef CONFIG_RING_BUFFER_STATS_THREAD

static void ring_buffer_stats_thread(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);
    
    LOG_INF("Ring buffer stats thread started");
    
    while (1) {
        k_sleep(K_SECONDS(10));
        ring_buffer_print_stats(&g_sensor_ring_buffer, "Periodic");
    }
}

K_THREAD_DEFINE(ring_buffer_stats_tid, 1024,
                ring_buffer_stats_thread, NULL, NULL, NULL,
                7, 0, 0);

#endif  /* CONFIG_RING_BUFFER_STATS_THREAD */
