/*
 * Copyright (c) 2024 ProtoCentral Electronics
 * SPDX-License-Identifier: MIT
 * 
 * Algorithm Module - M4 Core Signal Processing
 * QRS detection and heart-rate calculation
 */

#ifndef ALGORITHM_MODULE_H
#define ALGORITHM_MODULE_H

#include <zephyr/kernel.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Algorithm Results Structures */

/**
 * @brief ECG algorithm results (QRS detection, heart rate)
 */
struct ecg_algorithm_results {
    uint32_t timestamp;           /* Timestamp in milliseconds */
    uint16_t heart_rate;          /* Heart rate in BPM (30-200) */
    uint16_t rr_interval_ms;      /* RR interval in milliseconds */
    uint8_t qrs_detected;         /* 1 if QRS detected in this update, 0 otherwise */
    uint8_t signal_quality;       /* Signal quality 0-100% */
    uint8_t confidence;           /* HR confidence 0-100% */
    uint8_t reserved;             /* Padding for alignment */
} __packed;

/**
 * @brief PPG algorithm results (SpO2, perfusion index)
 */
struct ppg_algorithm_results {
    uint32_t timestamp;
    uint16_t spo2;                /* SpO2 percentage (0-100) */
    uint16_t perfusion_index;     /* PI in 0.1% units (0-200 = 0-20%) */
    uint16_t pulse_rate;          /* Pulse rate from PPG (30-200 BPM) */
    uint8_t confidence;           /* 0-100% */
    uint8_t reserved;
} __packed;

/**
 * @brief Combined vital signs update
 */
struct vital_signs_update {
    uint32_t timestamp;
    uint16_t heart_rate;          /* From ECG */
    uint16_t spo2;                /* From PPG */
    uint16_t respiration_rate;    /* Breaths per minute */
    uint8_t signal_quality_ecg;
    uint8_t signal_quality_ppg;
} __packed;

/* Algorithm Configuration */

/**
 * @brief ECG algorithm configuration parameters
 */
struct ecg_algorithm_config {
    uint16_t sample_rate;         /* Samples per second (500 Hz default) */
    uint16_t filter_lowcut;       /* High-pass cutoff in 0.1 Hz units (5 Hz default) */
    uint16_t filter_highcut;      /* Low-pass cutoff in 0.1 Hz units (15 Hz default) */
    uint16_t detection_threshold; /* QRS detection threshold (adaptive if 0) */
    uint8_t averaging_window;     /* Number of beats to average (10 default) */
    uint8_t enable_noise_reject;  /* Enable motion artifact rejection */
};

/* Algorithm Statistics */

/**
 * @brief Algorithm module statistics
 */
struct algorithm_stats {
    uint32_t ecg_batches_processed;
    uint32_t ppg_batches_processed;
    uint32_t qrs_detections_total;
    uint32_t hr_updates_sent;
    uint32_t algorithm_overruns;
    uint32_t queue_max_usage;
    uint32_t processing_time_us_max;
    uint32_t processing_time_us_avg;
};

/* Public API Functions */

/**
 * @brief Initialize algorithm module
 * 
 * Sets up threads, message queues, and algorithm state.
 * Must be called before any other algorithm functions.
 * 
 * @return 0 on success, negative errno on error
 */
int algorithm_module_init(void);

/**
 * @brief Start algorithm processing
 * 
 * Starts algorithm threads after IPC is ready.
 * 
 * @return 0 on success, negative errno on error
 */
int algorithm_module_start(void);

/**
 * @brief Get algorithm statistics
 * 
 * @param stats Pointer to statistics structure to fill
 * @return 0 on success, negative errno on error
 */
int algorithm_get_stats(struct algorithm_stats *stats);

/**
 * @brief Configure ECG algorithm parameters
 * 
 * @param config Pointer to configuration structure
 * @return 0 on success, negative errno on error
 */
int algorithm_configure_ecg(const struct ecg_algorithm_config *config);

/**
 * @brief Reset algorithm state
 * 
 * Clears all buffers and resets detection thresholds.
 * Useful for handling lead-off or signal quality issues.
 */
void algorithm_reset(void);

/* Internal Processing Functions (exposed for testing) */

/**
 * @brief Process single ECG batch (16 samples)
 * 
 * Performs QRS detection and heart rate calculation.
 * Called by ECG algorithm thread.
 * 
 * @param batch Pointer to ECG batch structure
 * @param results Pointer to results structure to fill
 * @return 0 on success, negative errno on error
 */
int ecg_process_batch(const void *batch, struct ecg_algorithm_results *results);

/**
 * @brief Detect QRS complex in filtered ECG signal
 * 
 * Implements Pan-Tompkins algorithm for QRS detection.
 * 
 * @param samples Array of filtered ECG samples
 * @param count Number of samples
 * @param detected_index Output: index of detected QRS peak (if found)
 * @return 1 if QRS detected, 0 if not, negative errno on error
 */
int qrs_detect(const int32_t *samples, size_t count, size_t *detected_index);

/**
 * @brief Calculate heart rate from RR intervals
 *
 * @param rr_intervals Array of RR intervals in milliseconds
 * @param count Number of intervals (up to 10)
 * @return Heart rate in BPM, or 0 if insufficient data
 */
uint16_t calculate_heart_rate(const uint16_t *rr_intervals, size_t count);

/* NOTE: The M4 performs signal calculations only (QRS/HR/HRV/SpO2). ML
 * arrhythmia classification runs on the HealthyLink Compute (STM32N657 NPU)
 * module when installed - its firmware is the separate
 * Protocentral/healthylink-compute-fw repository. */

/**
 * @brief Enable or disable HRV calculation (disabled by default)
 *
 * Time domain: SDNN, RMSSD, pNN50 (updated every 5 s).
 * Frequency domain: LF, HF, LF/HF ratio (updated every 60 s).
 *
 * @param enable true to enable HRV, false to disable
 */
void algorithm_set_hrv_enabled(bool enable);

/**
 * @brief Check if HRV calculation is enabled
 *
 * @return true if HRV is enabled, false otherwise
 */
bool algorithm_is_hrv_enabled(void);

/**
 * @brief Register IPC callbacks for algorithm module
 *
 * @return 0 on success, negative errno on error
 */
int algorithm_register_ipc_callbacks(void);

#ifdef __cplusplus
}
#endif

#endif /* ALGORITHM_MODULE_H */
