/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Onboard sensor acquisition (L2 producer). Reads ECG/respiration (ADS1294R)
 * and PPG (AFE4400) via the Zephyr sensor + RTIO trigger path and publishes
 * canonical frames to the sample bus. Depends only on drivers + the bus --
 * never on services/UI (the dependency rule).
 */

#ifndef HPI_CORE_ACQUISITION_H
#define HPI_CORE_ACQUISITION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Start onboard acquisition (installs DRDY data-ready triggers). Safe to call
 * after hpi_bus_init(). Returns 0, or a negative errno if a sensor could not
 * be armed (the others still run -- a missing sensor is non-fatal). */
int hpi_acquisition_init(void);

/* The current debounced ECG lead-off mask (HP6_LEAD_OFF_* in
 * core/sample_formats.h); 0 = every electrode connected. The same value rides
 * on every ECG sample; this accessor is for consumers that never see the ECG
 * stream -- the vitals producer (which must not report an ECG heart rate from
 * a floating electrode) and the group-64 diag adapter.
 *
 * `age_ms` (may be NULL) returns how long ago the last ECG sample arrived, so
 * a caller can tell "leads on" from "acquisition stopped talking" -- a stale 0
 * is not good news. Reads UINT32_MAX-ish until the first sample. */
uint8_t hpi_acquisition_lead_off(uint32_t *age_ms);

#ifdef __cplusplus
}
#endif

#endif /* HPI_CORE_ACQUISITION_H */
