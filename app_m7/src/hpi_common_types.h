/*
 * Copyright (c) 2024-2026 ProtoCentral Electronics
 *
 * SPDX-License-Identifier: MIT
 *
 * HealthyPi specific common data types. This is the header the unchanged M4
 * includes, so it is the shared M7<->M4 contract. Keep it in sync with
 * app_m4/src/ipc_module.h, which mirrors the message-type enum.
 */

#pragma once

#define PPG_POINTS_PER_SAMPLE 8

#define ECG_POINTS_PER_SAMPLE   8
#define BIOZ_POINTS_PER_SAMPLE  4

struct hpi_hr_trend_point_t
{
    uint16_t hr;
    uint32_t timestamp;
};

struct hpi_ecg_bioz_sensor_data_t
{
    int32_t stat;
    int32_t data_ch0;
    int32_t data_ch1;
    int32_t data_ch2;
    int32_t data_ch3;

    uint16_t hr;
    uint8_t rr;
    
    uint8_t ecg_lead_off;
    uint8_t bioz_lead_off;

};

struct hpi_ppg_sensor_data_t
{
    int32_t ppg_red_sample;
    int32_t ppg_ir_sample;
    uint8_t ppg_lead_off;
    
    uint8_t spo2;
    uint16_t hr;
};

struct hpi_computed_hrv_t
{
    int32_t hrv_max;
    int32_t hrv_min;
    float mean;
    float sdnn;
    float pnn;
    float rmssd;
    bool hrv_ready_flag;
};

struct hpi_hr_t
{
    uint16_t hr;
};

struct hpi_steps_t
{
    uint32_t steps_run;
    uint32_t steps_walk;
};

struct hpi_temp_t
{
    double temp_f;
    double temp_c;
};

struct hpi_spo2_t
{
    uint8_t spo2;
};

struct hpi_resp_rate_t
{
    uint16_t resp_rate;
};

struct hpi_batt_status_t
{
    uint8_t batt_level;
    bool batt_charging;
};

struct hpi_version_desc_t
{
    uint8_t major;
    uint8_t minor;
    uint8_t patch;
};

/* ============================================================================
 * IPC ACKNOWLEDGMENT STRUCTURES
 * ============================================================================ */

/* Sensor type enumeration for acknowledgments */
enum hpi_ipc_sensor_type {
    HPI_IPC_SENSOR_PPG = 0,
    HPI_IPC_SENSOR_ECG = 1,
    HPI_IPC_SENSOR_EEG = 2,
};

/* Generic acknowledgment structure from M4 to M7 */
struct hpi_ipc_ack_data {
    uint8_t sensor_type;        // hpi_ipc_sensor_type
    uint8_t reserved[3];        // Alignment
    uint32_t batches_received;  // Total batches received by M4
    uint32_t samples_received;  // Total samples received by M4
    uint32_t sequence_errors;   // Number of sequence gaps detected
} __packed;

/* ============================================================================
 * IPC MESSAGE STRUCTURES FOR PPG/SpO2 ALGORITHM
 * ============================================================================ */

/* The rate at which the M7 feeds ECG to the M4, in Hz.
 *
 * Unlike PPG this is NOT decimated -- forward_ecg() passes the acquisition rate
 * straight through -- so it equals the bus rate. It is stated here anyway
 * because the M4 needs it to convert QRS sample indices into RR intervals, and
 * an RR interval derived from the wrong rate is a wrong heart rate and wrong
 * HRV, with nothing to show that anything went wrong. */
#define HPI_ECG_M4_RATE_HZ 500

/* The rate at which the M7 feeds PPG to the M4, in Hz.
 *
 * THIS IS THE ONE PLACE IT IS DEFINED. The M7 decimates the full-rate PPG bus
 * (HPI_PPG_RATE_HZ, the AFE4400 PRF) down to this before sending
 * (app_m7/src/platform/ipc.c, forward_ppg); the M4's SpO2 algorithm sizes every
 * one of its windows against it (app_m4/src/spo2_module.h). Both sides now
 * derive from this symbol and BUILD_ASSERT against it, because they did not
 * used to: the M4 believed it was handed 500 Hz and decimated a second time by
 * 4, so the algorithm ran at 31.25 Hz against 125 Hz constants and never
 * produced a reading. Two numbers, maintained apart, that had to agree.
 *
 * Changing this is a change to an algorithm's sample rate, not a tuning knob:
 * every filter cutoff and peak-spacing window on the M4 assumes it. */
#define HPI_PPG_M4_RATE_HZ 125

/* PPG Raw Sample - M7 to M4 (sampled at HPI_PPG_M4_RATE_HZ) */
struct hpi_ipc_ppg_raw_sample {
    uint32_t timestamp_ms;      // Timestamp in milliseconds
    int32_t red_raw;            // Red LED photodiode value (22-bit sign-extended)
    int32_t ir_raw;             // IR LED photodiode value (22-bit sign-extended)
    uint16_t sample_number;     // Sequential sample number (wraps at 65535)
    uint8_t lead_off_status;    // Lead-off detection flags
    uint8_t quality_flags;      // Signal quality indicators
    uint8_t reserved[8];        // Reserved for future use
} __packed;

/* PPG Raw Batch Transfer - optimized for IPC efficiency */
// Batch size MUST keep the message under the 512-byte IPC/PBUF limit:
// 16 samples × 24 bytes/sample + 4-byte header = 388 bytes.
#define HPI_PPG_BATCH_SIZE 16 // Number of samples per batch sent to M4

struct hpi_ipc_ppg_raw_batch {
    uint16_t sample_count;      // Number of samples in this batch (1-16)
    uint16_t reserved;          // Alignment
    struct hpi_ipc_ppg_raw_sample samples[HPI_PPG_BATCH_SIZE];
} __packed;

/* PPG Vitals - M4 to M7 (calculated SpO2 and heart rate) */
struct hpi_ipc_ppg_vitals {
    uint32_t timestamp_ms;      // Timestamp when calculated
    
    /* Primary vital signs */
    uint8_t spo2;               // SpO2 percentage (0-100%)
    uint16_t heart_rate;        // Heart rate in BPM (0-300)
    
    /* Confidence metrics */
    uint8_t spo2_confidence;    // SpO2 confidence (0-100%)
    uint8_t hr_confidence;      // HR confidence (0-100%)
    uint8_t signal_quality;     // Overall signal quality (0-100%)
    
    /* Algorithm state */
    uint8_t algorithm_state;    // State machine status
    uint8_t perfusion_index;    // Perfusion Index (PI) in 0.1% units
    
    /* Advanced metrics */
    uint16_t rr_interval_ms;    // R-R interval for HRV
    uint16_t peak_to_peak;      // PPG amplitude (signal strength)
    
    /* Status flags */
    uint8_t flags;              // Bit flags for warnings/errors
    uint8_t reserved[13];       // Reserved for future metrics
} __packed;

/* PPG Algorithm State Values */
#define HPI_PPG_ALGO_STATE_INIT         0x00    // Initializing
#define HPI_PPG_ALGO_STATE_SEARCHING    0x01    // Searching for pulse
#define HPI_PPG_ALGO_STATE_TRACKING     0x02    // Tracking pulse
#define HPI_PPG_ALGO_STATE_CALCULATING  0x03    // Calculating SpO2
#define HPI_PPG_ALGO_STATE_VALID        0x04    // Valid measurement

/* PPG Status Flags */
#define HPI_PPG_FLAG_LEAD_OFF           (1 << 0)    // Finger removed
#define HPI_PPG_FLAG_LOW_PERFUSION      (1 << 1)    // Weak signal
#define HPI_PPG_FLAG_MOTION_DETECTED    (1 << 2)    // Motion artifact
#define HPI_PPG_FLAG_SATURATION_LOW     (1 << 3)    // SpO2 < 90%
#define HPI_PPG_FLAG_HR_OUT_OF_RANGE    (1 << 4)    // HR abnormal

/* PPG Algorithm Configuration - M7 to M4 */
struct hpi_ipc_ppg_config {
    /* Algorithm tuning */
    uint16_t update_rate_ms;        // Output update interval (default: 1000ms)
    uint8_t averaging_window;       // Averaging samples (default: 4)
    uint8_t min_perfusion_index;    // Minimum PI threshold (default: 1%)
    
    /* Heart rate detection */
    uint16_t hr_min_bpm;            // Minimum valid HR (default: 40 BPM)
    uint16_t hr_max_bpm;            // Maximum valid HR (default: 200 BPM)
    
    /* SpO2 calibration */
    uint8_t spo2_cal_curve;         // Calibration curve selection
    uint8_t enable_motion_reject;   // Motion artifact rejection (1=on)
    
    /* Control flags */
    uint8_t enable_algorithm;       // Master enable (1=on, 0=off)
    uint8_t reset_algorithm;        // Reset state machine (1=reset)
    
    uint8_t reserved[18];           // Reserved for future parameters
} __packed;

/* ============================================================================
 * IPC MESSAGE STRUCTURES FOR ECG/QRS DETECTION ALGORITHM
 * ============================================================================ */

/* ECG Raw Sample - M7 to M4 (500 Hz sampling rate) */
struct hpi_ipc_ecg_raw_sample {
    uint32_t timestamp_ms;      // Timestamp in milliseconds
    int32_t ecg_lead1;          // ECG Lead I (24-bit sign-extended)
    int32_t ecg_lead2;          // ECG Lead II (24-bit sign-extended)
    int32_t ecg_lead3;          // ECG Lead III (calculated or measured)
    uint16_t sample_number;     // Sequential sample number (wraps at 65535)
    uint8_t lead_off_status;    // Lead-off detection flags (bits 0-2 for leads 1-3)
    uint8_t reserved;           // Alignment
} __packed;

/* ECG Raw Batch Transfer - optimized for IPC efficiency */
#define HPI_ECG_BATCH_SIZE 16  // 16×28 + 4 = 452 bytes, under the 512-byte IPC/PBUF limit
                               // Message rate: 500Hz / 16 = 31.25 batches/sec

struct hpi_ipc_ecg_raw_batch {
    uint16_t sample_count;      // Number of samples in this batch (1-16)
    uint16_t reserved;          // Alignment
    struct hpi_ipc_ecg_raw_sample samples[HPI_ECG_BATCH_SIZE];
} __packed;

/* ECG Vitals - M4 to M7 (Pan-Tompkins QRS detection results) */
struct hpi_ipc_ecg_vitals {
    uint32_t timestamp_ms;      // Timestamp when calculated

    /* Primary vital signs */
    uint16_t heart_rate;        // Heart rate in BPM (0-300)
    uint16_t rr_interval_ms;    // R-R interval in milliseconds

    /* QRS Detection metrics */
    uint16_t qrs_count;         // Total QRS complexes detected
    uint8_t qrs_confidence;     // QRS detection confidence (0-100%)
    uint8_t signal_quality;     // Overall ECG signal quality (0-100%)

    /* Heart Rate Variability - Time Domain (HRV) */
    uint16_t hrv_sdnn;          // SDNN (Standard Deviation of NN intervals) in ms
    uint16_t hrv_rmssd;         // RMSSD (Root Mean Square of Successive Differences) in ms

    /* Advanced ECG metrics */
    uint16_t qrs_width_ms;      // Average QRS complex width
    int16_t st_deviation_uv;    // ST segment deviation in microvolts
    uint8_t arrhythmia_flags;   // Detected arrhythmias (bit flags)
    uint8_t algorithm_state;    // Algorithm state machine status

    /* Status flags */
    uint8_t flags;              // Status/warning flags

    /* Heart Rate Variability - Frequency Domain (HRV) */
    uint16_t hrv_lf_power;      // LF power (0.04-0.15 Hz) in ms² (capped at 65535)
    uint16_t hrv_hf_power;      // HF power (0.15-0.4 Hz) in ms² (capped at 65535)
    uint8_t hrv_lf_hf_ratio_x10;// LF/HF ratio × 10 (e.g., 15 = 1.5)
    uint8_t hrv_lf_nu;          // LF normalized units (0-100%)
    uint8_t hrv_hf_nu;          // HF normalized units (0-100%)
    uint8_t hrv_freq_valid;     // 1 if frequency domain data is valid

    uint8_t reserved[3];        // Reserved for future metrics
} __packed;

/* ECG Algorithm State Values */
#define HPI_ECG_ALGO_STATE_INIT         0x00    // Initializing
#define HPI_ECG_ALGO_STATE_LEARNING     0x01    // Learning baseline
#define HPI_ECG_ALGO_STATE_DETECTING    0x02    // Active QRS detection
#define HPI_ECG_ALGO_STATE_TRACKING     0x03    // Tracking heart rate
#define HPI_ECG_ALGO_STATE_VALID        0x04    // Valid measurement

/* ECG Status Flags */
#define HPI_ECG_FLAG_LEAD_OFF           (1 << 0)    // Electrode disconnected
#define HPI_ECG_FLAG_LOW_AMPLITUDE      (1 << 1)    // Weak ECG signal
#define HPI_ECG_FLAG_NOISE_DETECTED     (1 << 2)    // High noise level
#define HPI_ECG_FLAG_MISSED_BEATS       (1 << 3)    // QRS detection gaps
#define HPI_ECG_FLAG_HR_OUT_OF_RANGE    (1 << 4)    // HR abnormal
#define HPI_ECG_FLAG_ST_ELEVATION       (1 << 5)    // ST segment elevated
#define HPI_ECG_FLAG_ST_DEPRESSION      (1 << 6)    // ST segment depressed

/* ECG Arrhythmia Detection Flags */
#define HPI_ECG_ARRHYTHMIA_NONE         0x00
#define HPI_ECG_ARRHYTHMIA_PVC          (1 << 0)    // Premature Ventricular Contraction
#define HPI_ECG_ARRHYTHMIA_PAC          (1 << 1)    // Premature Atrial Contraction
#define HPI_ECG_ARRHYTHMIA_AFIB         (1 << 2)    // Atrial Fibrillation
#define HPI_ECG_ARRHYTHMIA_BRADYCARDIA  (1 << 3)    // HR < 60 BPM
#define HPI_ECG_ARRHYTHMIA_TACHYCARDIA  (1 << 4)    // HR > 100 BPM

/* ECG Algorithm Configuration - M7 to M4 */
struct hpi_ipc_ecg_config {
    /* Algorithm tuning */
    uint16_t update_rate_ms;        // Output update interval (default: 1000ms)
    uint8_t qrs_threshold_scale;    // QRS detection sensitivity (default: 128)
    uint8_t noise_threshold_scale;  // Noise rejection level (default: 64)
    
    /* Heart rate detection */
    uint16_t hr_min_bpm;            // Minimum valid HR (default: 30 BPM)
    uint16_t hr_max_bpm;            // Maximum valid HR (default: 250 BPM)
    
    /* Feature detection */
    uint8_t enable_hrv;             // Calculate HRV metrics (1=on)
    uint8_t enable_st_analysis;     // ST segment detection (1=on)
    uint8_t enable_arrhythmia;      // Arrhythmia detection (1=on)
    
    /* Control flags */
    uint8_t enable_algorithm;       // Master enable (1=on, 0=off)
    uint8_t reset_algorithm;        // Reset state machine (1=reset)
    
    uint8_t reserved[17];           // Reserved for future parameters
} __packed;

/* ============================================================================
 * ECG Beat Detection Notification (M4 → M7)
 * ============================================================================
 * Lightweight beat notification: the M4 sends only the sample index (16 bytes
 * per beat); the receiver extracts the beat window from its own ring buffer.
 * Currently unused: beat classification runs on the NPU module.
 */

/**
 * @brief Beat detection notification from M4 to M7
 *
 * Sent when M4's Pan-Tompkins algorithm detects a QRS complex.
 * IPC Message Type: HPI_IPC_MSG_TYPE_BEAT_NOTIFY (0x23). Size: 16 bytes.
 */
struct hpi_ipc_beat_notify {
    uint32_t timestamp_ms;      /**< M4 uptime when R-peak detected */
    uint32_t sample_number;     /**< Absolute ECG sample number (wraps at 2^32) */
    uint16_t rr_interval_ms;    /**< R-R interval in milliseconds */
    uint16_t heart_rate_bpm;    /**< Current heart rate in BPM */
    uint8_t  qrs_confidence;    /**< QRS detection confidence 0-100% */
    uint8_t  signal_quality;    /**< ECG signal quality 0-100% */
    uint8_t  reserved[2];       /**< Padding for alignment */
} __packed;

/* ============================================================================
 * EEG Data Structures - ADS1299 8-Channel AFE (HealthyLink EEG-8CH module)
 * ============================================================================
 * 8 channels @ 250 Hz (configurable up to 16 kSPS), 24-bit signed stored as
 * int32_t in microvolts. Hardware: ADS1299 on SPI4 (PE2 SCK, PE5 MISO,
 * PE6 MOSI, PE4 CS); DRDY PG12, RESET PI3, PWDN PI6, START PI2.
 */

#define HPI_EEG_CHANNEL_COUNT 8     /* ADS1299 has 8 channels */
#define HPI_EEG_BATCH_SIZE 8        /* 8 samples × 48 bytes + 4 = 388 bytes (within 512-byte IPC limit) */

/**
 * @brief Single EEG sample from the ADS1299: 8 channels in microvolts.
 * Reference voltage 4.5V; at gain=24, LSB ≈ 22.35 nV. Size: 48 bytes.
 */
struct hpi_eeg_sensor_data_t {
    int32_t channels[HPI_EEG_CHANNEL_COUNT];  /* 8 channels in microvolts */
    uint32_t status;                           /* ADS1299 24-bit status word */
    uint8_t lead_off_p;                        /* Positive electrode lead-off status */
    uint8_t lead_off_n;                        /* Negative electrode lead-off status */
    uint8_t reserved[2];                       /* Alignment padding */
};

/**
 * @brief Single EEG sample for IPC transmission (timestamp + sequence number
 * for M4 processing). Size: 48 bytes.
 */
struct hpi_ipc_eeg_raw_sample {
    uint32_t timestamp_ms;                      /* M7 uptime when sampled */
    int32_t channels[HPI_EEG_CHANNEL_COUNT];    /* 8 channels in microvolts */
    uint32_t sample_number;                     /* Sequence number for gap detection */
    uint8_t lead_off_p;                         /* Positive electrode lead-off status */
    uint8_t lead_off_n;                         /* Negative electrode lead-off status */
    uint8_t quality_flags;                      /* Signal quality indicators */
    uint8_t reserved;                           /* Alignment padding */
} __packed;

/**
 * @brief Batch of EEG samples for IPC transmission. 8 samples per batch:
 * at 250 Hz that is 31.25 batches/sec. Size: 388 bytes (8 × 48 + 4 header),
 * within the 512-byte IPC limit.
 */
struct hpi_ipc_eeg_raw_batch {
    uint16_t sample_count;      /* Number of valid samples in this batch (1-8) */
    uint16_t reserved;          /* Alignment padding */
    struct hpi_ipc_eeg_raw_sample samples[HPI_EEG_BATCH_SIZE];
} __packed;

/**
 * @brief EEG-derived vitals and mental state metrics, M4 → M7 (FFT band
 * analysis). Bands: Delta 0.5-4 Hz, Theta 4-8 Hz, Alpha 8-13 Hz,
 * Beta 13-30 Hz, Gamma 30-100 Hz. Size: 48 bytes.
 */
struct hpi_ipc_eeg_vitals {
    uint32_t timestamp_ms;      /* M4 timestamp of computation */

    /* Frequency band powers (µV²) - FFT output */
    float delta_power;          /* 0.5-4 Hz power */
    float theta_power;          /* 4-8 Hz power */
    float alpha_power;          /* 8-13 Hz power */
    float beta_power;           /* 13-30 Hz power */
    float gamma_power;          /* 30-100 Hz power */

    /* Per-channel alpha power for topographic display (optional) */
    uint8_t alpha_by_channel[HPI_EEG_CHANNEL_COUNT];  /* 0-255 scaled */

    /* Derived mental state metrics */
    uint8_t mental_state;       /* 0=sleep, 1=drowsy, 2=relaxed, 3=focused, 4=stressed */
    uint8_t attention_level;    /* 0-100% (based on beta/theta ratio) */
    uint8_t relaxation_level;   /* 0-100% (based on alpha power) */
    uint8_t algorithm_state;    /* Algorithm status/confidence */
} __packed;

/* EEG algorithm states */
#define HPI_EEG_ALG_STATE_INIT          0x00    /* Initializing */
#define HPI_EEG_ALG_STATE_CALIBRATING   0x01    /* Baseline calibration */
#define HPI_EEG_ALG_STATE_RUNNING       0x02    /* Normal operation */
#define HPI_EEG_ALG_STATE_LOW_QUALITY   0x03    /* Poor signal quality */
#define HPI_EEG_ALG_STATE_ERROR         0xFF    /* Error condition */

/* Mental state classifications */
#define HPI_EEG_MENTAL_SLEEP      0    /* Deep sleep (high delta) */
#define HPI_EEG_MENTAL_DROWSY     1    /* Drowsiness (high theta) */
#define HPI_EEG_MENTAL_RELAXED    2    /* Relaxed (high alpha) */
#define HPI_EEG_MENTAL_FOCUSED    3    /* Active concentration (high beta) */
#define HPI_EEG_MENTAL_STRESSED   4    /* Stress/anxiety (high beta+gamma) */

/* ============================================================================
 * ARRHYTHMIA CLASSIFICATION (beat classifier results; vestigial -- ML runs on
 * the NPU module, not the M4)
 * ============================================================================
 * Model: CVxTz MIT-BIH Beat Classifier. Input: 187 samples @ 125 Hz centered
 * on the R-peak (~1.5 s). Output: 5 AAMI classes (N, S, V, F, Q).
 */

/**
 * @brief AAMI Arrhythmia classification classes
 *
 * Based on AAMI EC57 standard for automated arrhythmia detection.
 * Maps to MIT-BIH annotation symbols.
 */
enum hpi_arrhythmia_class {
    HPI_ARRHYTHMIA_NORMAL = 0,      /**< N - Normal beat (N, L, R, e, j) */
    HPI_ARRHYTHMIA_SVEB = 1,        /**< S - Supraventricular ectopic beat (A, a, J, S) */
    HPI_ARRHYTHMIA_VEB = 2,         /**< V - Ventricular ectopic beat (V, E) */
    HPI_ARRHYTHMIA_FUSION = 3,      /**< F - Fusion of normal and ventricular (F) */
    HPI_ARRHYTHMIA_UNKNOWN = 4,     /**< Q - Unknown/unclassifiable (/, f, Q) */
};

/**
 * @brief Arrhythmia classification result, M4 → M7, once per QRS detection.
 * Size: 32 bytes (cache-aligned).
 */
struct hpi_ipc_arrhythmia_result {
    uint32_t timestamp_ms;          /**< M4 timestamp when classified */
    uint32_t beat_number;           /**< Sequential beat count since start */

    /* Classification result */
    uint8_t predicted_class;        /**< enum hpi_arrhythmia_class (0-4) */
    uint8_t confidence;             /**< Prediction confidence (0-100%) */
    uint16_t inference_time_ms;     /**< TFLite inference latency in ms (max 65535ms = 65s) */

    /* Class probabilities (scaled 0-255 = 0-100%) */
    uint8_t prob_normal;            /**< P(Normal) × 255 */
    uint8_t prob_sveb;              /**< P(SVEB) × 255 */
    uint8_t prob_veb;               /**< P(VEB) × 255 */
    uint8_t prob_fusion;            /**< P(Fusion) × 255 */
    uint8_t prob_unknown;           /**< P(Unknown) × 255 */

    /* Associated ECG metrics */
    uint16_t rr_interval_ms;        /**< R-R interval for this beat */
    uint16_t qrs_width_ms;          /**< QRS complex width */

    /* Model status */
    uint8_t model_loaded;           /**< 1 if TFLite model is loaded */
    uint8_t reserved[7];            /**< Reserved for future use */
} __packed;

/**
 * @brief Arrhythmia detection statistics, M4 → M7 periodic summary.
 * Size: 32 bytes (cache-aligned).
 */
struct hpi_ipc_arrhythmia_stats {
    uint32_t timestamp_ms;          /**< Timestamp of this summary */
    uint32_t total_beats;           /**< Total beats classified */
    uint32_t period_start_ms;       /**< Start of this period */

    /* Beat counts by class */
    uint16_t count_normal;          /**< Normal beats in period */
    uint16_t count_sveb;            /**< SVEB beats in period */
    uint16_t count_veb;             /**< VEB beats in period */
    uint16_t count_fusion;          /**< Fusion beats in period */
    uint16_t count_unknown;         /**< Unknown beats in period */

    /* Performance metrics */
    uint16_t avg_inference_ms;      /**< Average inference time (ms) */
    uint16_t max_inference_ms;      /**< Maximum inference time (ms) */

    uint8_t reserved[2];            /**< Reserved for future use */
} __packed;

/**
 * @brief Arrhythmia alert notification, M4 → M7, threshold-triggered.
 * Size: 16 bytes.
 */
struct hpi_ipc_arrhythmia_alert {
    uint32_t timestamp_ms;          /**< Alert timestamp */
    uint8_t alert_type;             /**< Alert category (see below) */
    uint8_t severity;               /**< 0=info, 1=warning, 2=critical */
    uint8_t arrhythmia_class;       /**< Which class triggered alert */
    uint8_t count_in_window;        /**< Number of abnormal beats in window */
    uint16_t window_duration_sec;   /**< Monitoring window duration */
    uint8_t reserved[6];            /**< Reserved for future use */
} __packed;

/* Arrhythmia alert types */
#define HPI_ARRHYTHMIA_ALERT_VEB_FREQUENT   0x01  /**< >6 VEBs per minute */
#define HPI_ARRHYTHMIA_ALERT_VEB_COUPLET    0x02  /**< 2 consecutive VEBs */
#define HPI_ARRHYTHMIA_ALERT_VEB_RUN        0x03  /**< 3+ consecutive VEBs */
#define HPI_ARRHYTHMIA_ALERT_SVEB_FREQUENT  0x04  /**< >6 SVEBs per minute */
#define HPI_ARRHYTHMIA_ALERT_BIGEMINY       0x05  /**< Alternating normal/abnormal */
#define HPI_ARRHYTHMIA_ALERT_TRIGEMINY      0x06  /**< Every 3rd beat abnormal */
#define HPI_ARRHYTHMIA_ALERT_MODEL_ERROR    0xFF  /**< TFLite inference error */

/**
 * @brief Beat waveform with classification result (for display), M4 → M7.
 * 748 samples @ 500 Hz are decimated 4:1 to the model's 187 samples @ 125 Hz
 * (1.496 s). Size: 16-byte header + 187 × 2 bytes = 390 bytes.
 */
#define BEAT_WAVEFORM_SAMPLES 187

struct hpi_ipc_beat_waveform {
    /* Header (16 bytes) */
    uint32_t timestamp_ms;          /**< M4 timestamp when classified */
    uint32_t beat_number;           /**< Sequential beat count since start */
    uint8_t predicted_class;        /**< enum hpi_arrhythmia_class (0-4) */
    uint8_t confidence;             /**< Prediction confidence (0-100%) */
    uint16_t inference_time_us;     /**< TFLite inference latency in µs */
    uint16_t rr_interval_ms;        /**< R-R interval for this beat */
    uint8_t sample_count;           /**< Number of valid samples (usually 187) */
    uint8_t reserved;               /**< Reserved for alignment */

    /* Decimated waveform samples (187 × 2 = 374 bytes)
     * Normalized to int16_t range: -32768 to 32767
     * R-peak positioned at ~62 samples (1/3 into window)
     */
    int16_t samples[BEAT_WAVEFORM_SAMPLES];
} __packed;

/**
 * @brief Inference debug information, M4 → M7: preprocessing stats and
 * inference results for each beat. Size: 64 bytes.
 */
struct hpi_ipc_tflite_debug {
    uint32_t beat_number;           /**< Sequential beat count */

    /* Raw input statistics (748 samples @ 500Hz) */
    int32_t raw_min;                /**< Minimum raw ECG value */
    int32_t raw_max;                /**< Maximum raw ECG value */
    int32_t raw_range;              /**< max - min */

    /* R-peak position */
    uint16_t rpeak_raw_idx;         /**< R-peak index in 748-sample window */
    uint16_t rpeak_decimated_idx;   /**< R-peak index after 4:1 decimation (0-186) */

    /* Quantized input stats */
    int8_t quant_min;               /**< Minimum quantized value */
    int8_t quant_max;               /**< Maximum quantized value */
    uint8_t quant_rpeak_idx;        /**< Index of max quantized value */
    uint8_t reserved1;              /**< Padding */

    /* First 8 quantized samples (for pattern verification) */
    int8_t quant_samples[8];        /**< First 8 quantized input values */

    /* Model output (raw INT8) */
    int8_t output_raw[5];           /**< Raw INT8 output for 5 classes */
    uint8_t reserved2[3];           /**< Padding */

    /* Dequantized probabilities (0-100%) */
    uint8_t prob_normal;            /**< P(Normal) as 0-100% */
    uint8_t prob_sveb;              /**< P(SVEB) as 0-100% */
    uint8_t prob_veb;               /**< P(VEB) as 0-100% */
    uint8_t prob_fusion;            /**< P(Fusion) as 0-100% */
    uint8_t prob_unknown;           /**< P(Unknown) as 0-100% */

    /* Classification result */
    uint8_t predicted_class;        /**< 0=N, 1=S, 2=V, 3=F, 4=Q */
    uint8_t confidence;             /**< Confidence 0-100% */
    uint8_t reserved3;              /**< Padding */

    /* Timing */
    uint16_t inference_time_ms;     /**< Inference time in milliseconds */
    uint16_t rr_interval_ms;        /**< RR interval for this beat */

    uint8_t reserved4[4];           /**< Reserved for future use */
} __packed;

/**
 * @brief EEG algorithm configuration, M7 → M4: runtime tuning of filters and
 * feature extraction. Size: 32 bytes (cache-aligned).
 */
struct hpi_ipc_eeg_config {
    /* Filter parameters */
    float notch_filter_freq;        /* Power line filter: 50.0 Hz or 60.0 Hz */
    float highpass_cutoff;          /* Highpass cutoff (default: 0.5 Hz) */
    float lowpass_cutoff;           /* Lowpass cutoff (default: 45.0 Hz for EEG) */

    /* Feature extraction control */
    uint8_t enable_band_power;      /* Enable FFT band power calculation (1=on) */
    uint8_t enable_mental_state;    /* Enable mental state classification (1=on) */
    uint8_t enable_artifact_removal;/* Enable artifact detection/removal (1=on) */
    uint8_t channel_mask;           /* Active channels bitmap (bit 0 = ch1, etc.) */

    /* Update rate */
    uint16_t update_rate_ms;        /* Vitals output interval (default: 1000ms) */

    /* Control flags */
    uint8_t enable_algorithm;       /* Master enable (1=on, 0=off) */
    uint8_t reset_algorithm;        /* Reset state machine (1=reset) */

    uint8_t reserved[12];           /* Reserved for future parameters */
} __packed;



