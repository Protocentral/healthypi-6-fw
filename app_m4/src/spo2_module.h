/*
 * Copyright (c) 2024 Protocentral
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file spo2_module.h
 * @brief SpO2 and PPG-derived heart-rate algorithm module
 *
 * Computes SpO2 from dual-wavelength PPG (Red + IR) and heart rate from PPG
 * pulse detection. Pipeline: DC extraction (low-pass <0.5 Hz), AC extraction
 * (bandpass 0.5-5 Hz), systolic peak detection for HR,
 * R = (AC_red/DC_red) / (AC_ir/DC_ir), SpO2 = 110 - 25×R (empirical curve),
 * then quality assessment (perfusion index, SNR, motion).
 *
 * Data flow: M7 sends PPG (Red+IR @ 125 Hz, 16-sample batches) → M4 runs the
 * algorithm (<100µs per batch) → vitals return to the M7 for display.
 */

#ifndef SPO2_MODULE_H
#define SPO2_MODULE_H

#include <zephyr/kernel.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * ALGORITHM CONSTANTS
 * ============================================================================ */

/* Sampling Configuration */
#define SPO2_SAMPLE_RATE_HZ         125     // PPG sampling rate
#define SPO2_BATCH_SIZE             16      // Samples per IPC batch
#define SPO2_BATCHES_PER_SEC        8       // 125 Hz / 16 samples = ~8 batches/sec

/* Filter Design Constants */
#define SPO2_DC_FILTER_ORDER        4       // IIR filter order for DC extraction
#define SPO2_AC_FILTER_ORDER        4       // IIR filter order for AC extraction
#define SPO2_DC_CUTOFF_HZ           0.5     // Low-pass cutoff for baseline
#define SPO2_AC_LOW_CUTOFF_HZ       0.5     // Bandpass lower cutoff
#define SPO2_AC_HIGH_CUTOFF_HZ      5.0     // Bandpass upper cutoff (cardiac band)

/* Peak Detection */
#define SPO2_PEAK_HISTORY_SIZE      10      // Number of recent peaks to track
#define SPO2_MIN_PEAK_INTERVAL_MS   300     // 200 BPM max (60000/200)
#define SPO2_MAX_PEAK_INTERVAL_MS   2000    // 30 BPM min (60000/30)
#define SPO2_PEAK_THRESHOLD_RATIO   0.6     // Peak must be 60% of recent max

/* R-Value Calibration */
#define SPO2_R_VALUE_MIN            0.4     // Minimum valid R-value (100% SpO2)
#define SPO2_R_VALUE_MAX            2.0     // Maximum valid R-value (70% SpO2)
#define SPO2_CALIBRATION_A          110     // SpO2 = A - B×R (empirical curve)
#define SPO2_CALIBRATION_B          25      // Typical calibration constants

/* Physiological Ranges */
#define SPO2_MIN_VALID              70      // Minimum physiological SpO2 (%)
#define SPO2_MAX_VALID              100     // Maximum SpO2
#define SPO2_HR_MIN_BPM             30      // Minimum heart rate
#define SPO2_HR_MAX_BPM             200     // Maximum heart rate

/* Quality Metrics */
#define SPO2_MIN_PERFUSION_INDEX    0.5     // Minimum PI for valid reading (%)
#define SPO2_MIN_SNR_DB             6.0     // Minimum signal-to-noise ratio
#define SPO2_MIN_QUALITY_SCORE      20      // Minimum quality for reporting (0-100)

/* Update Control */
#define SPO2_UPDATE_INTERVAL_MS     1000    // Send vitals every 1 second
#define SPO2_AVERAGING_WINDOW       4       // Average last N measurements

/* ============================================================================
 * ALGORITHM STATE STRUCTURES
 * ============================================================================ */

/**
 * @brief DC Component Filter State
 * 
 * Low-pass IIR filter for extracting baseline (non-pulsatile) component.
 * Removes AC components and motion artifacts to get stable baseline.
 */
struct spo2_dc_filter_state {
    int32_t dc_red;                 // Current DC estimate (Red channel)
    int32_t dc_ir;                  // Current DC estimate (IR channel)
    int32_t dc_red_history[SPO2_DC_FILTER_ORDER];  // Filter history
    int32_t dc_ir_history[SPO2_DC_FILTER_ORDER];
};

/**
 * @brief AC Component Filter State
 * 
 * Bandpass IIR filter for extracting pulsatile (cardiac) component.
 * Isolates 0.5-5 Hz band containing heartbeat information.
 */
struct spo2_ac_filter_state {
    int32_t ac_red;                 // Current AC estimate (Red channel)
    int32_t ac_ir;                  // Current AC estimate (IR channel)
    int32_t ac_red_history[SPO2_AC_FILTER_ORDER];  // Filter history
    int32_t ac_ir_history[SPO2_AC_FILTER_ORDER];
};

/**
 * @brief Peak Detection State
 * 
 * Tracks systolic peaks in AC component for heart rate calculation.
 * Uses adaptive thresholding to handle varying signal amplitudes.
 */
struct spo2_peak_detector {
    int32_t peak_red_values[SPO2_PEAK_HISTORY_SIZE];   // Recent peak amplitudes
    int32_t peak_ir_values[SPO2_PEAK_HISTORY_SIZE];
    uint32_t peak_timestamps_ms[SPO2_PEAK_HISTORY_SIZE]; // Peak timing
    uint8_t peak_count;             // Number of valid peaks in history
    uint8_t peak_index;             // Circular buffer index
    
    int32_t last_sample_red;        // Previous sample (for edge detection)
    int32_t last_sample_ir;
    uint32_t last_peak_time_ms;     // Time of last detected peak
    
    int32_t adaptive_threshold_red; // Current detection threshold
    int32_t adaptive_threshold_ir;
};

/**
 * @brief SpO2 Calculation State
 * 
 * Maintains running averages and quality metrics for SpO2 estimation.
 */
struct spo2_calculator {
    /* Current measurements */
    uint8_t spo2;                   // Current SpO2 estimate (70-100%)
    uint16_t heart_rate_ppg;        // HR from PPG peaks (30-200 BPM)
    
    /* AC/DC ratios */
    float r_value;                  // Current R-value (AC_red/DC_red)/(AC_ir/DC_ir)
    float perfusion_index;          // PI = (AC_rms / DC) × 100
    
    /* Quality metrics */
    uint8_t signal_quality;         // Overall quality score (0-100%)
    uint8_t spo2_confidence;        // SpO2 confidence (0-100%)
    uint8_t hr_confidence;          // HR confidence (0-100%)
    float snr_db;                   // Signal-to-noise ratio
    
    /* Averaging window */
    uint8_t spo2_history[SPO2_AVERAGING_WINDOW];
    uint16_t hr_history[SPO2_AVERAGING_WINDOW];
    uint8_t history_index;
    uint8_t history_count;
    
    /* Algorithm state */
    uint8_t algorithm_state;        // State machine status
    uint8_t flags;                  // Status flags (lead-off, motion, etc.)
};

/**
 * @brief Complete SpO2 Algorithm Context
 * 
 * Top-level structure containing all algorithm state.
 */
struct spo2_algorithm_context {
    /* Filter states */
    struct spo2_dc_filter_state dc_filter;
    struct spo2_ac_filter_state ac_filter;
    
    /* Peak detection */
    struct spo2_peak_detector peak_detector;
    
    /* SpO2 calculation */
    struct spo2_calculator calculator;
    
    /* Timing control */
    uint32_t last_update_time_ms;   // Time of last vitals update
    uint32_t sample_count;          // Total samples processed
    uint32_t batch_count;           // Total batches processed
    
    /* Performance tracking */
    uint32_t processing_time_us;    // Last batch processing time
    uint32_t max_processing_time_us; // Maximum processing time
    uint32_t vitals_sent_count;     // Number of vitals messages sent
};

/**
 * @brief SpO2 Algorithm Results (sent to M7)
 * 
 * Packed structure matching hpi_ipc_ppg_vitals in hpi_common_types.h
 * Size: 48 bytes (matches IPC structure)
 */
struct spo2_algorithm_results {
    uint32_t timestamp_ms;          // Timestamp when calculated
    
    /* Primary vital signs */
    uint8_t spo2;                   // SpO2 percentage (70-100%)
    uint16_t heart_rate;            // Heart rate in BPM (30-200)
    
    /* Confidence metrics */
    uint8_t spo2_confidence;        // SpO2 confidence (0-100%)
    uint8_t hr_confidence;          // HR confidence (0-100%)
    uint8_t signal_quality;         // Overall signal quality (0-100%)
    
    /* Algorithm state */
    uint8_t algorithm_state;        // State machine status
    uint8_t perfusion_index;        // Perfusion Index (PI) in 0.1% units
    
    /* Advanced metrics */
    uint16_t rr_interval_ms;        // R-R interval (peak-to-peak)
    uint16_t peak_to_peak;          // PPG amplitude (signal strength)
    
    /* Status flags */
    uint8_t flags;                  // Bit flags for warnings/errors
    uint8_t reserved[13];           // Reserved for future metrics
} __packed;

/* ============================================================================
 * ALGORITHM STATISTICS
 * ============================================================================ */

/**
 * @brief Runtime statistics for performance monitoring
 */
struct spo2_algorithm_stats {
    uint32_t batches_processed;     // Total batches received
    uint32_t samples_processed;     // Total samples processed
    uint32_t peaks_detected;        // Total peaks found
    uint32_t vitals_sent;           // Total vitals messages sent
    uint32_t quality_rejects;       // Low-quality measurements rejected
    uint32_t avg_processing_us;     // Average processing time per batch
    uint32_t max_processing_us;     // Peak processing time
};

/* ============================================================================
 * PUBLIC API
 * ============================================================================ */

/**
 * @brief Initialize SpO2 algorithm module
 * 
 * Sets up algorithm context, registers IPC callbacks, starts processing thread.
 * Must be called during M4 initialization sequence.
 * 
 * @return 0 on success, negative errno on failure
 */
int spo2_module_init(void);

/**
 * @brief Start SpO2 algorithm processing
 * 
 * Enables algorithm thread to begin receiving and processing PPG batches.
 * Should be called after IPC is bound and ready.
 * 
 * @return 0 on success, negative errno on failure
 */
int spo2_module_start(void);

/**
 * @brief Get current algorithm statistics
 * 
 * @param stats Pointer to stats structure to fill
 * @return 0 on success, negative errno on failure
 */
int spo2_get_statistics(struct spo2_algorithm_stats *stats);

/**
 * @brief Register IPC callbacks for PPG data reception
 * 
 * Internal function to register callbacks for PPG_RAW messages from M7.
 * Called automatically during module initialization.
 * 
 * @return 0 on success, negative errno on failure
 */
int spo2_register_ipc_callbacks(void);

#ifdef __cplusplus
}
#endif

#endif /* SPO2_MODULE_H */
