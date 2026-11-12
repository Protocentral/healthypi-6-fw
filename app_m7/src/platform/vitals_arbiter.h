/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Vitals arbitration (platform, pure logic). Turns the latest M4 vitals inputs
 * plus "now" into the M4-derived fields of an hp6_vitals sample: which sensor's
 * heart rate wins, and which values have aged out.
 *
 * No kernel calls, no clocks, no globals: every function is a pure function of
 * its arguments, so platform/ipc.c owns the timing and locking and the host
 * test (app_m7/tests/host/test_vitals_arbiter.c) can drive the rules with
 * synthetic timestamps.
 */

#ifndef HPI_PLATFORM_VITALS_ARBITER_H
#define HPI_PLATFORM_VITALS_ARBITER_H

#include <stdbool.h>
#include <stdint.h>

#include "core/sample_formats.h"

#ifdef __cplusplus
extern "C" {
#endif

/* An M4 input older than this no longer counts. Applies to the ECG rate (and
 * the HRV that comes with it) and to everything the PPG message carried: its
 * pulse rate, SpO2 and the weak-perfusion qualifier. */
#define HPI_VIT_STALE_MS 5000

/* hpi_vit_next_expiry(): nothing is fresh, so nothing will age out. */
#define HPI_VIT_NO_EXPIRY INT64_MAX

/* The latest value of each M4 input and when it arrived (M7 uptime, ms).
 * A zero value means "not available", as in hp6_vitals. */
struct hpi_vit_inputs {
    uint16_t ecg_hr;
    uint16_t hrv_sdnn_ms;
    uint16_t hrv_rmssd_ms;
    uint16_t hrv_lf_hf_x10;
    int64_t  ecg_at;

    uint16_t ppg_hr;
    uint16_t spo2_x10;
    bool     ppg_weak;    /* M4 says low perfusion / finger off */
    int64_t  ppg_at;
};

/* Fill the M4-derived fields of *out -- hr_bpm, spo2_x10, the three HRV fields
 * and the HP6_VIT_HR_FROM_PPG / HP6_VIT_ECG_LEAD_OFF / HP6_VIT_PPG_WEAK flag
 * bits -- as they stand at `now`. `leads_off` is the caller's judgement that
 * an ECG electrode is currently off. The M7-side fields (temp_c_x100, rr_bpm,
 * HP6_VIT_MOTION) are left as they were for the caller to fill. */
void hpi_vit_arbitrate(const struct hpi_vit_inputs *in, int64_t now,
                       bool leads_off, struct hp6_vitals *out);

/* The first instant after `now` at which an input that is fresh at `now` ages
 * out, or HPI_VIT_NO_EXPIRY if nothing is fresh. Always > now otherwise. */
int64_t hpi_vit_next_expiry(const struct hpi_vit_inputs *in, int64_t now);

/* True if a and b differ in anything hpi_vit_arbitrate() sets. */
bool hpi_vit_arbitrated_differ(const struct hp6_vitals *a,
                               const struct hp6_vitals *b);

#ifdef __cplusplus
}
#endif

#endif /* HPI_PLATFORM_VITALS_ARBITER_H */
