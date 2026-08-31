/*
 * Copyright (c) 2024 Protocentral
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file spo2_module.c
 * @brief SpO2 and PPG-derived Heart Rate Algorithm Implementation
 * 
 * SpO2 + PPG-HR algorithm
 * Processes dual-wavelength PPG signals to calculate oxygen saturation and heart rate.
 */

#include "spo2_module.h"
#include "ipc_module.h"
#include "../../app_m7/src/hpi_common_types.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

LOG_MODULE_REGISTER(spo2_module, LOG_LEVEL_DBG);

/* ============================================================================
 * CALIBRATED SPO2 LOOKUP TABLE (from HealthyPi 5)
 * ============================================================================ */

/**
 * SpO2 calibration table based on R-ratio
 * R = (AC_red/DC_red) / (AC_ir/DC_ir)
 * Formula: SpO2 = -45.060*R² + 30.354*R + 94.845
 * Table covers R values from 0.5 to 2.3 (index 50-230 = R*100)
 * Valid SpO2 range: 70-100%
 */
static const uint8_t uch_spo2_table[184] = {
    95, 95, 95, 96, 96, 96, 97, 97, 97, 97, 97, 98, 98, 98, 98, 98, 99, 99, 99, 99,
    99, 99, 99, 99, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100,
    100, 100, 100, 100, 99, 99, 99, 99, 99, 99, 99, 99, 98, 98, 98, 98, 98, 98, 97, 97,
    97, 97, 96, 96, 96, 96, 95, 95, 95, 94, 94, 94, 93, 93, 93, 92, 92, 92, 91, 91,
    90, 90, 89, 89, 89, 88, 88, 87, 87, 86, 86, 85, 85, 84, 84, 83, 82, 82, 81, 81,
    80, 80, 79, 78, 78, 77, 76, 76, 75, 74, 74, 73, 72, 72, 71, 70, 69, 69, 68, 67,
    66, 66, 65, 64, 63, 62, 62, 61, 60, 59, 58, 57, 56, 56, 55, 54, 53, 52, 51, 50,
    49, 48, 47, 46, 45, 44, 43, 42, 41, 40, 39, 38, 37, 36, 35, 34, 33, 31, 30, 29,
    28, 27, 26, 25, 23, 22, 21, 20, 19, 17, 16, 15, 14, 12, 11, 10, 9, 7, 6, 5,
    3, 2, 1
};

/* ============================================================================
 * THREAD CONFIGURATION
 * ============================================================================ */

#define SPO2_THREAD_STACK_SIZE  8192
#define SPO2_THREAD_PRIORITY    4       // Same as ECG algorithm

/* Thread declaration */
static void spo2_algorithm_thread_func(void *p1, void *p2, void *p3);

/* Thread starts suspended (K_FOREVER) - started explicitly by spo2_module_start() */
K_THREAD_DEFINE(spo2_thread_id, SPO2_THREAD_STACK_SIZE,
                spo2_algorithm_thread_func, NULL, NULL, NULL,
                SPO2_THREAD_PRIORITY, 0, K_TICKS_FOREVER);

/* ============================================================================
 * MESSAGE QUEUE FOR PPG BATCHES
 * ============================================================================ */

/* Message queue is defined in main.c and shared across modules */
#define SPO2_QUEUE_SIZE     32  // Matches main.c definition

extern struct k_msgq q_ppg_batches;

/* ============================================================================
 * GLOBAL ALGORITHM STATE
 * ============================================================================ */

/* HP5-Compatible Buffer System - 2 seconds @ SPO2_SAMPLE_RATE_HZ */
#define SPO2_BUFFER_SIZE    250     /* 2 seconds of data for reliable peak detection */

/* There is NO decimation here, deliberately.
 *
 * This module used to define HP6_SAMPLE_RATE 500 / HP5_SAMPLE_RATE 125 and drop
 * three samples in four, on the belief that it was handed the raw AFE rate. It
 * is not, and it never was at 500 Hz either: the AFE4400 runs at 250 Hz
 * (CONFIG_AFE4400_PRF_HZ) and the M7 already decimates 2:1 with averaging before
 * sending (app_m7/src/platform/ipc.c, forward_ppg). Decimating again produced
 * 125 / 4 = 31.25 Hz while every window below assumes 125 Hz -- the "2 second"
 * buffer became 8 seconds, the DC filter was tuned for the wrong fs, and the
 * peak-spacing gate that admits 30-200 BPM at 125 Hz admits only 7.5-50 BPM at
 * 31.25, so a normal pulse was rejected and no R ratio was ever computed.
 *
 * The input arrives at exactly SPO2_SAMPLE_RATE_HZ. Use every sample. */
BUILD_ASSERT(SPO2_SAMPLE_RATE_HZ == HPI_PPG_M4_RATE_HZ,
             "SpO2 window constants are sized for SPO2_SAMPLE_RATE_HZ, but the M7 "
             "feeds PPG at HPI_PPG_M4_RATE_HZ. Re-tune the filters and the "
             "peak-spacing gate before changing either.");

/* Shared buffers for IR and Red channels (matches HP5 an_x/an_y) */
static uint32_t an_x[SPO2_BUFFER_SIZE];  /* IR buffer */
static uint32_t an_y[SPO2_BUFFER_SIZE];  /* Red buffer */
static int buffer_index = 0;              /* Current write position in algorithm buffer */
static bool buffer_ready = false;         /* True when buffer is full */

static struct spo2_algorithm_context algo_ctx;
static struct spo2_algorithm_stats algo_stats;

/* Semaphore to start algorithm processing (initial count=0, max=1) */
K_SEM_DEFINE(spo2_sem_start, 0, 1);

/* ============================================================================
 * HELPER FUNCTIONS (from HealthyPi 5)
 * ============================================================================ */

/**
 * @brief Sort array in ascending order (insertion sort)
 * From HealthyPi 5 spo2_process.c
 */
static void sort_ascend(int32_t *pn_x, int32_t n_size)
{
    int32_t i, j, n_temp;
    for (i = 1; i < n_size; i++) {
        n_temp = pn_x[i];
        for (j = i; j > 0 && n_temp < pn_x[j - 1]; j--)
            pn_x[j] = pn_x[j - 1];
        pn_x[j] = n_temp;
    }
}

/**
 * @brief Calculate mean of array
 * From HealthyPi 5 spo2_process.c
 */
static int32_t calculate_mean(uint32_t *buffer, int32_t length)
{
    int64_t sum = 0;
    for (int i = 0; i < length; i++) {
        sum += buffer[i];
    }
    return (int32_t)(sum / length);
}

/* ============================================================================
 * MAXIM PEAK DETECTION ALGORITHM (from HealthyPi 5)
 * ============================================================================ */

/**
 * @brief Find peaks in signal using the Maxim algorithm (as in HealthyPi 5)
 *
 * Finds local maxima that exceed a minimum distance and height threshold.
 *
 * @param pn_locs Output: array of peak locations (indices)
 * @param pn_npks Output: number of peaks found
 * @param pn_x Input: signal buffer (inverted, so valleys become peaks)
 * @param n_size Length of signal buffer
 * @param n_min_height Minimum peak height threshold
 * @param n_min_distance Minimum distance between peaks (samples)
 * @param n_max_num Maximum number of peaks to find
 */
static void find_peaks(int32_t *pn_locs, int32_t *pn_npks, int32_t *pn_x,
                       int32_t n_size, int32_t n_min_height,
                       int32_t n_min_distance, int32_t n_max_num)
{
    *pn_npks = 0;
    
    /* Find all peaks above threshold */
    for (int i = 1; i < n_size - 1; i++) {
        if (pn_x[i] > n_min_height &&
            pn_x[i] > pn_x[i - 1] &&
            pn_x[i] >= pn_x[i + 1]) {
            
            /* Check minimum distance from previous peak */
            if (*pn_npks == 0 ||
                (i - pn_locs[*pn_npks - 1]) >= n_min_distance) {
                
                pn_locs[*pn_npks] = i;
                (*pn_npks)++;
                
                if (*pn_npks >= n_max_num) {
                    break;
                }
            }
        }
    }
    
    /* Sort peaks by height (descending) and keep top n_max_num */
    if (*pn_npks > 1) {
        /* Simple bubble sort by peak height */
        for (int i = 0; i < *pn_npks - 1; i++) {
            for (int j = i + 1; j < *pn_npks; j++) {
                if (pn_x[pn_locs[i]] < pn_x[pn_locs[j]]) {
                    /* Swap */
                    int32_t temp = pn_locs[i];
                    pn_locs[i] = pn_locs[j];
                    pn_locs[j] = temp;
                }
            }
        }
        
        /* Keep only top peaks, then re-sort by location */
        if (*pn_npks > n_max_num) {
            *pn_npks = n_max_num;
        }
        
        /* Sort by location (ascending) */
        for (int i = 0; i < *pn_npks - 1; i++) {
            for (int j = i + 1; j < *pn_npks; j++) {
                if (pn_locs[i] > pn_locs[j]) {
                    /* Swap */
                    int32_t temp = pn_locs[i];
                    pn_locs[i] = pn_locs[j];
                    pn_locs[j] = temp;
                }
            }
        }
    }
}

/* ============================================================================
 * FILTER IMPLEMENTATIONS
 * ============================================================================ */

/**
 * @brief Simple IIR low-pass filter for DC component extraction
 * 
 * Implements first-order IIR: y[n] = alpha×x[n] + (1-alpha)×y[n-1]
 * Alpha = 2π × fc / (2π × fc + fs) where fc=0.5 Hz, fs=125 Hz
 * Alpha ≈ 0.0246 (use 6/256 = 0.0234 for efficiency)
 */
static inline int32_t dc_filter_update(int32_t new_sample, int32_t *dc_state)
{
    /* IIR: dc = dc + alpha × (sample - dc) */
    /* Using fixed-point: alpha = 6/256 for ~0.5 Hz cutoff @ 125 Hz */
    int32_t error = new_sample - *dc_state;
    *dc_state += (error * 6) >> 8;  // Multiply by 6/256
    
    return *dc_state;
}

/**
 * @brief Bandpass filter for AC component extraction
 * 
 * Implements AC = sample - DC (simple high-pass)
 * Then applies simple moving average for low-pass smoothing
 */
static inline int32_t ac_filter_update(int32_t new_sample, int32_t dc_value,
                                       int32_t *ac_history, uint8_t *history_idx)
{
    /* High-pass: remove DC */
    int32_t ac = new_sample - dc_value;
    
    /* Simple 4-sample moving average for smoothing */
    ac_history[*history_idx] = ac;
    *history_idx = (*history_idx + 1) % 4;
    
    int32_t sum = 0;
    for (int i = 0; i < 4; i++) {
        sum += ac_history[i];
    }
    
    return sum / 4;
}

/* ============================================================================
 * PEAK DETECTION
 * ============================================================================ */

/**
 * @brief Detect peaks in AC signal for heart rate calculation
 * 
 * Uses simple derivative-based peak detection with adaptive thresholding.
 * Peak = zero-crossing of derivative (positive to negative) above threshold.
 * 
 * @param ac_current Current AC sample
 * @param ac_previous Previous AC sample
 * @param timestamp_ms Sample timestamp
 * @param detector Peak detector state
 * @return true if peak detected, false otherwise
 */
static bool detect_peak(int32_t ac_current, int32_t ac_previous,
                        uint32_t timestamp_ms,
                        struct spo2_peak_detector *detector)
{
    /* Calculate derivative (simple difference) */
    int32_t derivative = ac_current - ac_previous;
    
    /* Detect zero-crossing: previous positive, current negative (peak) */
    int32_t prev_deriv = detector->last_sample_ir - ac_previous;
    
    bool is_peak = (prev_deriv > 0) && (derivative <= 0) && 
                   (ac_previous > detector->adaptive_threshold_ir);
    
    /* Check minimum interval between peaks (avoid double-counting) */
    uint32_t interval_ms = timestamp_ms - detector->last_peak_time_ms;
    if (interval_ms < SPO2_MIN_PEAK_INTERVAL_MS) {
        is_peak = false;
    }
    
    if (is_peak) {
        /* Valid peak detected - store in history */
        detector->peak_ir_values[detector->peak_index] = ac_previous;
        detector->peak_timestamps_ms[detector->peak_index] = timestamp_ms;
        
        detector->peak_index = (detector->peak_index + 1) % SPO2_PEAK_HISTORY_SIZE;
        if (detector->peak_count < SPO2_PEAK_HISTORY_SIZE) {
            detector->peak_count++;
        }
        
        detector->last_peak_time_ms = timestamp_ms;
        
        /* Update adaptive threshold (60% of recent max) */
        int32_t max_peak = 0;
        for (int i = 0; i < detector->peak_count; i++) {
            if (detector->peak_ir_values[i] > max_peak) {
                max_peak = detector->peak_ir_values[i];
            }
        }
        detector->adaptive_threshold_ir = (max_peak * SPO2_PEAK_THRESHOLD_RATIO);
        
        return true;
    }
    
    detector->last_sample_ir = ac_current;
    return false;
}

/**
 * @brief Calculate heart rate from peak intervals
 * 
 * @param detector Peak detector with timestamp history
 * @return Heart rate in BPM (0 if insufficient data)
 */
static uint16_t calculate_heart_rate_from_peaks(struct spo2_peak_detector *detector)
{
    if (detector->peak_count < 3) {
        return 0;  // Need at least 3 peaks for stable HR
    }
    
    /* Calculate average interval from recent peaks */
    uint32_t total_interval_ms = 0;
    uint8_t interval_count = 0;
    
    for (int i = 1; i < detector->peak_count && i < 8; i++) {
        uint8_t curr_idx = (detector->peak_index - 1 - i + SPO2_PEAK_HISTORY_SIZE) 
                          % SPO2_PEAK_HISTORY_SIZE;
        uint8_t prev_idx = (curr_idx - 1 + SPO2_PEAK_HISTORY_SIZE) 
                          % SPO2_PEAK_HISTORY_SIZE;
        
        uint32_t interval = detector->peak_timestamps_ms[curr_idx] - 
                           detector->peak_timestamps_ms[prev_idx];
        
        /* Reject outliers (physiologically impossible intervals) */
        if (interval >= SPO2_MIN_PEAK_INTERVAL_MS && 
            interval <= SPO2_MAX_PEAK_INTERVAL_MS) {
            total_interval_ms += interval;
            interval_count++;
        }
    }
    
    if (interval_count == 0) {
        return 0;
    }
    
    /* HR (BPM) = 60000 ms/min / interval_ms */
    uint32_t avg_interval_ms = total_interval_ms / interval_count;
    uint16_t hr = (uint16_t)(60000 / avg_interval_ms);
    
    /* Clamp to physiological range */
    if (hr < SPO2_HR_MIN_BPM) hr = SPO2_HR_MIN_BPM;
    if (hr > SPO2_HR_MAX_BPM) hr = SPO2_HR_MAX_BPM;
    
    return hr;
}

/* ============================================================================
 * SPO2 CALCULATION (HealthyPi 5 Method)
 * ============================================================================ */

/* Forward declaration */
static uint8_t calculate_spo2_from_r(float r_value);

/**
 * @brief Calculate AC and DC components for each detected pulse (HP5 method)
 * 
 * For each pulse between consecutive peaks: AC = peak - valley (true
 * pulsatile amplitude), DC = (peak + valley) / 2 (local baseline).
 * Per-pulse min/max, not an RMS approximation.
 *
 * @param buffer Signal buffer (IR or Red)
 * @param peak_locs Array of peak locations (indices in buffer)
 * @param n_peaks Number of peaks
 * @param ac_values Output array for AC values
 * @param dc_values Output array for DC values
 * @param n_valid Output: number of valid AC/DC pairs calculated
 */
static void calculate_ac_dc_hp5(uint32_t *buffer, int32_t *peak_locs, int32_t n_peaks,
                                int32_t *ac_values, int32_t *dc_values, int32_t *n_valid)
{
    *n_valid = 0;
    
    if (n_peaks < 2) return;
    
    /* For each pulse between consecutive peaks */
    for (int p = 0; p < n_peaks - 1 && *n_valid < 10; p++) {
        int32_t start = peak_locs[p];
        int32_t end = peak_locs[p + 1];
        
        /* Need reasonable segment length for physiological HR range (30-200 BPM)
         * Min: 200 BPM = 300ms = 38 samples @ 125Hz
         * Max: 30 BPM = 2000ms = 250 samples @ 125Hz
         * Using 10-250 range with margin for variability
         */
        if (end - start < 10 || end - start > 250) {
            if (*n_valid == 0) {
                LOG_DBG("  → Segment[%d-%d] length=%d REJECTED (need 10-250)", 
                        start, end, end - start);
            }
            continue;
        }
        
        /* Find min and max in this pulse segment */
        uint32_t min_val = buffer[start];
        uint32_t max_val = buffer[start];
        
        for (int i = start + 1; i < end; i++) {
            if (buffer[i] < min_val) min_val = buffer[i];
            if (buffer[i] > max_val) max_val = buffer[i];
        }
        
        /* Calculate AC (peak-to-peak amplitude - FULL amplitude, not divided by 2) */
        int32_t ac = (max_val - min_val);
        
        /* Calculate DC (midpoint between peak and valley) */
        int32_t dc = (max_val + min_val) / 2;
        
        /* Debug: log the first few pulses */
        if (*n_valid < 3) {
            LOG_DBG("Pulse[%d] segment[%d-%d]: min=%u, max=%u → AC=%d, DC=%d", 
                    *n_valid, start, end, min_val, max_val, ac, dc);
        }
        
        /* Store if values are reasonable */
        if (dc > 1000 && ac > 0) {
            ac_values[*n_valid] = ac;
            dc_values[*n_valid] = dc;
            (*n_valid)++;
        } else {
            LOG_DBG("  → REJECTED: dc=%d (need >1000), ac=%d (need >0)", dc, ac);
        }
    }
}

/**
 * @brief Calculate heart rate from peak intervals (HP5 method)
 * 
 * @param peak_locs Array of peak indices
 * @param n_peaks Number of peaks
 * @param pn_heart_rate Output: calculated heart rate
 * @param pch_hr_valid Output: validity flag (0 or 1)
 */
static void calculate_heart_rate_hp5(int32_t *peak_locs, int32_t n_peaks,
                                     int32_t *pn_heart_rate, int8_t *pch_hr_valid)
{
    *pch_hr_valid = 0;
    *pn_heart_rate = 0;
    
    if (n_peaks < 2) return;
    
    /* Calculate intervals between peaks */
    /* At 125 Hz: 1 sample = 8 ms */
    int32_t total_intervals = 0;
    for (int i = 1; i < n_peaks; i++) {
        total_intervals += (peak_locs[i] - peak_locs[i-1]);
    }
    
    int32_t avg_interval_samples = total_intervals / (n_peaks - 1);
    
    /* Convert samples to ms: samples * 8 ms/sample */
    int32_t avg_interval_ms = avg_interval_samples * 8;
    
    /* HR = 60000 ms/min / interval_ms */
    if (avg_interval_ms > 0) {
        int32_t hr = 60000 / avg_interval_ms;
        
        /* Clamp to physiological range */
        if (hr >= SPO2_HR_MIN_BPM && hr <= SPO2_HR_MAX_BPM) {
            *pn_heart_rate = hr;
            *pch_hr_valid = 1;
        }
    }
}

/**
 * @brief Calculate SpO2 from AC/DC arrays (HP5 method)
 * 
 * Uses median filtering for robustness, then calculates R-value and looks up SpO2.
 * 
 * @param ac_red Array of AC values (Red channel)
 * @param dc_red Array of DC values (Red channel)  
 * @param ac_ir Array of AC values (IR channel)
 * @param dc_ir Array of DC values (IR channel)
 * @param n_valid Number of valid AC/DC pairs
 * @param pn_spo2 Output: calculated SpO2
 * @param pch_spo2_valid Output: validity flag
 */
/* SpO2 debug telemetry, surfaced to the M7 via the PPG-vitals reserved[] bytes
 * (see spo2 thread + app_m7 platform/ipc.c). For LED/gain calibration. */
int32_t g_spo2_dbg_dc_red, g_spo2_dbg_dc_ir, g_spo2_dbg_ac_red, g_spo2_dbg_r_x1000;

static void calculate_spo2_hp5(int32_t *ac_red, int32_t *dc_red,
                               int32_t *ac_ir, int32_t *dc_ir,
                               int32_t n_valid,
                               int32_t *pn_spo2, int8_t *pch_spo2_valid)
{
    *pch_spo2_valid = 0;
    *pn_spo2 = 0;
    
    if (n_valid < 1) return;
    
    /* Use median for robustness if we have multiple samples */
    int32_t median_ac_red, median_dc_red, median_ac_ir, median_dc_ir;
    
    if (n_valid >= 2) {
        /* Sort arrays to find median */
        sort_ascend(ac_red, n_valid);
        sort_ascend(dc_red, n_valid);
        sort_ascend(ac_ir, n_valid);
        sort_ascend(dc_ir, n_valid);
        
        median_ac_red = ac_red[n_valid / 2];
        median_dc_red = dc_red[n_valid / 2];
        median_ac_ir = ac_ir[n_valid / 2];
        median_dc_ir = dc_ir[n_valid / 2];
    } else {
        /* Only one sample - use it directly */
        median_ac_red = ac_red[0];
        median_dc_red = dc_red[0];
        median_ac_ir = ac_ir[0];
        median_dc_ir = dc_ir[0];
    }
    
    /* Capture for M7-side debug (set before the guard so DC is visible even
     * when R can't be computed). */
    g_spo2_dbg_dc_red  = median_dc_red;
    g_spo2_dbg_dc_ir   = median_dc_ir;
    g_spo2_dbg_ac_red  = median_ac_red;
    g_spo2_dbg_r_x1000 = 0;

    /* Calculate R-value: (AC_red/DC_red) / (AC_ir/DC_ir) */
    if (median_dc_red == 0 || median_dc_ir == 0 || median_ac_ir == 0) {
        LOG_DBG("SpO2 calc failed: invalid DC/AC (AC_red=%d, DC_red=%d, AC_ir=%d, DC_ir=%d)",
                median_ac_red, median_dc_red, median_ac_ir, median_dc_ir);
        return;
    }
    
    float ratio_red = (float)median_ac_red / (float)median_dc_red;
    float ratio_ir = (float)median_ac_ir / (float)median_dc_ir;
    float r_value = ratio_red / ratio_ir;
    g_spo2_dbg_r_x1000 = (int32_t)(r_value * 1000.0f);

    /* Debug: Log R-value calculation */
    int32_t ratio_red_int = (int32_t)(ratio_red * 10000);
    int32_t ratio_ir_int = (int32_t)(ratio_ir * 10000);
    int32_t r_value_int = (int32_t)(r_value * 1000);
    
    LOG_DBG("SpO2 AC/DC: red=%d/%d, ir=%d/%d → ratios: red=%d.%04d, ir=%d.%04d, R=%d.%03d",
            median_ac_red, median_dc_red, median_ac_ir, median_dc_ir,
            ratio_red_int / 10000, abs(ratio_red_int % 10000),
            ratio_ir_int / 10000, abs(ratio_ir_int % 10000),
            r_value_int / 1000, abs(r_value_int % 1000));
    
    /* Look up SpO2 from calibration table */
    uint8_t spo2 = calculate_spo2_from_r(r_value);
    
    LOG_DBG("SpO2 lookup result: %d%% (valid range: %d-%d%%)",
            spo2, SPO2_MIN_VALID, SPO2_MAX_VALID);
    
    if (spo2 >= SPO2_MIN_VALID && spo2 <= SPO2_MAX_VALID) {
        *pn_spo2 = spo2;
        *pch_spo2_valid = 1;
    }
}

/**
 * @brief Calculate SpO2 from R-value using calibrated lookup table
 * 
 * Uses the industry-standard calibration table from HealthyPi 5.
 * Table is indexed by R*100, covering R values from 0.5 to 2.3.
 * 
 * @param r_value R-value (AC ratio red/ir)
 * @return SpO2 percentage (70-100%), 0 if invalid
 */
static uint8_t calculate_spo2_from_r(float r_value)
{
    /* Sanity check R-value - table covers 0.5 to 2.33 */
    if (r_value < 0.5f || r_value > 2.33f) {
        return 0;  /* Invalid R-value */
    }
    
    /* Convert R-value to table index: index = (R * 100) - 50 */
    /* R=0.5 -> index 0, R=2.33 -> index 183 */
    int32_t index = (int32_t)((r_value * 100.0f) - 50.0f);
    
    /* Clamp to table bounds */
    if (index < 0) index = 0;
    if (index > 183) index = 183;
    
    /* Lookup SpO2 value */
    uint8_t spo2 = uch_spo2_table[index];
    
    /* Clamp to valid physiological range */
    if (spo2 < SPO2_MIN_VALID) {
        return SPO2_MIN_VALID;
    }
    if (spo2 > SPO2_MAX_VALID) {
        return SPO2_MAX_VALID;
    }
    
    return spo2;
}

/**
 * @brief Calculate perfusion index (signal strength metric)
 * 
 * PI = (AC_rms / DC) × 100
 * Typical range: 0.5-20% (higher = better signal)
 * 
 * @param ac_rms RMS of AC component
 * @param dc DC baseline
 * @return Perfusion index in 0.1% units (e.g., 15 = 1.5%)
 */
static uint8_t calculate_perfusion_index(int32_t ac_rms, int32_t dc)
{
    if (dc == 0) {
        return 0;
    }
    
    /* PI = (AC/DC) × 100, expressed in 0.1% units */
    float pi = ((float)ac_rms / (float)dc) * 1000.0f;
    
    if (pi > 255.0f) {
        pi = 255.0f;
    }
    
    return (uint8_t)pi;
}

/**
 * @brief Calculate RMS of recent AC samples
 * 
 * @param ac_history Array of recent AC samples
 * @param count Number of samples
 * @return RMS value
 */
static int32_t calculate_rms(int32_t *ac_history, uint8_t count)
{
    if (count == 0) {
        return 0;
    }
    
    int64_t sum_squares = 0;
    for (int i = 0; i < count; i++) {
        int64_t val = ac_history[i];
        sum_squares += val * val;
    }
    
    int64_t mean_square = sum_squares / count;
    
    /* Integer square root approximation */
    int32_t rms = 0;
    int32_t bit = 1 << 30;  // Second-to-top bit set
    
    while (bit > mean_square) {
        bit >>= 2;
    }
    
    while (bit != 0) {
        if (mean_square >= rms + bit) {
            mean_square -= rms + bit;
            rms = (rms >> 1) + bit;
        } else {
            rms >>= 1;
        }
        bit >>= 2;
    }
    
    return rms;
}

/* ============================================================================
 * PROBE-OFF DETECTION THRESHOLDS (Adjusted for HealthyPi 6 Hardware)
 * ============================================================================ */

/* HP6 AFE4400 range: raw 22-bit signed ±2,097,151; +2^21 offset maps it to
 * 0..4,194,303 unsigned. Probe on sits near the offset (~2.0-2.5M IR,
 * ~2.0-2.2M Red); saturation is near 0 or 4M; probe-off collapses to the DC
 * offset with minimal variation. HP6 perfusion index runs ~0.05-0.30%
 * (vs 0.5-2.0% on HP5 due to gain settings), so the PI threshold is 0.05%. */

#define DC_OFFSET_VALUE             2097152 /* 2^21 - added to signed values */
#define PROBE_OFF_DC_THRESHOLD      (DC_OFFSET_VALUE - 1000000)  /* <1.1M = probe off (raw < -1M) */
#define PROBE_OFF_DC_SATURATED      (DC_OFFSET_VALUE + 1000000)  /* >3.1M = saturated (raw > +1M) */
#define PROBE_OFF_PI_THRESHOLD      3       /* PI < 0.03% = probe off (HP6 - lowered to accept weaker signals) */
#define PROBE_OFF_WEAK_SIGNAL       (DC_OFFSET_VALUE - 500000)   /* <1.6M = weak (raw < -500k) */
#define PROBE_OFF_AC_LOW            10      /* AC amplitude too low */
#define PROBE_OFF_AC_HIGH           50      /* AC amplitude acceptable */

/* ============================================================================
 * HP5-STYLE FULL BUFFER PROCESSING
 * ============================================================================ */

/**
 * @brief Process full 2-second buffer using the HP5 algorithm
 *
 * DC baselines + probe-off checks, IR normalization + peak detection, HR
 * from peak intervals, per-pulse AC/DC, SpO2 from R-value, quality metrics.
 *
 * @param results Output structure for SpO2, HR, quality metrics
 * @return 0 on success, negative on error
 */
static int spo2_process_full_buffer(struct spo2_algorithm_results *results)
{
    uint64_t start_time = k_uptime_get();
    
    /* Clear results */
    memset(results, 0, sizeof(*results));
    results->timestamp_ms = (uint32_t)k_uptime_get();
    
    /* === STEP 1: Calculate DC baselines === */
    int32_t mean_ir = calculate_mean(an_x, SPO2_BUFFER_SIZE);
    int32_t mean_red = calculate_mean(an_y, SPO2_BUFFER_SIZE);

    /* === STEP 2: Probe-off detection (multi-criteria) === */
    
    /* Check 1: DC Signal Too Weak */
    if (mean_ir < PROBE_OFF_DC_THRESHOLD || mean_red < PROBE_OFF_DC_THRESHOLD) {
        LOG_WRN("Probe OFF: DC too weak (IR=%d, Red=%d)", mean_ir, mean_red);
        results->flags |= HPI_PPG_FLAG_LEAD_OFF;
        results->algorithm_state = HPI_PPG_ALGO_STATE_SEARCHING;
        return 0;
    }
    
    /* Check 2: Signal Saturation */
    if (mean_ir > PROBE_OFF_DC_SATURATED || mean_red > PROBE_OFF_DC_SATURATED) {
        LOG_WRN("Probe OFF: Signal saturated (IR=%d, Red=%d)", mean_ir, mean_red);
        results->flags |= HPI_PPG_FLAG_LEAD_OFF;
        results->algorithm_state = HPI_PPG_ALGO_STATE_SEARCHING;
        return 0;
    }
    
    /* Check 3: Weak signal warning */
    bool weak_signal = (mean_ir < PROBE_OFF_WEAK_SIGNAL || mean_red < PROBE_OFF_WEAK_SIGNAL);

    /* === STEP 3: Normalize IR signal for peak detection === */
    /* HP5 method: Remove DC, invert, and normalize to remove slow baseline drift */
    int32_t ir_normalized[SPO2_BUFFER_SIZE];
    
    /* First pass: Remove DC and invert */
    for (int i = 0; i < SPO2_BUFFER_SIZE; i++) {
        ir_normalized[i] = -((int32_t)an_x[i] - mean_ir);
    }
    
    /* Second pass: Apply moving average baseline removal (simple high-pass filter)
     * This removes slow baseline drift that can obscure peaks
     * Window size: 25 samples @ 125Hz = 0.2 seconds (removes <5 Hz drift)
     */
    int32_t window_size = 25;
    for (int i = window_size; i < SPO2_BUFFER_SIZE; i++) {
        int32_t baseline = 0;
        for (int j = 0; j < window_size; j++) {
            baseline += ir_normalized[i - j];
        }
        baseline /= window_size;
        ir_normalized[i] -= baseline;
    }
    
    /* === STEP 4: Detect peaks === */
    int32_t peak_locs[15];
    int32_t n_peaks = 0;
    
    /* Signal statistics are computed AFTER baseline removal so the adaptive
     * threshold tracks the filtered signal, not the raw drift. */
    int32_t valid_start = window_size;  /* Skip filter settling region */
    int32_t valid_count = SPO2_BUFFER_SIZE - valid_start;
    
    /* Calculate mean */
    int64_t sum = 0;
    for (int i = valid_start; i < SPO2_BUFFER_SIZE; i++) {
        sum += ir_normalized[i];
    }
    int32_t mean = (int32_t)(sum / valid_count);
    
    /* Calculate standard deviation */
    int64_t var_sum = 0;
    for (int i = valid_start; i < SPO2_BUFFER_SIZE; i++) {
        int32_t diff = ir_normalized[i] - mean;
        var_sum += (int64_t)diff * diff;
    }
    
    /* Integer square root for std_dev */
    int32_t std_dev = 1;
    if (var_sum > 0) {
        int64_t variance = var_sum / valid_count;
        int64_t x = variance;
        int64_t y = (x + 1) / 2;
        while (y < x) {
            x = y;
            y = (x + variance / x) / 2;
        }
        std_dev = (int32_t)x;
    }
    
    /* Also calculate range for debugging */
    int32_t ir_min = ir_normalized[valid_start];
    int32_t ir_max = ir_normalized[valid_start];
    for (int i = valid_start + 1; i < SPO2_BUFFER_SIZE; i++) {
        if (ir_normalized[i] < ir_min) ir_min = ir_normalized[i];
        if (ir_normalized[i] > ir_max) ir_max = ir_normalized[i];
    }
    int32_t signal_range = ir_max - ir_min;
    
    /* Adaptive peak threshold (HP6-tuned): base = mean + 0.4×std_dev,
     * floor = 20% of signal range clamped to 800-1200 (weak signals),
     * ceiling = 2000 (don't miss middle beats; dicrotic notches sit <1000).
     * Min peak distance is 94 samples (0.75 s @ 125 Hz) to reject the
     * dicrotic notch. */
    int32_t min_height = mean + (std_dev * 4) / 10;  /* mean + 0.4×std_dev */
    
    /* Use percentage-based floor for weak signals */
    int32_t percentage_threshold = signal_range / 5;  /* 20% of signal range */
    int32_t floor_threshold = (percentage_threshold < 800) ? 800 : percentage_threshold;
    if (floor_threshold > 1200) floor_threshold = 1200;  /* Cap floor at 1200 */
    
    /* Bound threshold to proven effective range for HP6 */
    if (min_height < floor_threshold) {
        min_height = floor_threshold;  /* Floor: adaptive to signal strength */
    } else if (min_height > 2000) {
        min_height = 2000;  /* Ceiling: don't miss middle beats */
    }
    
    int32_t min_distance = 94;  /* 0.75 seconds @ 125Hz = prevents dicrotic notch detection */
    
    LOG_DBG("Signal: range=%d, mean=%d, std=%d → thresh=%d (floor=%d, bounded 800-2000), min_dist=%d", 
            signal_range, mean, std_dev, min_height, floor_threshold, min_distance);
    
    find_peaks(peak_locs, &n_peaks, ir_normalized, SPO2_BUFFER_SIZE,
               min_height, min_distance, 15);
    
    /* Debug: log detected peak intervals */
    if (n_peaks >= 2) {
        LOG_DBG("Found %d peaks:", n_peaks);
        for (int p = 0; p < n_peaks && p < 5; p++) {
            if (p < n_peaks - 1) {
                int32_t interval = peak_locs[p+1] - peak_locs[p];
                LOG_DBG("  Peak[%d] @ %d, height=%d, next_interval=%d samples (%d ms)", 
                        p, peak_locs[p], ir_normalized[peak_locs[p]], interval, interval * 8);
            } else {
                LOG_DBG("  Peak[%d] @ %d, height=%d (last)", 
                        p, peak_locs[p], ir_normalized[peak_locs[p]]);
            }
        }
    }

    if (n_peaks < 2) {
        /* No pulsatile signal at all. That IS probe-off, and it must be said
         * so: this exit used to set only algorithm_state and no flag, so the
         * M7 -- which derives HP6_VIT_PPG_WEAK from LEAD_OFF/LOW_PERFUSION --
         * could never learn that the finger was absent. Verified on hardware:
         * with nothing on the sensor the M4 sent flags=0x00 forever.
         *
         * The DC checks above cannot catch this case either. They compare
         * against DC_OFFSET_VALUE +/- 1e6, but an absent signal reads as
         * ~2^21 once spo2_accumulate_batch adds the unsigned-intensity offset
         * -- dead centre of the "healthy" band. The offset IS the reading when
         * nothing is connected, so only the AC/pulsatile evidence can tell. */
        LOG_WRN("Insufficient peaks: %d -- probe off", n_peaks);
        results->flags |= HPI_PPG_FLAG_LEAD_OFF;
        results->algorithm_state = HPI_PPG_ALGO_STATE_SEARCHING;
        return 0;
    }
    
    algo_stats.peaks_detected += n_peaks;
    
    /* === STEP 5: Calculate heart rate === */
    int32_t heart_rate = 0;
    int8_t hr_valid = 0;
    
    calculate_heart_rate_hp5(peak_locs, n_peaks, &heart_rate, &hr_valid);
    
    /* === STEP 6: Calculate AC/DC for IR and Red === */
    int32_t ac_ir[10], dc_ir[10], n_valid_ir = 0;
    int32_t ac_red[10], dc_red[10], n_valid_red = 0;
    
    calculate_ac_dc_hp5(an_x, peak_locs, n_peaks, ac_ir, dc_ir, &n_valid_ir);
    calculate_ac_dc_hp5(an_y, peak_locs, n_peaks, ac_red, dc_red, &n_valid_red);
    
    LOG_DBG("Valid AC/DC pairs: IR=%d, Red=%d", n_valid_ir, n_valid_red);
    
    if (n_valid_ir < 1 || n_valid_red < 1) {
        /* Peaks were found but no usable AC/DC pair came out of them: there is
         * a signal, it is just too poor to measure. Flag it as low perfusion
         * rather than returning silently -- same reasoning as the n_peaks exit
         * above. */
        LOG_WRN("Insufficient AC/DC values (IR=%d, Red=%d) - check signal quality or peak locations", 
                n_valid_ir, n_valid_red);
        results->flags |= HPI_PPG_FLAG_LOW_PERFUSION;
        results->algorithm_state = HPI_PPG_ALGO_STATE_TRACKING;
        return 0;
    }
    
    /* === STEP 7: Calculate SpO2 === */
    int32_t spo2 = 0;
    int8_t spo2_valid = 0;
    int32_t n_valid = (n_valid_ir < n_valid_red) ? n_valid_ir : n_valid_red;
    
    calculate_spo2_hp5(ac_red, dc_red, ac_ir, dc_ir, n_valid, &spo2, &spo2_valid);
    
    /* === STEP 8: Calculate quality metrics === */
    if (n_valid_ir > 0 && n_valid_red > 0) {
        /* Use median AC and DC for quality calculation */
        sort_ascend(ac_ir, n_valid_ir);
        sort_ascend(dc_ir, n_valid_ir);
        sort_ascend(ac_red, n_valid_red);
        sort_ascend(dc_red, n_valid_red);
        
        int32_t median_ac_ir = ac_ir[n_valid_ir / 2];
        int32_t median_dc_ir = dc_ir[n_valid_ir / 2];
        int32_t median_ac_red = ac_red[n_valid_red / 2];
        int32_t median_dc_red = dc_red[n_valid_red / 2];
        
        /* Calculate Perfusion Index (PI = AC/DC × 100) */
        /* Store as PI × 100 for precision (e.g., 150 = 1.5%) */
        uint16_t pi_ir = 0, pi_red = 0;
        if (median_dc_ir > 0) {
            pi_ir = (uint16_t)((int64_t)median_ac_ir * 10000 / median_dc_ir);
        }
        if (median_dc_red > 0) {
            pi_red = (uint16_t)((int64_t)median_ac_red * 10000 / median_dc_red);
        }
        
        /* Check probe-off via Perfusion Index */
        if (pi_ir < PROBE_OFF_PI_THRESHOLD) {
            LOG_WRN("Probe OFF: PI too low (%d.%02d%%)", pi_ir / 100, pi_ir % 100);
            results->flags |= HPI_PPG_FLAG_LEAD_OFF;
            results->algorithm_state = HPI_PPG_ALGO_STATE_SEARCHING;
            return 0;
        }
        
        /* Check probe-off via AC amplitude */
        if (median_ac_ir < PROBE_OFF_AC_LOW) {
            LOG_WRN("Probe OFF: AC too weak (%d)", median_ac_ir);
            results->flags |= HPI_PPG_FLAG_LEAD_OFF;
            results->algorithm_state = HPI_PPG_ALGO_STATE_SEARCHING;
            return 0;
        }
        
        /* Calculate signal quality (confidence score 0-100) */
        int32_t confidence = 100;
        
        /* Penalize low perfusion - ADJUSTED for HP6 lower PI baseline */
        if (pi_ir < 5) {
            confidence = 0;  /* < 0.05% - no signal */
        } else if (pi_ir < 10) {
            confidence -= 60;  /* < 0.1% - very weak */
        } else if (pi_ir < 30) {
            confidence -= 30;  /* < 0.3% - weak but usable */
        } else if (pi_ir < 50) {
            confidence -= 15;  /* < 0.5% - acceptable */
        } else if (pi_ir < 100) {
            confidence -= 5;   /* < 1.0% - good */
        }
        
        /* Penalize few peaks */
        if (n_peaks < 3) {
            confidence -= 40;
        } else if (n_peaks < 4) {
            confidence -= 20;
        }
        
        /* Penalize weak signal */
        if (weak_signal) {
            confidence -= 20;
        }
        
        /* Clamp confidence */
        if (confidence < 0) confidence = 0;
        if (confidence > 100) confidence = 100;
        
        /* Fill results structure */
        results->spo2 = (uint8_t)spo2;
        results->heart_rate = (uint16_t)heart_rate;
        results->spo2_confidence = (uint8_t)confidence;
        results->hr_confidence = hr_valid ? (uint8_t)confidence : 0;
        results->signal_quality = (uint8_t)confidence;
        results->perfusion_index = (uint8_t)(pi_ir / 10);  /* Convert to 0.1% units */
        results->peak_to_peak = (uint16_t)median_ac_ir;
        
        /* RR interval from last two peaks */
        if (n_peaks >= 2) {
            int32_t interval_samples = peak_locs[n_peaks-1] - peak_locs[n_peaks-2];
            results->rr_interval_ms = (uint16_t)(interval_samples * 8);  /* 8 ms/sample @ 125Hz */
        }
        
        /* Set flags */
        if (pi_ir < 50) {  /* < 0.5% */
            results->flags |= HPI_PPG_FLAG_LOW_PERFUSION;
        }
        if (spo2 > 0 && spo2 < 90) {
            results->flags |= HPI_PPG_FLAG_SATURATION_LOW;
        }
        
        /* Determine algorithm state */
        bool is_valid = (spo2_valid && hr_valid && confidence >= 40 && pi_ir >= 30);
        results->algorithm_state = is_valid ? HPI_PPG_ALGO_STATE_VALID : HPI_PPG_ALGO_STATE_TRACKING;
        
        LOG_INF("✓ SpO2=%d%%, HR=%d BPM, PI=%d.%d%%, Q=%d%%, Valid=%d",
                spo2, heart_rate, pi_ir / 100, (pi_ir / 10) % 10, confidence, is_valid);
    }
    
    /* Update performance tracking */
    uint64_t end_time = k_uptime_get();
    algo_ctx.processing_time_us = (uint32_t)((end_time - start_time) * 1000);
    if (algo_ctx.processing_time_us > algo_ctx.max_processing_time_us) {
        algo_ctx.max_processing_time_us = algo_ctx.processing_time_us;
    }
    
    return 0;
}

/* ============================================================================
 * BATCH ACCUMULATION
 * ============================================================================ */

/**
 * @brief Accumulate PPG batch into 2-second buffer with downsampling
 * 
 * HP6 samples at 500 Hz, but HP5 algorithm expects 125 Hz.
 * Downsample by 4x (keep every 4th sample) before accumulating.
 * Accumulates 250 samples @ 125Hz (2 seconds) for reliable peak detection.
 * When buffer is full, triggers HP5-style full-buffer processing.
 * 
 * @param batch Pointer to PPG batch structure (16 samples @ 500Hz)
 * @param results Pointer to results structure (output, filled when buffer is ready)
 * @return 1 if buffer full and processed, 0 if accumulating, negative on error
 */
static int spo2_accumulate_batch(const struct hpi_ipc_ppg_raw_batch *batch,
                                 struct spo2_algorithm_results *results)
{
    static uint32_t batch_count = 0;
    
    /* Debug: Log first sample of every 100th batch at accumulation point */
    if ((batch_count % 100) == 0) {  /* Reduced frequency */
        LOG_DBG("[M4-ACCUM] Batch %u: IR[0]=%d→%u, Red[0]=%d→%u",
                batch_count,
                batch->samples[0].ir_raw,
                (uint32_t)(batch->samples[0].ir_raw + (1 << 21)),
                batch->samples[0].red_raw,
                (uint32_t)(batch->samples[0].red_raw + (1 << 21)));
    }
    batch_count++;
    
    /* Process each sample in the batch */
    for (int i = 0; i < batch->sample_count && i < HPI_PPG_BATCH_SIZE; i++) {
        const struct hpi_ipc_ppg_raw_sample *sample = &batch->samples[i];
        
        /* AFE4400 samples are 22-bit signed (±2^21); the HP5 algorithm expects
         * unsigned intensity, so add a 2^21 offset → 0..4194303. */
        const int32_t DC_OFFSET = (1 << 21);  /* 2^21 = 2097152 */
        
        an_x[buffer_index] = (uint32_t)(sample->ir_raw + DC_OFFSET);   /* IR buffer */
        an_y[buffer_index] = (uint32_t)(sample->red_raw + DC_OFFSET);  /* Red buffer */
        
        buffer_index++;
        algo_ctx.sample_count++;
        
        /* Check if buffer is full (2 seconds @ 125Hz = 250 samples) */
        if (buffer_index >= SPO2_BUFFER_SIZE) {
            buffer_index = 0;
            buffer_ready = true;
            
            /* Process full buffer using HP5 algorithm */
            int ret = spo2_process_full_buffer(results);
            
            algo_ctx.batch_count++;
            
            return (ret == 0) ? 1 : ret;  /* Return 1 = buffer processed */
        }
    }
    
    return 0;  /* Still accumulating */
}

/* ============================================================================
 * IPC CALLBACK
 * ============================================================================ */

/**
 * @brief IPC callback for PPG_RAW messages from M7
 */
static void ppg_raw_ipc_callback(enum hpi_ipc_msg_type msg_type,
                                 const void *data, size_t len,
                                 void *user_data)
{
    static uint32_t callback_count = 0;
    
    if (msg_type != HPI_IPC_MSG_TYPE_PPG_RAW || data == NULL) {
        return;
    }
    
    /* Expected size check */
    if (len != sizeof(struct hpi_ipc_ppg_raw_batch)) {
        LOG_WRN("PPG batch size mismatch: got %zu, expected %zu",
                len, sizeof(struct hpi_ipc_ppg_raw_batch));
        return;
    }
    
    const struct hpi_ipc_ppg_raw_batch *batch = 
        (const struct hpi_ipc_ppg_raw_batch *)data;

    /* Debug: Log first batch received */
    if (callback_count == 0) {
        LOG_INF("First PPG batch received: count=%u, IR[0]=%d, Red[0]=%d",
                batch->sample_count,
                batch->samples[0].ir_raw, batch->samples[0].red_raw);
    }
    callback_count++;
    
    /* Queue the batch for processing (non-blocking in callback) */
    int ret = k_msgq_put(&q_ppg_batches, batch, K_NO_WAIT);
    if (ret != 0) {
        LOG_ERR("PPG queue full, batch dropped");
    }
}

/* ============================================================================
 * ALGORITHM THREAD
 * ============================================================================ */

/**
 * @brief Main SpO2 algorithm thread
 * 
 * Dequeues PPG batches, processes them, and sends vitals to M7 periodically.
 */
static void spo2_algorithm_thread_func(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);
    
    LOG_INF("SpO2 algorithm thread started");
    
    /* Wait for start signal */
    LOG_DBG("Waiting for start semaphore...");
    k_sem_take(&spo2_sem_start, K_FOREVER);
    
    LOG_INF("SpO2 algorithm processing enabled - semaphore received!");
    LOG_INF("Thread priority: %d, stack size: %d", SPO2_THREAD_PRIORITY, SPO2_THREAD_STACK_SIZE);
    
    struct hpi_ipc_ppg_raw_batch batch;
    struct spo2_algorithm_results results;
    
    /* Track last sent values to avoid spam */
    static uint8_t last_sent_spo2 = 0;
    static uint16_t last_sent_hr = 0;
    
    LOG_INF("Using HP5-style algorithm: 2-second buffering (250 samples @ 125Hz)");
    
    while (1) {
        /* Wait for PPG batch from M7 (16 samples) */
        int ret = k_msgq_get(&q_ppg_batches, &batch, K_FOREVER);
        if (ret != 0) {
            continue;
        }
        
        algo_stats.batches_processed++;
        algo_stats.samples_processed += batch.sample_count;
        
        /* Accumulate batch into 2-second buffer */
        ret = spo2_accumulate_batch(&batch, &results);
        
        if (ret < 0) {
            LOG_ERR("Batch accumulation failed: %d", ret);
            continue;
        }
        
        if (ret == 0) {
            /* Still accumulating, not ready yet */
            continue;
        }
        
        /* ret == 1: Buffer full, processed successfully */

        /* DEBUG: surface R / DC / AC to the M7 via reserved[] for SpO2 LED+gain
         * calibration. reserved[0..3]=dc_red, [4..7]=dc_ir, [8..9]=ac_red(i16),
         * [10..11]=R*1000(i16). (ac_ir is already in peak_to_peak.) */
        {
            int16_t r16  = (int16_t)g_spo2_dbg_r_x1000;
            int16_t acr16 = (int16_t)g_spo2_dbg_ac_red;
            memcpy(&results.reserved[0], &g_spo2_dbg_dc_red, 4);
            memcpy(&results.reserved[4], &g_spo2_dbg_dc_ir, 4);
            memcpy(&results.reserved[8], &acr16, 2);
            memcpy(&results.reserved[10], &r16, 2);
        }

        bool quality_good = (results.signal_quality >= SPO2_MIN_QUALITY_SCORE);

        /* Report on EVERY completed buffer (~0.5 Hz at 250 samples / 125 Hz),
         * not only when the reading is already good.
         *
         * This used to send only when `(spo2_changed || hr_changed) &&
         * quality_good`, which made "the signal is too weak to trust"
         * unreportable BY CONSTRUCTION: the only message that carries
         * HPI_PPG_FLAG_LOW_PERFUSION was gated on the quality being good. The
         * M7 sets HP6_VIT_PPG_WEAK from exactly that flag, so the bit could
         * never be set -- verified on hardware, where 82 consecutive vitals
         * samples with no finger on the sensor carried no PPG_WEAK at all.
         *
         * It also left the M7 unable to tell "no finger" from "the SpO2
         * subsystem is dead": both were silence. Silence is the one thing a
         * consumer cannot interpret.
         *
         * 0.5 Hz is well inside the M7's HR_STALE_MS (5 s), so the heartbeat
         * also keeps the PPG heart-rate source alive between good readings
         * instead of letting it expire.
         *
         * VALUES ARE UNCHANGED. A number is still only reported when quality
         * clears SPO2_MIN_QUALITY_SCORE; below that we report ABSENCE, not a
         * guess. Zero means "not available" everywhere in the .HP6 contract and
         * never "measured zero", and spo2 is not guaranteed to be 0 here just
         * because the reading was rejected. The flags, perfusion index and
         * quality score still travel -- when the reading is bad, they ARE the
         * message. */
        if (!quality_good) {
            results.spo2 = 0;
            results.heart_rate = 0;
            algo_stats.quality_rejects++;
        }

        ret = hpi_ipc_send(HPI_IPC_MSG_TYPE_PPG_VITALS, &results, sizeof(results));
        if (ret < 0) {
            LOG_ERR("Failed to send PPG vitals: %d", ret);
        } else {
            algo_stats.vitals_sent++;
            algo_ctx.vitals_sent_count++;
            /* Log only on a change: at 0.5 Hz an unconditional log would bury
             * everything else on the console within a minute. */
            bool changed = (results.spo2 != last_sent_spo2) ||
                           (results.heart_rate != last_sent_hr);
            last_sent_spo2 = results.spo2;
            last_sent_hr = results.heart_rate;
            if (changed) {
                LOG_INF("Sent vitals: SpO2=%u%%, HR=%u BPM, PI=%u.%u%%, Q=%u%%, flags=0x%02x",
                        results.spo2, results.heart_rate,
                        results.perfusion_index / 10, results.perfusion_index % 10,
                        results.signal_quality, results.flags);
            }
        }
        
        /* Periodic statistics (every 10 buffer processes = 20 seconds) */
        if ((algo_ctx.batch_count % 10) == 0) {
            LOG_INF("📊 SpO2 Stats: Buffers=%u, Batches=%u, Samples=%u, Peaks=%u, "
                    "Time=%uµs (max %uµs), Sent=%u",
                    algo_ctx.batch_count,
                    algo_stats.batches_processed,
                    algo_stats.samples_processed,
                    algo_stats.peaks_detected,
                    algo_ctx.processing_time_us,
                    algo_ctx.max_processing_time_us,
                    algo_stats.vitals_sent);
        }
    }
}

/* ============================================================================
 * PUBLIC API IMPLEMENTATION
 * ============================================================================ */

int spo2_module_init(void)
{
    LOG_INF("Initializing SpO2 algorithm module (HP5-compatible)...");
    
    /* NOTE: Semaphore already initialized by K_SEM_DEFINE at file scope */
    
    /* Clear algorithm context */
    memset(&algo_ctx, 0, sizeof(algo_ctx));
    memset(&algo_stats, 0, sizeof(algo_stats));
    
    /* Clear buffers */
    memset(an_x, 0, sizeof(an_x));
    memset(an_y, 0, sizeof(an_y));
    buffer_index = 0;
    buffer_ready = false;
    
    /* Initialize algorithm state */
    algo_ctx.calculator.algorithm_state = HPI_PPG_ALGO_STATE_INIT;
    
    /* Set initial adaptive thresholds */
    algo_ctx.peak_detector.adaptive_threshold_red = 1000;
    algo_ctx.peak_detector.adaptive_threshold_ir = 1000;
    
    /* Register IPC callbacks */
    int ret = spo2_register_ipc_callbacks();
    if (ret < 0) {
        LOG_ERR("Failed to register IPC callbacks: %d", ret);
        return ret;
    }
    
    LOG_INF("SpO2 module initialized: 2-sec buffering, %d samples @ 125Hz",
            SPO2_BUFFER_SIZE);
    LOG_INF("Thread waiting for start signal...");
    return 0;
}

int spo2_module_start(void)
{
    LOG_INF("Starting SpO2 algorithm processing");

    /* Start the thread (was created suspended with K_TICKS_FOREVER) */
    k_thread_start(spo2_thread_id);

    /* Give semaphore to unblock waiting thread */
    k_sem_give(&spo2_sem_start);

    /* Brief yield to allow thread to wake up */
    k_yield();

    LOG_INF("SpO2 thread started and semaphore given");
    return 0;
}

int spo2_register_ipc_callbacks(void)
{
    /* Register callback for PPG_RAW messages from M7 */
    int ret = hpi_ipc_register_callback(HPI_IPC_MSG_TYPE_PPG_RAW,
                                        ppg_raw_ipc_callback,
                                        NULL);
    if (ret < 0) {
        LOG_ERR("Failed to register PPG_RAW callback: %d", ret);
        return ret;
    }
    
    LOG_INF("Registered IPC callback for PPG_RAW messages");
    return 0;
}

int spo2_get_statistics(struct spo2_algorithm_stats *stats)
{
    if (stats == NULL) {
        return -EINVAL;
    }
    
    stats->batches_processed = algo_stats.batches_processed;
    stats->samples_processed = algo_stats.samples_processed;
    stats->peaks_detected = algo_stats.peaks_detected;
    stats->vitals_sent = algo_stats.vitals_sent;
    stats->quality_rejects = algo_stats.quality_rejects;
    stats->avg_processing_us = algo_ctx.processing_time_us;
    stats->max_processing_us = algo_ctx.max_processing_time_us;
    
    return 0;
}
