/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Heart-rate source arbitration and vitals expiry. See vitals_arbiter.h.
 *
 * Two detectors can produce a heart rate -- the M4's ECG QRS detector and its
 * PPG pulse detector -- and they are NOT interchangeable. This decides which
 * one hp6_vitals.hr_bpm carries and records that choice in hp6_vitals.flags,
 * so every consumer knows the number's provenance.
 *
 * Rules:
 *  1. An ECG HR is only credible while the leads are ON -- a disconnected
 *     electrode rails and the QRS detector finds "beats" in the noise.
 *  2. Every M4 input EXPIRES (HPI_VIT_STALE_MS); a stale value never holds a
 *     field. HRV dies with the ECG rate; SpO2 and PPG_WEAK die with the PPG
 *     message that carried them.
 *  3. The PPG fallback is EXPLICIT (HP6_VIT_HR_FROM_PPG) -- a PPG pulse rate
 *     must never be rendered under an ECG label.
 *
 * Staleness is a function of time, so the output is recomputed from the
 * inputs and "now" every time rather than mutated in place by whichever
 * message happened to arrive last.
 */

#include "vitals_arbiter.h"

#define ARBITRATED_FLAGS (HP6_VIT_HR_FROM_PPG | HP6_VIT_ECG_LEAD_OFF | \
                          HP6_VIT_PPG_WEAK)

static bool fresh(int64_t at, int64_t now)
{
    return (now - at) <= HPI_VIT_STALE_MS;
}

/* Anything in the PPG message that a consumer would see. */
static bool ppg_has_data(const struct hpi_vit_inputs *in)
{
    return in->ppg_hr != 0U || in->spo2_x10 != 0U || in->ppg_weak;
}

void hpi_vit_arbitrate(const struct hpi_vit_inputs *in, int64_t now,
                       bool leads_off, struct hp6_vitals *out)
{
    bool ecg_ok = !leads_off && in->ecg_hr != 0U && fresh(in->ecg_at, now);
    bool ppg_fresh = fresh(in->ppg_at, now);
    bool ppg_ok = ppg_fresh && in->ppg_hr != 0U;
    uint8_t flags = leads_off ? HP6_VIT_ECG_LEAD_OFF : 0U;

    if (ecg_ok) {
        out->hr_bpm = in->ecg_hr;
    } else if (ppg_ok) {
        /* Reported even when the M4 calls perfusion low -- with PPG_WEAK set,
         * so a consumer can present it as provisional. */
        out->hr_bpm = in->ppg_hr;
        flags |= HP6_VIT_HR_FROM_PPG;
    } else {
        out->hr_bpm = 0U;   /* 0 = not available (never "measured zero") */
    }

    /* PPG_WEAK qualifies EVERYTHING the PPG produced, not just a PPG-sourced
     * rate: SpO2 is the value most likely to be read off a poorly perfused
     * finger and the one a reader is least able to sanity-check. It goes when
     * the PPG data it qualifies goes. */
    if (ppg_fresh && in->ppg_weak) {
        flags |= HP6_VIT_PPG_WEAK;
    }
    out->spo2_x10 = ppg_fresh ? in->spo2_x10 : 0U;

    /* HRV comes from the ECG beat series, so it dies with the ECG HR. */
    out->hrv_sdnn_ms   = ecg_ok ? in->hrv_sdnn_ms   : 0U;
    out->hrv_rmssd_ms  = ecg_ok ? in->hrv_rmssd_ms  : 0U;
    out->hrv_lf_hf_x10 = ecg_ok ? in->hrv_lf_hf_x10 : 0U;

    out->flags = (uint8_t)((out->flags & ~ARBITRATED_FLAGS) | flags);
}

int64_t hpi_vit_next_expiry(const struct hpi_vit_inputs *in, int64_t now)
{
    int64_t next = HPI_VIT_NO_EXPIRY;

    /* An input stops counting at the first instant (now - at) > STALE. */
    if (in->ecg_hr != 0U && fresh(in->ecg_at, now)) {
        next = in->ecg_at + HPI_VIT_STALE_MS + 1;
    }
    if (ppg_has_data(in) && fresh(in->ppg_at, now)) {
        int64_t t = in->ppg_at + HPI_VIT_STALE_MS + 1;

        if (t < next) {
            next = t;
        }
    }
    return next;
}

bool hpi_vit_arbitrated_differ(const struct hp6_vitals *a,
                               const struct hp6_vitals *b)
{
    return a->hr_bpm != b->hr_bpm ||
           a->spo2_x10 != b->spo2_x10 ||
           a->hrv_sdnn_ms != b->hrv_sdnn_ms ||
           a->hrv_rmssd_ms != b->hrv_rmssd_ms ||
           a->hrv_lf_hf_x10 != b->hrv_lf_hf_x10 ||
           (a->flags & ARBITRATED_FLAGS) != (b->flags & ARBITRATED_FLAGS);
}
