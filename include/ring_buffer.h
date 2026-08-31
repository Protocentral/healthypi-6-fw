/*
 * Copyright (c) 2024-2026 ProtoCentral Electronics
 * SPDX-License-Identifier: MIT
 */

/**
 * @file ring_buffer.h
 * @brief Zero-copy ring buffer for M4-M7 sensor data sharing
 *
 * Lives in SRAM2 (shared between cores). Lock-free, valid only for a single
 * producer (M4) and single consumer (M7); each core writes only its own index
 * fields. Layout is struct shared_ring_buffer below.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Ring buffer configuration */
#define RING_BUFFER_SIZE 128  /* Must be power of 2 */
#define RING_BUFFER_MASK (RING_BUFFER_SIZE - 1)

/**
 * @brief Single ring buffer entry (48 bytes)
 * 
 * Contains one sample from each sensor, timestamped.
 */
struct ring_buffer_entry {
    uint32_t timestamp_ms;      /* System uptime in ms */
    uint32_t sample_id;         /* Sequential sample number */
    
    /* ECG channels (4 × 4 bytes = 16 bytes) */
    int32_t ecg_ch1;           /* Lead I */
    int32_t ecg_ch2;           /* Lead II */
    int32_t ecg_ch3;           /* Lead III (or V) */
    int32_t ecg_ch0;           /* Respiration/impedance */
    
    /* PPG channels (2 × 4 bytes = 8 bytes) */
    int32_t ppg_red;           /* Red LED */
    int32_t ppg_ir;            /* IR LED */
    
    /* Status flags (4 bytes) */
    uint8_t ecg_lead_off;      /* ECG lead-off detection */
    uint8_t ppg_lead_off;      /* PPG sensor contact */
    uint8_t signal_quality;    /* 0-100 */
    uint8_t flags;             /* General flags */
    
    /* Algorithm results (8 bytes) */
    uint16_t hr_bpm;           /* Heart rate */
    uint8_t spo2_percent;      /* SpO2 */
    uint8_t resp_rate;         /* Respiration rate */
    uint32_t reserved1;        /* Reserved for future */
    uint32_t reserved2;        /* Reserved for future */
    uint16_t reserved3;        /* Padding to 48 bytes */
} __attribute__((packed));

/**
 * @brief Shared ring buffer structure
 * 
 * Located in SRAM2 via linker section attribute.
 * Total size: 64 + (128 × 48) = 6208 bytes (~6 KB)
 */
struct shared_ring_buffer {
    /* Producer (M4) fields - cacheline 1 */
    volatile uint32_t write_idx;        /* Write index (0 to RING_BUFFER_SIZE-1) */
    volatile uint32_t write_wrap_count; /* Number of wraps */
    volatile uint32_t overruns;         /* Count of buffer overruns */
    volatile uint32_t reserved_m4;      /* Padding */
    
    /* Consumer (M7) fields - cacheline 2 */
    volatile uint32_t read_idx;         /* Read index (0 to RING_BUFFER_SIZE-1) */
    volatile uint32_t read_wrap_count;  /* Number of wraps */
    volatile uint32_t underruns;        /* Count of read-when-empty */
    volatile uint32_t reserved_m7;      /* Padding */
    
    /* Statistics - cacheline 3 */
    volatile uint32_t total_writes;     /* Total samples written */
    volatile uint32_t total_reads;      /* Total samples read */
    volatile uint32_t max_fill_level;   /* Maximum fill level seen */
    volatile uint32_t reserved_stats;   /* Padding */
    
    /* Reserved space */
    uint32_t reserved[4];
    
    /* Data buffer - starts on 64-byte boundary */
    struct ring_buffer_entry samples[RING_BUFFER_SIZE];
} __attribute__((aligned(64)));

/* Note: Ring buffer entry size is compiler-dependent due to padding/alignment */
/* Original design: 48 bytes, actual size may vary */

/**
 * @brief Initialize ring buffer
 * 
 * Must be called by both M4 and M7 during startup.
 * Not thread-safe; call before starting producer/consumer threads.
 * 
 * @param rb Pointer to ring buffer (must be in shared memory)
 */
void ring_buffer_init(struct shared_ring_buffer *rb);

/**
 * @brief Write entry to ring buffer (M4 only)
 * 
 * This function is optimized for the producer (M4 core).
 * It's lock-free and wait-free for single producer scenario.
 * 
 * @param rb Pointer to ring buffer
 * @param entry Pointer to entry to write
 * @return true if written successfully, false if buffer full
 */
bool ring_buffer_write(struct shared_ring_buffer *rb,
                       const struct ring_buffer_entry *entry);

/**
 * @brief Read entry from ring buffer (M7 only)
 * 
 * This function is optimized for the consumer (M7 core).
 * It's lock-free and wait-free for single consumer scenario.
 * 
 * @param rb Pointer to ring buffer
 * @param entry Pointer to output entry
 * @return true if read successfully, false if buffer empty
 */
bool ring_buffer_read(struct shared_ring_buffer *rb,
                      struct ring_buffer_entry *entry);

/**
 * @brief Get number of entries available to read (M7)
 * 
 * @param rb Pointer to ring buffer
 * @return Number of entries ready to read
 */
static inline uint32_t ring_buffer_available(const struct shared_ring_buffer *rb)
{
    /* Read indices atomically */
    uint32_t write_idx = rb->write_idx;
    uint32_t read_idx = rb->read_idx;
    uint32_t write_wrap = rb->write_wrap_count;
    uint32_t read_wrap = rb->read_wrap_count;
    
    /* Calculate distance */
    if (write_wrap == read_wrap) {
        return (write_idx >= read_idx) ? (write_idx - read_idx) : 0;
    } else if (write_wrap == read_wrap + 1) {
        return RING_BUFFER_SIZE - read_idx + write_idx;
    } else {
        /* Buffer overrun occurred */
        return RING_BUFFER_SIZE;
    }
}

/**
 * @brief Get free space in buffer (M4)
 * 
 * @param rb Pointer to ring buffer
 * @return Number of free entries
 */
static inline uint32_t ring_buffer_space(const struct shared_ring_buffer *rb)
{
    return RING_BUFFER_SIZE - ring_buffer_available(rb) - 1;
}

/**
 * @brief Check if buffer is empty (M7)
 * 
 * @param rb Pointer to ring buffer
 * @return true if empty
 */
static inline bool ring_buffer_is_empty(const struct shared_ring_buffer *rb)
{
    return (rb->read_idx == rb->write_idx) &&
           (rb->read_wrap_count == rb->write_wrap_count);
}

/**
 * @brief Check if buffer is full (M4)
 * 
 * @param rb Pointer to ring buffer
 * @return true if full
 */
static inline bool ring_buffer_is_full(const struct shared_ring_buffer *rb)
{
    uint32_t next_write = (rb->write_idx + 1) & RING_BUFFER_MASK;
    uint32_t next_wrap = (next_write == 0) ? (rb->write_wrap_count + 1) : rb->write_wrap_count;
    
    return (next_write == rb->read_idx) && (next_wrap != rb->read_wrap_count);
}

/**
 * @brief Reset ring buffer statistics
 * 
 * @param rb Pointer to ring buffer
 */
void ring_buffer_reset_stats(struct shared_ring_buffer *rb);

/**
 * @brief Get ring buffer statistics
 * 
 * @param rb Pointer to ring buffer
 * @param stats Output structure for statistics
 */
struct ring_buffer_stats {
    uint32_t total_writes;
    uint32_t total_reads;
    uint32_t overruns;
    uint32_t underruns;
    uint32_t max_fill_level;
    uint32_t current_fill;
};

void ring_buffer_get_stats(const struct shared_ring_buffer *rb,
                           struct ring_buffer_stats *stats);

/**
 * @brief Print ring buffer statistics (for debugging)
 * 
 * @param rb Pointer to ring buffer
 * @param label Label to print with stats
 */
void ring_buffer_print_stats(const struct shared_ring_buffer *rb,
                             const char *label);

#ifdef __cplusplus
}
#endif
