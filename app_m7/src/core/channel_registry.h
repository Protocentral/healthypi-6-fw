/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Canonical channel registry. Channel ids match the .HP6 channel-type
 * enum (RECORDING_MODULE_SRS.md section 4.1) so the recorder, the stream encoder,
 * and the host parser all share one definition -- no translation layer.
 */

#ifndef HPI_CORE_CHANNEL_REGISTRY_H
#define HPI_CORE_CHANNEL_REGISTRY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Channel ids -- keep in sync with .HP6 recording_packet_type. */
typedef enum {
    HPI_CH_ECG     = 1,
    HPI_CH_PPG     = 2,
    HPI_CH_RESP    = 3,
    HPI_CH_VITALS  = 4,   /* HR/SpO2/RR/temp/HRV from M4, 1 Hz */
    HPI_CH_EEG     = 5,   /* HealthyLink EEG module */
    HPI_CH_EVENT   = 6,
    HPI_CH_SYNC    = 7,
    HPI_CH_INFER   = 8,   /* HealthyLink compute module: classified beats */
} hpi_channel_id_t;

/* Subscription mask helpers (bit = 1 << channel_id). */
#define HPI_CH_BIT(id)   (1u << (id))
#define HPI_CH_MASK_ALL  0xFFFFFFFFu

#ifdef __cplusplus
}
#endif

#endif /* HPI_CORE_CHANNEL_REGISTRY_H */
