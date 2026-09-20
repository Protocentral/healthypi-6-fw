/*
 * Copyright (c) 2026 ProtoCentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Thoracic-Z respiration rate from hp6_ecg_sample.resp (ADS1294R CH1).
 * Same getter pattern as hpi_temp_c_x100(): platform/ipc copies this into
 * hp6_vitals.rr_bpm. 0 means unavailable — leads off, not yet locked, or
 * stale — not a measured zero. Not a clinical RR.
 */

#ifndef HPI_CORE_RESP_RATE_H
#define HPI_CORE_RESP_RATE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int hpi_resp_rate_init(void);

/* Latest breaths/min, or 0 if unavailable. */
uint16_t hpi_resp_rate_bpm(void);

#ifdef __cplusplus
}
#endif

#endif /* HPI_CORE_RESP_RATE_H */
