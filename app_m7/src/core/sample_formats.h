/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Canonical per-channel sample payloads carried on the sample bus.
 *
 * These are the SAME structs written into .HP6 data frames and emitted on the
 * live stream (one format, one parser -- see docs/HP6_DATA_FORMAT.md). A
 * producer fills one of these and points the bus frame's `payload` at it;
 * recording/stream/UI consume the identical layout. Little-endian, packed,
 * fixed size.
 */

#ifndef HPI_CORE_SAMPLE_FORMATS_H
#define HPI_CORE_SAMPLE_FORMATS_H

#include <stdint.h>
#include <zephyr/toolchain.h>   /* BUILD_ASSERT */

#ifdef __cplusplus
extern "C" {
#endif

/* Canonical per-sample rates (Hz). Shared so producers (acquisition) and
 * consumers (M4 feed, recording, stream) agree on one source of truth. */
#define HPI_ECG_RATE_HZ  500

/* PPG rate derives from the AFE4400 driver's Kconfig, which is what actually
 * programs the hardware (PRPCOUNT = 4000000/PRF - 1). CONFIG_AFE4400_PRF_HZ is
 * the single source of truth -- never restate a literal here, or the
 * advertised rate and the hardware drift apart. */
#if defined(CONFIG_AFE4400_PRF_HZ)
#define HPI_PPG_RATE_HZ  CONFIG_AFE4400_PRF_HZ
#else
#define HPI_PPG_RATE_HZ  250   /* AFE4400 driver not built (e.g. host tests) */
#endif

/* ECG electrode lead-off mask (hp6_ecg_sample.lead_off). Bit set = that
 * electrode is OFF the subject, per the ADS1294R lead-off comparators.
 *
 * The mask is in ELECTRODE terms, not channel terms: RA is the shared limb
 * reference on IN2N/IN3N, while LA/LL/V1 are the positive inputs of CH2/CH3/CH4
 * (the canonical map in drivers/sensor/ads129xx/ads129xx.c). Acquisition maps
 * the AFE's LOFF_STATP/LOFF_STATN bits onto this, so consumers never have to
 * know the channel layout. */
#define HP6_LEAD_OFF_RA   0x01
#define HP6_LEAD_OFF_LA   0x02
#define HP6_LEAD_OFF_LL   0x04
#define HP6_LEAD_OFF_V1   0x08

/* HPI_CH_ECG payload (20 B): ECG leads + respiration, microvolts.
 * Respiration shares the ECG frame (ADS1294R CH0). */
struct hp6_ecg_sample {
    int32_t  resp;       /* CH0, µV (moving-average filtered) */
    int32_t  lead_i;     /* CH1, µV */
    int32_t  lead_ii;    /* CH2, µV */
    int32_t  v1;         /* CH3, µV (Lead III / V1) */
    uint8_t  lead_off;   /* HP6_LEAD_OFF_* mask, debounced by acquisition */
    uint8_t  flags;
    uint16_t _pad;
} __packed;

/* HPI_CH_PPG payload (12 B): AFE4400 red/IR, raw 22-bit sign-extended. */
struct hp6_ppg_sample {
    int32_t red;         /* LED1 */
    int32_t ir;          /* LED2 */
    uint8_t lead_off;
    uint8_t _pad[3];
} __packed;

/* hp6_vitals.flags -- the provenance of hr_bpm. The rate can come from the
 * ECG QRS detector or the PPG pulse detector, and those are not the same
 * measurement. These bits travel with every vitals sample -- on the bus, in
 * the .HP6 recording and on the live stream -- so a consumer can always tell
 * which sensor produced the number. Never present a rate with
 * HP6_VIT_HR_FROM_PPG set as an ECG heart rate. */
#define HP6_VIT_HR_FROM_PPG    0x01  /* hr_bpm is a PPG pulse rate, not ECG    */
#define HP6_VIT_ECG_LEAD_OFF   0x02  /* >=1 ECG electrode off -> ECG HR gated  */
/* PPG perfusion low: treat BOTH spo2_x10 and a PPG-sourced hr_bpm as
 * provisional. Set whenever the M4 reports low perfusion or finger-off,
 * independently of which sensor won the HR arbitration. */
#define HP6_VIT_PPG_WEAK       0x04

/* HPI_CH_VITALS payload (16 B): derived metrics from the M4, ~1 Hz.
 *
 * The HRV fields were uint8 milliseconds in format 0x0200, clamped at 255. That
 * clips silently, and it clips exactly where the number matters: SDNN above
 * 255 ms occurs in healthy young adults at rest and routinely in atrial
 * fibrillation. A clamped 255 is indistinguishable from a real 255. Widened to
 * uint16 in 0x0300, along with somewhere for the frequency-domain result to go
 * -- the M4 has computed LF/HF all along and had no field to put it in. */
struct hp6_vitals {
    uint16_t hr_bpm;
    uint16_t spo2_x10;      /* SpO2 % x10                                     */
    uint16_t rr_bpm;        /* respiration rate (0 until wired)               */
    int16_t  temp_c_x100;   /* 0 until temp sensor wired                      */
    uint16_t hrv_sdnn_ms;   /* SDNN, ms                                       */
    uint16_t hrv_rmssd_ms;  /* RMSSD, ms                                      */
    uint16_t hrv_lf_hf_x10; /* LF/HF ratio x10; 0 = not computed              */
    uint8_t  flags;         /* HP6_VIT_* -- HR provenance + PPG quality       */
    uint8_t  _pad;
} __packed;

/* HPI_CH_EEG payload (36 B): HealthyLink EEG module (ADS1299, 8 ch, µV). */
struct hp6_eeg_sample {
    int32_t ch[8];        /* channels 1..8, microvolts */
    uint8_t lead_off;     /* bit N = channel N+1 electrode off */
    uint8_t _pad[3];
} __packed;

/* HealthyLink EEG sample rate (Hz). */
#define HPI_EEG_RATE_HZ  250

/* hp6_infer_sample.flags */
#define HP6_INF_STUB       0x01  /* NOT a real inference -- see below         */
#define HP6_INF_LOW_CONF   0x02  /* below the model's usable confidence       */
#define HP6_INF_ECG_SUSPECT 0x04 /* input beat came from a poor-quality trace */

/* AAMI beat classes, in the order the network emits its scores. */
enum hp6_infer_class {
    HP6_INF_CLASS_N = 0,   /* normal / bundle-branch block                    */
    HP6_INF_CLASS_S = 1,   /* supraventricular ectopic                        */
    HP6_INF_CLASS_V = 2,   /* ventricular ectopic                             */
    HP6_INF_CLASS_F = 3,   /* fusion of ventricular and normal                */
    HP6_INF_CLASS_Q = 4,   /* unclassifiable / paced                          */
};

/* HPI_CH_INFER payload (16 B): one classified beat from a HealthyLink compute
 * module. Event-rate, not sampled -- published per beat, like HPI_CH_EVENT, so
 * `sample_rate` on the frame is 0.
 *
 * HP6_INF_STUB is the most important bit here. The compute module's
 * RUN_INFERENCE returns five zero bytes today; without a bit that says "this
 * did not come from a real inference", a recording made during bring-up is
 * indistinguishable from a clinical one after the fact. A producer that cannot
 * prove it ran a network MUST set it. */
struct hp6_infer_sample {
    /* Device uptime in ms, NOT session-relative -- unlike hp6_event.ts_ms,
     * which recording rebases onto the session start. A producer of this
     * channel lives in healthylink/, which may not reach services/ and so
     * cannot see the session epoch. Ordering against surrounding frames is
     * what this field is for; use the enclosing block's timestamp for wall
     * clock. Do not silently "fix" one of the two to match the other. */
    uint32_t ts_ms;
    uint16_t model_id;     /* which network produced this (0 = unknown)       */
    uint8_t  class_id;     /* argmax; enum hp6_infer_class                    */
    uint8_t  confidence;   /* 0..255, the winning score rescaled              */
    int8_t   scores[5];    /* raw per-class scores, as the network returned   */
    uint8_t  flags;        /* HP6_INF_*                                       */
    uint8_t  _pad[2];
} __packed;

/* HPI_CH_EVENT payload (8 B): a point-in-time marker, not a sampled signal.
 * Published as a one-sample frame (today only a user mark). It rides the bus
 * like any other channel, so it lands in the .HP6 recording, live stream and
 * ESP32 link in-band, ordered against the data it sits between -- and so it
 * survives an interrupted recording, when the .IDX sidecar (written at close)
 * is least trustworthy. */
struct hp6_event {
    uint32_t ts_ms;   /* ms since the session started */
    uint16_t type;    /* enum hp6_event_type */
    uint16_t seq;     /* 1-based, per session, so a gap means a dropped frame */
} __packed;

enum hp6_event_type {
    HP6_EVENT_USER_MARK = 1,   /* operator marked this instant */
};

/* The payload sizes are the wire format: they are what the .HP6 file, the live
 * stream and every host parser agree on, and docs/HP6_DATA_FORMAT.md publishes
 * them. Pin them here so a field added or a type widened cannot silently change
 * the format -- the build fails instead, next to the struct that moved. */
BUILD_ASSERT(sizeof(struct hp6_ecg_sample)   == 20, "ECG payload must be 20 B");
BUILD_ASSERT(sizeof(struct hp6_ppg_sample)   == 12, "PPG payload must be 12 B");
BUILD_ASSERT(sizeof(struct hp6_vitals)       == 16, "VITALS payload must be 16 B");
BUILD_ASSERT(sizeof(struct hp6_eeg_sample)   == 36, "EEG payload must be 36 B");
BUILD_ASSERT(sizeof(struct hp6_event)        == 8,  "EVENT payload must be 8 B");
BUILD_ASSERT(sizeof(struct hp6_infer_sample) == 16, "INFER payload must be 16 B");

#ifdef __cplusplus
}
#endif

#endif /* HPI_CORE_SAMPLE_FORMATS_H */
