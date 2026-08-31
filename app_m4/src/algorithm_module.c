/*
 * Copyright (c) 2024 ProtoCentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Algorithm Module - M4 Core Signal Processing Implementation
 * - QRS Detection and Heart Rate Calculation
 * - HRV Time Domain (SDNN, RMSSD, pNN50)
 * - HRV Frequency Domain (LF, HF, LF/HF ratio) via Welch's FFT
 */

#include "algorithm_module.h"
#include "ipc_module.h"
#include "../../app_m7/src/hpi_common_types.h"
#include <zephyr/logging/log.h>
#include <zephyr/sys/ring_buffer.h>
#include <string.h>
#include <math.h>

/* CMSIS-DSP for FFT-based HRV frequency analysis */
#include <arm_math.h>

LOG_MODULE_REGISTER(algorithm_module, LOG_LEVEL_INF);

/* Configuration Constants */
#define ECG_SAMPLE_RATE         500    /* Hz */
#define ECG_BATCH_SIZE          16     /* Samples per batch */
#define ECG_QUEUE_SIZE          32     /* Queue depth */
#define ECG_BUFFER_SIZE         256    /* Circular buffer for context */
#define RR_HISTORY_SIZE         256    /* Sized for frequency-domain HRV */

/* HRV Configuration - can be toggled at runtime via algorithm_set_hrv_enabled().
 *
 * ON by default since 2026-08-31. It defaulted to false "to save CPU", and
 * nothing anywhere in either core ever called algorithm_set_hrv_enabled(true) --
 * so SDNN and RMSSD were computed by no one and the fields they travel in read 0
 * on every device ever built. A feature that ships disabled with no way to
 * enable it is not shipped. The cost is a time-domain pass every 5 s over at
 * most RR_HISTORY_SIZE intervals, and an FFT every 60 s, on a core that is
 * otherwise idle between batches. */
#define HRV_ENABLED_DEFAULT     true
#define HRV_MIN_RR_COUNT        8      /* Minimum RR intervals for time domain HRV */
#define HRV_FREQ_MIN_RR_COUNT   32     /* Minimum RR intervals for frequency domain HRV */
#define HRV_UPDATE_INTERVAL_MS  5000   /* Update time domain HRV every 5 seconds */
#define HRV_FREQ_UPDATE_INTERVAL_MS 60000  /* Update frequency domain HRV every 60 seconds */

/* Runtime HRV enable flag */
static bool hrv_enabled = HRV_ENABLED_DEFAULT;

/* HRV Frequency Domain Configuration */
#define HRV_FFT_SIZE            256    /* FFT size for Welch's method */
#define HRV_INTERP_RATE         4.0f   /* Interpolation rate in Hz */
#define HRV_VLF_LOW             0.003f /* VLF band: 0.003 - 0.04 Hz */
#define HRV_VLF_HIGH            0.04f
#define HRV_LF_LOW              0.04f  /* LF band: 0.04 - 0.15 Hz */
#define HRV_LF_HIGH             0.15f
#define HRV_HF_LOW              0.15f  /* HF band: 0.15 - 0.4 Hz */
#define HRV_HF_HIGH             0.4f

/* QRS detection parameters (Pan-Tompkins inspired).
 * ECG arrives from the M7 in MICROVOLTS: typical R-peak 500-2000 µV,
 * baseline noise 10-50 µV. After derivative + squaring + integration,
 * R-peaks and noise separate by orders of magnitude. Thresholds adapt from
 * signal/noise estimates, so the initial values below are only starting
 * points. */
#define QRS_THRESHOLD_INITIAL   50     /* Initial threshold - will adapt up */
#define QRS_THRESHOLD_MIN       10     /* Minimum threshold floor */
#define QRS_THRESHOLD_FACTOR    0.5    /* Adaptive threshold factor */
#define QRS_REFRACTORY_MS       250    /* Refractory period in ms (max 240 BPM) */
#define QRS_SEARCH_WINDOW       25     /* Samples to search for peak */
#define QRS_LEARNING_BEATS      5      /* Number of beats before stable detection */

/* Dual-threshold tracking for noise rejection */
static int32_t signal_peak_estimate = 0;   /* Running estimate of QRS peak amplitude */
static int32_t noise_peak_estimate = 0;    /* Running estimate of noise peak amplitude */
static uint32_t learning_beat_count = 0;   /* Count beats during learning phase */

/* Filter Coefficients (simplified bandpass 5-15 Hz) */
#define FILTER_ORDER            5
static const float bp_filter_b[FILTER_ORDER] = {0.1, 0.2, 0.4, 0.2, 0.1};
static const float bp_filter_a[FILTER_ORDER] = {1.0, -0.5, 0.3, -0.1, 0.05};

/* HRV Time Domain Results */
struct hrv_time_domain {
    uint16_t sdnn_ms;       /* Standard Deviation of NN intervals (ms) */
    uint16_t rmssd_ms;      /* Root Mean Square of Successive Differences (ms) */
    uint8_t pnn50;          /* % of intervals with >50ms difference */
    uint8_t valid;          /* 1 if enough data for calculation */
    uint16_t mean_rr_ms;    /* Mean RR interval (ms) */
    uint16_t min_rr_ms;     /* Minimum RR interval */
    uint16_t max_rr_ms;     /* Maximum RR interval */
};

/* HRV Frequency Domain Results */
struct hrv_freq_domain {
    uint16_t vlf_power_ms2; /* Very Low Frequency power (0.003-0.04 Hz) ms² */
    uint16_t lf_power_ms2;  /* Low Frequency power (0.04-0.15 Hz) ms² */
    uint16_t hf_power_ms2;  /* High Frequency power (0.15-0.4 Hz) ms² */
    uint16_t total_power;   /* Total spectral power ms² */
    uint8_t lf_hf_ratio_x10;/* LF/HF ratio × 10 (e.g., 15 = 1.5) */
    uint8_t lf_nu;          /* LF normalized units (0-100%) */
    uint8_t hf_nu;          /* HF normalized units (0-100%) */
    uint8_t valid;          /* 1 if enough data for calculation */
};

/* Algorithm State */
struct ecg_algorithm_state {
    /* Signal buffers */
    int32_t ecg_buffer[ECG_BUFFER_SIZE];  /* Raw ECG circular buffer */
    int32_t filtered[ECG_BUFFER_SIZE];    /* Filtered signal */
    int32_t integrated[ECG_BUFFER_SIZE];  /* Integrated signal */
    size_t buffer_index;                   /* Current write position */
    size_t sample_count;                   /* Total samples processed */

    /* QRS detection state */
    int32_t detection_threshold;          /* Current adaptive threshold */
    uint32_t last_qrs_time_ms;            /* Timestamp of last QRS (refractory only) */
    uint64_t last_qrs_sample;             /* Absolute sample index of last QRS (RR) */
    bool in_refractory;                   /* Refractory period flag */

    /* RR interval tracking (expanded for HRV) */
    uint16_t rr_intervals[RR_HISTORY_SIZE];  /* Last 64 RR intervals */
    size_t rr_count;                      /* Number of valid intervals */
    size_t rr_index;                      /* Circular buffer index */
    size_t rr_total_count;                /* Total RR intervals ever recorded */

    /* HRV state */
    struct hrv_time_domain hrv;           /* Latest time domain HRV metrics */
    struct hrv_freq_domain hrv_freq;      /* Latest frequency domain HRV metrics */
    uint32_t last_hrv_update_ms;          /* Last time domain HRV calculation */
    uint32_t last_hrv_freq_update_ms;     /* Last frequency domain HRV calculation */

    /* Results */
    uint16_t current_hr;                  /* Latest heart rate */
    uint16_t current_rr_ms;               /* Latest RR interval */
    uint8_t signal_quality;               /* 0-100% */
    uint8_t hr_confidence;                /* 0-100% */

    /* Statistics */
    uint32_t qrs_count;
    uint32_t batches_processed;
};

static struct ecg_algorithm_state ecg_state = {
    .detection_threshold = QRS_THRESHOLD_INITIAL,
    .signal_quality = 0,
    .hr_confidence = 0,
};

/* Algorithm Configuration */
static struct ecg_algorithm_config ecg_config = {
    .sample_rate = ECG_SAMPLE_RATE,
    .filter_lowcut = 50,   /* 5.0 Hz */
    .filter_highcut = 150, /* 15.0 Hz */
    .detection_threshold = 0,  /* Adaptive */
    .averaging_window = 10,
    .enable_noise_reject = 1,
};

/* Statistics */
static struct algorithm_stats algo_stats = {0};

/* Message queue for ECG batches from M7 via IPC.
 * 32 entries = ~1 second buffering at 31.25 batches/sec */
K_MSGQ_DEFINE(q_ecg_algorithm, sizeof(struct hpi_ipc_ecg_raw_batch), 32, 4);

/* Semaphores */
K_SEM_DEFINE(sem_algorithm_start, 0, 1);

/*----------------------------------------------------------------------------*/
/* HRV Frequency Domain Analysis Buffers (Welch's FFT)                        */
/*----------------------------------------------------------------------------*/

/* CMSIS-DSP FFT instance */
static arm_rfft_fast_instance_f32 hrv_fft_instance;
static bool hrv_fft_initialized = false;

/* FFT working buffers */
static float32_t hrv_interp_buffer[HRV_FFT_SIZE];    /* Interpolated RR signal */
static float32_t hrv_window_buffer[HRV_FFT_SIZE];    /* Windowed segment */
static float32_t hrv_fft_output[HRV_FFT_SIZE];       /* FFT output (complex) */
static float32_t hrv_power_spectrum[HRV_FFT_SIZE/2]; /* Power spectrum */

/* Precomputed Hanning window (stored in RAM for speed, could be const ROM) */
static float32_t hrv_hanning_window[HRV_FFT_SIZE];

/* Forward Declarations */
static void ecg_algorithm_thread_func(void *p1, void *p2, void *p3);
static int apply_bandpass_filter(const int32_t *input, int32_t *output, size_t count);
static int calculate_derivative(const int32_t *input, int32_t *output, size_t count);
static int square_signal(const int32_t *input, int32_t *output, size_t count);
static int moving_window_integration(const int32_t *input, int32_t *output, size_t count);
static uint8_t assess_signal_quality(const int32_t *samples, size_t count);
static void update_rr_interval(uint16_t interval_ms);
static void calculate_hrv_time_domain(struct hrv_time_domain *hrv);
static void calculate_hrv_freq_domain(struct hrv_freq_domain *hrv_freq);
static void send_ecg_vitals(void);

/* Thread starts suspended (K_TICKS_FOREVER); started explicitly by
 * algorithm_module_start(). */
K_THREAD_DEFINE(ecg_algorithm_thread_id,
                4096,  /* Stack size: 4KB */
                ecg_algorithm_thread_func,
                NULL, NULL, NULL,
                3,     /* Priority: 3 (higher than IPC) */
                0, K_TICKS_FOREVER);

/*----------------------------------------------------------------------------*/
/* Public API Implementation                                                  */
/*----------------------------------------------------------------------------*/

int algorithm_module_init(void)
{
    /* Reset algorithm state */
    memset(&ecg_state, 0, sizeof(ecg_state));
    ecg_state.detection_threshold = QRS_THRESHOLD_INITIAL;

    /* Reset dual-threshold estimates and learning state */
    signal_peak_estimate = 0;
    noise_peak_estimate = 0;
    learning_beat_count = 0;

    /* Reset statistics */
    memset(&algo_stats, 0, sizeof(algo_stats));


    /* Initialize CMSIS-DSP FFT for HRV frequency analysis */
    if (!hrv_fft_initialized) {
        arm_status status = arm_rfft_fast_init_f32(&hrv_fft_instance, HRV_FFT_SIZE);
        if (status == ARM_MATH_SUCCESS) {
            hrv_fft_initialized = true;
            LOG_INF("CMSIS-DSP FFT initialized (size=%d)", HRV_FFT_SIZE);
        } else {
            LOG_ERR("CMSIS-DSP FFT init failed: %d", status);
        }

        /* Precompute Hanning window: w[n] = 0.5 * (1 - cos(2πn/(N-1))) */
        for (int i = 0; i < HRV_FFT_SIZE; i++) {
            hrv_hanning_window[i] = 0.5f * (1.0f - arm_cos_f32(2.0f * PI * i / (HRV_FFT_SIZE - 1)));
        }
        LOG_INF("Hanning window precomputed");
    }

    LOG_INF("Algorithm module initialized (queue=%d, RR_buffer=%d samples)",
            ECG_QUEUE_SIZE, RR_HISTORY_SIZE);
    LOG_INF("HRV calculation: %s (call algorithm_set_hrv_enabled() to change)",
            hrv_enabled ? "ENABLED" : "DISABLED");

    return 0;
}

int algorithm_module_start(void)
{
    /* Start the ECG algorithm thread (was created suspended with K_TICKS_FOREVER) */
    k_thread_start(ecg_algorithm_thread_id);

    /* Signal algorithm thread to begin processing */
    k_sem_give(&sem_algorithm_start);

    LOG_INF("Algorithm module started");
    return 0;
}

int algorithm_get_stats(struct algorithm_stats *stats)
{
    if (!stats) {
        return -EINVAL;
    }
    
    memcpy(stats, &algo_stats, sizeof(struct algorithm_stats));
    return 0;
}

int algorithm_configure_ecg(const struct ecg_algorithm_config *config)
{
    if (!config) {
        return -EINVAL;
    }
    
    memcpy(&ecg_config, config, sizeof(struct ecg_algorithm_config));

    return 0;
}

void algorithm_reset(void)
{
    /* Clear buffers but keep configuration */
    memset(ecg_state.ecg_buffer, 0, sizeof(ecg_state.ecg_buffer));
    memset(ecg_state.filtered, 0, sizeof(ecg_state.filtered));
    memset(ecg_state.integrated, 0, sizeof(ecg_state.integrated));

    ecg_state.buffer_index = 0;
    ecg_state.sample_count = 0;
    ecg_state.detection_threshold = QRS_THRESHOLD_INITIAL;
    ecg_state.last_qrs_time_ms = 0;
    ecg_state.last_qrs_sample = 0;
    ecg_state.in_refractory = false;

    /* Reset dual-threshold estimates and learning state */
    signal_peak_estimate = 0;
    noise_peak_estimate = 0;
    learning_beat_count = 0;

    memset(ecg_state.rr_intervals, 0, sizeof(ecg_state.rr_intervals));
    ecg_state.rr_count = 0;
    ecg_state.rr_index = 0;
}

/*----------------------------------------------------------------------------*/
/* ECG Algorithm Thread                                                       */
/*----------------------------------------------------------------------------*/

static void ecg_algorithm_thread_func(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);
    
    struct hpi_ipc_ecg_raw_batch batch;
    struct ecg_algorithm_results results;
    int ret;
    uint32_t start_time, processing_time;

    /* Wait for start signal */
    k_sem_take(&sem_algorithm_start, K_FOREVER);
    
    LOG_INF("ECG algorithm thread active");
    
    while (1) {
        /* Wait for ECG batch from IPC */
        ret = k_msgq_get(&q_ecg_algorithm, &batch, K_FOREVER);

        if (ret != 0) {
            LOG_ERR("Failed to get ECG batch from queue: %d", ret);
            continue;
        }
        
        /* Start timing */
        start_time = k_cycle_get_32();
        
        /* Process the batch */
        ret = ecg_process_batch(&batch, &results);
        if (ret < 0) {
            LOG_ERR("Failed to process ECG batch: %d", ret);
            continue;
        }
        
        /* Calculate processing time */
        processing_time = k_cyc_to_us_floor32(k_cycle_get_32() - start_time);
        
        /* Update statistics */
        algo_stats.ecg_batches_processed++;
        if (processing_time > algo_stats.processing_time_us_max) {
            algo_stats.processing_time_us_max = processing_time;
        }
        algo_stats.processing_time_us_avg = 
            (algo_stats.processing_time_us_avg * 9 + processing_time) / 10;
        
        /* Check queue usage */
        uint32_t queue_usage = k_msgq_num_used_get(&q_ecg_algorithm);
        if (queue_usage > algo_stats.queue_max_usage) {
            algo_stats.queue_max_usage = queue_usage;
        }
        
        /* Calculate HRV periodically when enabled and we have enough data */
        uint32_t current_time = k_uptime_get_32();
        bool hrv_just_calculated = false;

        if (hrv_enabled) {
            /* Time domain HRV: every 5 seconds with minimum 8 RR intervals */
            if (ecg_state.rr_count >= HRV_MIN_RR_COUNT &&
                (current_time - ecg_state.last_hrv_update_ms) >= HRV_UPDATE_INTERVAL_MS) {
                LOG_INF("HRV time domain trigger: rr_count=%u, time_since_last=%u ms",
                        ecg_state.rr_count,
                        current_time - ecg_state.last_hrv_update_ms);
                calculate_hrv_time_domain(&ecg_state.hrv);
                ecg_state.last_hrv_update_ms = current_time;
                hrv_just_calculated = true;
            }

            /* Frequency domain HRV: every 60 seconds with minimum 32 RR intervals */
            if (ecg_state.rr_count >= HRV_FREQ_MIN_RR_COUNT &&
                (current_time - ecg_state.last_hrv_freq_update_ms) >= HRV_FREQ_UPDATE_INTERVAL_MS) {
                LOG_INF("HRV freq domain trigger: rr_count=%u, time_since_last=%u ms",
                        ecg_state.rr_count,
                        current_time - ecg_state.last_hrv_freq_update_ms);
                calculate_hrv_freq_domain(&ecg_state.hrv_freq);
                ecg_state.last_hrv_freq_update_ms = current_time;
                hrv_just_calculated = true;
            }
        }

        /* Send ECG vitals to M7:
         * - On every QRS detection (for real-time RR interval)
         * - When HR changes significantly
         * - Immediately after HRV calculation (to send VALID state) */
        static uint16_t last_sent_hr = 0;
        bool hr_changed = (results.heart_rate != last_sent_hr) && (results.heart_rate > 0);

        if (results.qrs_detected || hr_changed || hrv_just_calculated) {
            send_ecg_vitals();
            if (hr_changed) {
                algo_stats.hr_updates_sent++;
                last_sent_hr = results.heart_rate;
            }
        }

        /* Periodic status logging - simplified QRS counter only (every 500 batches = ~16 seconds) */
        if (algo_stats.ecg_batches_processed % 500 == 0) {
            LOG_INF("QRS: %u detected, HR=%u bpm",
                    algo_stats.qrs_detections_total,
                    ecg_state.current_hr);
        }
    }
}

/*----------------------------------------------------------------------------*/
/* ECG Batch Processing                                                       */
/*----------------------------------------------------------------------------*/

int ecg_process_batch(const void *batch_ptr, struct ecg_algorithm_results *results)
{
    if (!batch_ptr || !results) {
        return -EINVAL;
    }

    const struct hpi_ipc_ecg_raw_batch *batch =
        (const struct hpi_ipc_ecg_raw_batch *)batch_ptr;
    
    /* Validate batch */
    if (batch->sample_count != ECG_BATCH_SIZE) {
        LOG_WRN("Invalid batch size: %d (expected %d)", 
                batch->sample_count, ECG_BATCH_SIZE);
        return -EINVAL;
    }
    
    /* Extract ECG Lead I samples (we'll use lead I for QRS detection) */
    int32_t samples[ECG_BATCH_SIZE];
    for (size_t i = 0; i < ECG_BATCH_SIZE; i++) {
        samples[i] = batch->samples[i].ecg_lead1;  /* Use Lead I */
    }
    
    /* Assess signal quality */
    uint8_t quality = assess_signal_quality(samples, ECG_BATCH_SIZE);
    ecg_state.signal_quality = quality;
    
    /* Add samples to circular buffer */
    for (size_t i = 0; i < ECG_BATCH_SIZE; i++) {
        ecg_state.ecg_buffer[ecg_state.buffer_index] = samples[i];
        ecg_state.buffer_index = (ecg_state.buffer_index + 1) % ECG_BUFFER_SIZE;
        ecg_state.sample_count++;
    }
    
    /* Wait for buffer to fill before processing */
    if (ecg_state.sample_count < ECG_BUFFER_SIZE) {
        memset(results, 0, sizeof(*results));
        results->signal_quality = quality;
        return 0;  /* Not enough data yet */
    }
    
    /* Apply bandpass filter (5-15 Hz) */
    int32_t filtered[ECG_BATCH_SIZE];
    apply_bandpass_filter(samples, filtered, ECG_BATCH_SIZE);
    
    /* Derivative (emphasizes QRS slope) */
    int32_t derivative[ECG_BATCH_SIZE];
    calculate_derivative(filtered, derivative, ECG_BATCH_SIZE);
    
    /* Squaring (amplifies QRS, suppresses noise) */
    int32_t squared[ECG_BATCH_SIZE];
    square_signal(derivative, squared, ECG_BATCH_SIZE);
    
    /* Moving window integration (smooths) */
    int32_t integrated[ECG_BATCH_SIZE];
    moving_window_integration(squared, integrated, ECG_BATCH_SIZE);

    /* QRS detection */
    size_t qrs_index;
    int qrs_found = qrs_detect(integrated, ECG_BATCH_SIZE, &qrs_index);
    
    /* Build results */
    memset(results, 0, sizeof(*results));
    results->timestamp = k_uptime_get_32();
    results->signal_quality = quality;
    
    if (qrs_found > 0) {
        /* QRS detected! */
        algo_stats.qrs_detections_total++;
        ecg_state.qrs_count++;

        results->qrs_detected = 1;

        /* Calculate RR interval FROM SAMPLE INDICES, not the wall clock.
         *
         * This used to be a difference of k_uptime_get_32() readings taken when
         * the batch was *processed*. A batch is 16 samples = 32 ms, so every RR
         * was quantised to the batch period and carried the workqueue's
         * scheduling jitter on top. SDNN and RMSSD in a healthy adult at rest
         * are tens of milliseconds -- the same order as that error -- so the
         * numbers looked plausible and measured mostly the scheduler. HR
         * survived it (averaged over >= 3 intervals) but HRV cannot.
         *
         * qrs_index is the QRS position within this batch, and sample_count has
         * already been advanced past the batch, so the absolute index is
         * sample_count - ECG_BATCH_SIZE + qrs_index. At 500 Hz that is 2 ms
         * resolution, set by the ADC, and free of scheduling noise. */
        uint32_t current_time_ms = results->timestamp;
        uint64_t qrs_sample = ecg_state.sample_count - ECG_BATCH_SIZE + qrs_index;
        uint16_t rr_ms = 0;

        if (ecg_state.last_qrs_sample > 0) {
            uint64_t d = qrs_sample - ecg_state.last_qrs_sample;
            uint32_t ms = (uint32_t)((d * 1000U) / HPI_ECG_M4_RATE_HZ);

            /* Validate physiological range for HR calculation (300-2000ms = 30-200 BPM) */
            if (ms >= 300U && ms <= 2000U) {
                rr_ms = (uint16_t)ms;
                update_rr_interval(rr_ms);
                results->rr_interval_ms = rr_ms;
                ecg_state.current_rr_ms = rr_ms;
            }
        }

        ecg_state.last_qrs_sample = qrs_sample;
        ecg_state.last_qrs_time_ms = current_time_ms;  /* refractory gate only */
    }
    
    /* Calculate heart rate from RR intervals */
    if (ecg_state.rr_count >= 3) {
        results->heart_rate = calculate_heart_rate(ecg_state.rr_intervals, 
                                                     ecg_state.rr_count);
        ecg_state.current_hr = results->heart_rate;
        
        /* Confidence based on RR interval variability */
        results->confidence = (ecg_state.rr_count >= 8) ? 90 : 
                             (ecg_state.rr_count >= 5) ? 70 : 50;
        ecg_state.hr_confidence = results->confidence;
    } else {
        results->heart_rate = ecg_state.current_hr;  /* Use last known */
        results->confidence = 0;  /* Not enough data */
    }
    
    ecg_state.batches_processed++;
    
    return 0;
}

/*----------------------------------------------------------------------------*/
/* QRS Detection (Simplified Pan-Tompkins)                                   */
/*----------------------------------------------------------------------------*/

int qrs_detect(const int32_t *samples, size_t count, size_t *detected_index)
{
    if (!samples || count == 0) {
        return -EINVAL;
    }

    /* Check refractory period */
    uint32_t current_time_ms = k_uptime_get_32();
    if (ecg_state.in_refractory) {
        if (current_time_ms - ecg_state.last_qrs_time_ms < QRS_REFRACTORY_MS) {
            return 0;  /* Still in refractory period */
        }
        ecg_state.in_refractory = false;
    }

    /* Find peak in integrated signal */
    int32_t max_val = 0;
    size_t max_idx = 0;

    for (size_t i = 0; i < count; i++) {
        if (samples[i] > max_val) {
            max_val = samples[i];
            max_idx = i;
        }
    }

    /* Sanity check: If max_val is much larger than threshold but we've been
     * missing QRS for a while, reset the estimates. This handles cases where
     * signal estimates got corrupted earlier. */
    static uint32_t batches_since_qrs = 0;
    batches_since_qrs++;

    /* If we haven't detected QRS for >3 seconds (100 batches at 31.25/sec),
     * and current max is reasonable (>50), reset and try to learn again */
    if (batches_since_qrs > 100 && max_val > 50 && learning_beat_count >= QRS_LEARNING_BEATS) {
        LOG_WRN("QRS timeout: resetting signal estimates (max=%d, old_sig=%d)",
                max_val, signal_peak_estimate);
        signal_peak_estimate = max_val * 2;  /* Start with current as estimate */
        noise_peak_estimate = max_val / 4;
        learning_beat_count = 3;  /* Partial re-learning */
    }

    /* Adaptive threshold, relative to signal amplitude (never absolute):
     * learning phase uses a very low threshold to catch the first beats;
     * afterwards it sits 25% of the way from the noise estimate to the
     * signal estimate (Pan-Tompkins style). */
    int32_t adaptive_threshold;

    if (learning_beat_count < QRS_LEARNING_BEATS) {
        /* Learning phase: use very low threshold to catch first beats
         * Threshold = max(MIN, noise_estimate * 2) to stay above noise floor */
        adaptive_threshold = noise_peak_estimate * 2;
        if (adaptive_threshold < QRS_THRESHOLD_MIN) {
            adaptive_threshold = QRS_THRESHOLD_MIN;
        }
        /* But don't go too high during learning */
        if (adaptive_threshold > QRS_THRESHOLD_INITIAL * 10) {
            adaptive_threshold = QRS_THRESHOLD_INITIAL * 10;
        }
    } else {
        /* Stable phase: use Pan-Tompkins style threshold
         * Threshold at 25% between noise and signal peaks */
        if (signal_peak_estimate > noise_peak_estimate) {
            adaptive_threshold = noise_peak_estimate +
                                (signal_peak_estimate - noise_peak_estimate) / 4;
        } else {
            /* Fallback if estimates are inverted */
            adaptive_threshold = signal_peak_estimate / 2;
        }

        /* Ensure minimum threshold */
        if (adaptive_threshold < QRS_THRESHOLD_MIN) {
            adaptive_threshold = QRS_THRESHOLD_MIN;
        }
    }

    /* Update detection threshold */
    ecg_state.detection_threshold = adaptive_threshold;

    if (max_val > adaptive_threshold) {
        /* Potential QRS detected - update signal peak estimate
         * During learning: fast adaptation (50% old + 50% new)
         * After learning: slow adaptation (87.5% old + 12.5% new) */
        batches_since_qrs = 0;  /* Reset timeout counter */

        if (learning_beat_count < QRS_LEARNING_BEATS) {
            signal_peak_estimate = (signal_peak_estimate + max_val) / 2;
            learning_beat_count++;
            LOG_INF("Learning beat %u: peak=%d, new_sig_est=%d",
                    learning_beat_count, max_val, signal_peak_estimate);
        } else {
            signal_peak_estimate = (signal_peak_estimate * 7 + max_val) / 8;
        }

        *detected_index = max_idx;
        ecg_state.in_refractory = true;

        LOG_DBG("QRS detected: peak=%d, thresh=%d", max_val, adaptive_threshold);

        return 1;  /* QRS detected */
    } else {
        /* No QRS - this peak is noise, update noise estimate
         * Use faster adaptation during learning */
        if (learning_beat_count < QRS_LEARNING_BEATS) {
            noise_peak_estimate = (noise_peak_estimate + max_val) / 2;
        } else {
            noise_peak_estimate = (noise_peak_estimate * 7 + max_val) / 8;
        }
    }

    /* Slowly decay signal estimate if no QRS detected for a while
     * This handles the case where leads are disconnected */
    if (ecg_state.batches_processed % 32 == 0 && learning_beat_count >= QRS_LEARNING_BEATS) {
        /* Decay signal estimate by ~3% */
        signal_peak_estimate = (signal_peak_estimate * 31) / 32;

        /* Don't let signal estimate fall below noise + minimum gap */
        if (signal_peak_estimate < noise_peak_estimate + QRS_THRESHOLD_MIN) {
            signal_peak_estimate = noise_peak_estimate + QRS_THRESHOLD_MIN;
        }
    }

    return 0;  /* No QRS detected */
}

/*----------------------------------------------------------------------------*/
/* Signal Processing Functions                                                */
/*----------------------------------------------------------------------------*/

static int apply_bandpass_filter(const int32_t *input, int32_t *output, size_t count)
{
    /* Second-difference high-pass (y[n] = x[n] - 2x[n-1] + x[n-2]) drawn from
     * the 256-sample circular buffer so filtering is continuous across
     * batches. buffer_index is the next write slot, so the newest sample is
     * at buffer_index-1. */
    size_t newest_idx = (ecg_state.buffer_index + ECG_BUFFER_SIZE - 1) % ECG_BUFFER_SIZE;

    for (size_t i = 0; i < count; i++) {
        /* Calculate position of this sample and previous samples in circular buffer */
        size_t offset = count - 1 - i;  /* How far back from newest sample */
        size_t curr_idx = (newest_idx + ECG_BUFFER_SIZE - offset) % ECG_BUFFER_SIZE;
        size_t prev1_idx = (curr_idx + ECG_BUFFER_SIZE - 1) % ECG_BUFFER_SIZE;
        size_t prev2_idx = (curr_idx + ECG_BUFFER_SIZE - 2) % ECG_BUFFER_SIZE;

        /* Simple 3-tap high-pass filter: emphasizes changes, removes DC */
        int32_t curr = ecg_state.ecg_buffer[curr_idx];
        int32_t prev1 = ecg_state.ecg_buffer[prev1_idx];
        int32_t prev2 = ecg_state.ecg_buffer[prev2_idx];

        /* High-pass: y[n] = x[n] - 2*x[n-1] + x[n-2] (second difference) */
        output[i] = curr - 2 * prev1 + prev2;
    }

    return 0;
}

static int calculate_derivative(const int32_t *input, int32_t *output, size_t count)
{
    /* 5-point derivative: y[n] = (-x[n-2] - 2*x[n-1] + 2*x[n+1] + x[n+2]) / 8.
     * Batch edges clamp to the first/last valid sample. */
    for (size_t i = 0; i < count; i++) {
        /* Get indices, clamping to valid range */
        size_t im2 = (i >= 2) ? (i - 2) : 0;
        size_t im1 = (i >= 1) ? (i - 1) : 0;
        size_t ip1 = (i + 1 < count) ? (i + 1) : (count - 1);
        size_t ip2 = (i + 2 < count) ? (i + 2) : (count - 1);

        output[i] = (-input[im2] - 2*input[im1] + 2*input[ip1] + input[ip2]) / 8;
    }

    return 0;
}

static int square_signal(const int32_t *input, int32_t *output, size_t count)
{
    /* Square each sample (amplifies QRS, suppresses noise). No post-division:
     * it would lose small values needed for detection. Values fit in int32_t
     * (max derivative ~1000 → 1,000,000 squared). */
    for (size_t i = 0; i < count; i++) {
        int64_t squared = (int64_t)input[i] * input[i];
        /* No division - keep small values for proper detection */
        output[i] = (int32_t)squared;
    }

    return 0;
}

static int moving_window_integration(const int32_t *input, int32_t *output, size_t count)
{
    /* Moving window integration (150ms window @ 500Hz = 75 samples)
     * For batch processing, use smaller window (8 samples)
     */
    const size_t window = 8;
    
    for (size_t i = 0; i < count; i++) {
        int64_t sum = 0;
        size_t window_start = (i >= window) ? (i - window) : 0;
        
        for (size_t j = window_start; j <= i; j++) {
            sum += input[j];
        }
        
        output[i] = (int32_t)(sum / (i - window_start + 1));
    }
    
    return 0;
}

static uint8_t assess_signal_quality(const int32_t *samples, size_t count)
{
    /* Simple signal quality assessment based on:
     * - Signal amplitude (should be in reasonable range)
     * - Variability (too flat = poor quality)
     */
    
    int32_t min_val = samples[0];
    int32_t max_val = samples[0];
    int64_t sum = 0;
    
    for (size_t i = 0; i < count; i++) {
        if (samples[i] < min_val) min_val = samples[i];
        if (samples[i] > max_val) max_val = samples[i];
        sum += samples[i];
    }
    
    int32_t mean = sum / count;
    int32_t amplitude = max_val - min_val;
    
    /* Quality based on amplitude (expected range: 100-5000 for good signal) */
    uint8_t quality = 0;
    if (amplitude > 100 && amplitude < 10000) {
        quality = 80;  /* Good amplitude */
        
        /* Check for flat line (poor contact) */
        if (amplitude < 50) {
            quality = 20;  /* Likely flat line */
        }
    } else if (amplitude >= 10000) {
        quality = 30;  /* Too noisy or saturated */
    } else {
        quality = 10;  /* Too flat */
    }
    
    return quality;
}

/*----------------------------------------------------------------------------*/
/* Heart Rate Calculation                                                     */
/*----------------------------------------------------------------------------*/

static void update_rr_interval(uint16_t interval_ms)
{
    /* Add to circular buffer */
    ecg_state.rr_intervals[ecg_state.rr_index] = interval_ms;
    ecg_state.rr_index = (ecg_state.rr_index + 1) % RR_HISTORY_SIZE;
    
    if (ecg_state.rr_count < RR_HISTORY_SIZE) {
        ecg_state.rr_count++;
    }
}

uint16_t calculate_heart_rate(const uint16_t *rr_intervals, size_t count)
{
    if (!rr_intervals || count == 0) {
        return 0;
    }

    /* Average RR intervals */
    uint32_t sum = 0;
    for (size_t i = 0; i < count; i++) {
        sum += rr_intervals[i];
    }

    uint32_t avg_rr_ms = sum / count;

    /* Convert to BPM: HR = 60000 / RR_ms */
    if (avg_rr_ms == 0) {
        return 0;
    }

    uint16_t hr = 60000 / avg_rr_ms;

    /* Clamp to physiological range */
    if (hr < 30) hr = 30;
    if (hr > 200) hr = 200;

    return hr;
}

/*----------------------------------------------------------------------------*/
/* HRV Time Domain Calculation                                                */
/*----------------------------------------------------------------------------*/

/**
 * @brief Calculate HRV time domain metrics from RR intervals: SDNN (std dev
 * of NN intervals), RMSSD (RMS of successive differences), and pNN50
 * (% of successive intervals differing by >50 ms).
 *
 * @param hrv Pointer to HRV result structure to fill
 */
static void calculate_hrv_time_domain(struct hrv_time_domain *hrv)
{
    if (!hrv || ecg_state.rr_count < HRV_MIN_RR_COUNT) {
        if (hrv) {
            hrv->valid = 0;
        }
        return;
    }

    size_t count = ecg_state.rr_count;
    const uint16_t *rr = ecg_state.rr_intervals;

    /* Step 1: Calculate mean, min, max */
    uint32_t sum = 0;
    uint16_t min_rr = UINT16_MAX;
    uint16_t max_rr = 0;

    for (size_t i = 0; i < count; i++) {
        sum += rr[i];
        if (rr[i] < min_rr) min_rr = rr[i];
        if (rr[i] > max_rr) max_rr = rr[i];
    }

    uint32_t mean_rr = sum / count;
    hrv->mean_rr_ms = (uint16_t)mean_rr;
    hrv->min_rr_ms = min_rr;
    hrv->max_rr_ms = max_rr;

    /* Step 2: Calculate SDNN = sqrt(sum((RR - mean)^2) / N) */
    uint64_t variance_sum = 0;
    for (size_t i = 0; i < count; i++) {
        int32_t diff = (int32_t)rr[i] - (int32_t)mean_rr;
        variance_sum += (uint64_t)(diff * diff);
    }
    uint32_t variance = (uint32_t)(variance_sum / count);

    /* Integer square root approximation */
    uint32_t sdnn = 0;
    if (variance > 0) {
        /* Newton-Raphson integer sqrt */
        uint32_t x = variance;
        uint32_t y = (x + 1) / 2;
        while (y < x) {
            x = y;
            y = (x + variance / x) / 2;
        }
        sdnn = x;
    }
    hrv->sdnn_ms = (uint16_t)(sdnn > 65535 ? 65535 : sdnn);

    /* Step 3: Calculate RMSSD = sqrt(sum((RR[i+1] - RR[i])^2) / (N-1)) */
    uint64_t rmssd_sum = 0;
    uint32_t nn50_count = 0;

    /* Need to handle circular buffer properly */
    for (size_t i = 0; i < count - 1; i++) {
        /* Get consecutive RR intervals from circular buffer */
        size_t idx1 = (ecg_state.rr_index + RR_HISTORY_SIZE - count + i) % RR_HISTORY_SIZE;
        size_t idx2 = (idx1 + 1) % RR_HISTORY_SIZE;

        int32_t diff = (int32_t)rr[idx2] - (int32_t)rr[idx1];
        rmssd_sum += (uint64_t)(diff * diff);

        /* Count for pNN50 */
        if (diff < 0) diff = -diff;
        if (diff > 50) {
            nn50_count++;
        }
    }

    uint32_t rmssd_variance = (uint32_t)(rmssd_sum / (count - 1));
    uint32_t rmssd = 0;
    if (rmssd_variance > 0) {
        uint32_t x = rmssd_variance;
        uint32_t y = (x + 1) / 2;
        while (y < x) {
            x = y;
            y = (x + rmssd_variance / x) / 2;
        }
        rmssd = x;
    }
    hrv->rmssd_ms = (uint16_t)(rmssd > 65535 ? 65535 : rmssd);

    /* Step 4: Calculate pNN50 = (NN50 count / total) * 100 */
    hrv->pnn50 = (uint8_t)((nn50_count * 100) / (count - 1));

    hrv->valid = 1;

    LOG_INF("HRV calculated: SDNN=%u, RMSSD=%u, pNN50=%u%%, mean=%u, valid=%u",
            hrv->sdnn_ms, hrv->rmssd_ms, hrv->pnn50,
            hrv->mean_rr_ms, hrv->valid);
}

/*----------------------------------------------------------------------------*/
/* HRV Frequency Domain Calculation (Welch's FFT)                             */
/*----------------------------------------------------------------------------*/

/**
 * @brief Linearly interpolate RR intervals to uniform sampling rate
 *
 * RR intervals are unevenly spaced - we interpolate to HRV_INTERP_RATE Hz
 * for FFT processing. Uses simple linear interpolation.
 *
 * @param rr_intervals Source RR intervals in ms (circular buffer)
 * @param rr_count Number of RR intervals
 * @param rr_index Current index in circular buffer
 * @param output Output buffer for interpolated signal
 * @param output_len Length of output buffer
 * @return Number of samples written to output
 */
static size_t interpolate_rr_intervals(const uint16_t *rr_intervals, size_t rr_count,
                                       size_t rr_index, float32_t *output, size_t output_len)
{
    if (rr_count < 2 || output_len == 0) {
        return 0;
    }

    /* Calculate total time span of RR intervals */
    float32_t total_time_sec = 0;
    for (size_t i = 0; i < rr_count; i++) {
        size_t idx = (rr_index + RR_HISTORY_SIZE - rr_count + i) % RR_HISTORY_SIZE;
        total_time_sec += rr_intervals[idx] / 1000.0f;
    }

    /* Calculate number of output samples based on interpolation rate */
    size_t num_samples = (size_t)(total_time_sec * HRV_INTERP_RATE);
    if (num_samples > output_len) {
        num_samples = output_len;
    }
    if (num_samples < 8) {
        return 0;  /* Not enough data */
    }

    /* Build cumulative time array and RR values */
    float32_t t_cumulative = 0;
    size_t rr_idx = 0;
    float32_t t_prev = 0;
    float32_t rr_prev = 0;
    float32_t t_next = 0;
    float32_t rr_next = 0;

    /* Get first RR interval */
    size_t first_idx = (rr_index + RR_HISTORY_SIZE - rr_count) % RR_HISTORY_SIZE;
    rr_prev = (float32_t)rr_intervals[first_idx];
    t_next = rr_prev / 1000.0f;
    size_t next_idx = (first_idx + 1) % RR_HISTORY_SIZE;
    rr_next = (float32_t)rr_intervals[next_idx];

    /* Interpolate to uniform rate */
    float32_t dt = 1.0f / HRV_INTERP_RATE;
    for (size_t i = 0; i < num_samples; i++) {
        float32_t t = i * dt;

        /* Advance through RR intervals until we bracket current time */
        while (t >= t_next && rr_idx < rr_count - 1) {
            t_prev = t_next;
            rr_prev = rr_next;
            rr_idx++;
            size_t idx = (first_idx + rr_idx + 1) % RR_HISTORY_SIZE;
            if (rr_idx < rr_count - 1) {
                rr_next = (float32_t)rr_intervals[idx];
                t_next = t_prev + rr_prev / 1000.0f;
            }
        }

        /* Linear interpolation */
        if (t_next > t_prev) {
            float32_t alpha = (t - t_prev) / (t_next - t_prev);
            output[i] = rr_prev + alpha * (rr_next - rr_prev);
        } else {
            output[i] = rr_prev;
        }
    }

    return num_samples;
}

/**
 * @brief Calculate HRV frequency domain metrics using Welch's FFT method
 *
 * Computes power spectral density of RR intervals and extracts:
 * - VLF power (0.003-0.04 Hz): thermoregulation
 * - LF power (0.04-0.15 Hz): mixed SNS/PNS
 * - HF power (0.15-0.4 Hz): parasympathetic (vagal)
 * - LF/HF ratio: sympathovagal balance
 *
 * @param hrv_freq Pointer to frequency domain results structure
 */
static void calculate_hrv_freq_domain(struct hrv_freq_domain *hrv_freq)
{
    if (!hrv_freq || !hrv_fft_initialized) {
        if (hrv_freq) hrv_freq->valid = 0;
        return;
    }

    if (ecg_state.rr_count < HRV_FREQ_MIN_RR_COUNT) {
        hrv_freq->valid = 0;
        return;
    }

    /* Step 1: Interpolate RR intervals to uniform 4 Hz sampling */
    size_t interp_samples = interpolate_rr_intervals(
        ecg_state.rr_intervals, ecg_state.rr_count, ecg_state.rr_index,
        hrv_interp_buffer, HRV_FFT_SIZE);

    if (interp_samples < HRV_FFT_SIZE / 2) {
        hrv_freq->valid = 0;
        LOG_DBG("HRV freq: insufficient interpolated samples (%u)", interp_samples);
        return;
    }

    /* Step 2: Remove mean (DC component) using CMSIS-DSP */
    float32_t mean_val;
    arm_mean_f32(hrv_interp_buffer, interp_samples, &mean_val);
    arm_offset_f32(hrv_interp_buffer, -mean_val, hrv_interp_buffer, interp_samples);

    /* Step 3: Apply Hanning window using CMSIS-DSP multiply */
    size_t fft_len = (interp_samples < HRV_FFT_SIZE) ? interp_samples : HRV_FFT_SIZE;
    arm_mult_f32(hrv_interp_buffer, hrv_hanning_window, hrv_window_buffer, fft_len);

    /* Zero-pad if necessary */
    if (fft_len < HRV_FFT_SIZE) {
        memset(&hrv_window_buffer[fft_len], 0, (HRV_FFT_SIZE - fft_len) * sizeof(float32_t));
    }

    /* Step 4: Compute FFT using CMSIS-DSP */
    arm_rfft_fast_f32(&hrv_fft_instance, hrv_window_buffer, hrv_fft_output, 0);

    /* Step 5: Compute power spectrum (magnitude squared) */
    /* FFT output is interleaved real/imag: [Re0, Im0, Re1, Im1, ...] */
    arm_cmplx_mag_squared_f32(hrv_fft_output, hrv_power_spectrum, HRV_FFT_SIZE / 2);

    /* Step 6: Normalize power spectrum. PSD normalization is
     * 2 / (N * window_power), with Hanning window power ≈ N * 0.375; the ×2
     * accounts for the one-sided spectrum. Band powers are later integrated
     * by multiplying by df = fs/N, which supplies the remaining per-Hz
     * scaling. */
    float32_t window_power = (float32_t)HRV_FFT_SIZE * 0.375f;  /* Hanning window power */
    float32_t norm_factor = 2.0f / ((float32_t)HRV_FFT_SIZE * window_power);

    for (int i = 0; i < HRV_FFT_SIZE / 2; i++) {
        hrv_power_spectrum[i] *= norm_factor;
    }

    LOG_DBG("HRV FFT: N=%d, window_power=%.1f, norm=%.6f",
            HRV_FFT_SIZE, (double)window_power, (double)norm_factor);

    /* Step 7: Integrate power in frequency bands */
    float32_t freq_resolution = HRV_INTERP_RATE / HRV_FFT_SIZE;  /* Hz per bin */
    float32_t vlf_power = 0, lf_power = 0, hf_power = 0;

    for (int i = 1; i < HRV_FFT_SIZE / 2; i++) {  /* Skip DC (i=0) */
        float32_t freq = i * freq_resolution;

        if (freq >= HRV_VLF_LOW && freq < HRV_VLF_HIGH) {
            vlf_power += hrv_power_spectrum[i];
        } else if (freq >= HRV_LF_LOW && freq < HRV_LF_HIGH) {
            lf_power += hrv_power_spectrum[i];
        } else if (freq >= HRV_HF_LOW && freq < HRV_HF_HIGH) {
            hf_power += hrv_power_spectrum[i];
        }
    }

    /* Scale by frequency resolution to get integrated power in ms² */
    vlf_power *= freq_resolution;
    lf_power *= freq_resolution;
    hf_power *= freq_resolution;

    float32_t total_power = vlf_power + lf_power + hf_power;

    /* Step 8: Fill results structure */
    hrv_freq->vlf_power_ms2 = (uint16_t)(vlf_power > 65535 ? 65535 : vlf_power);
    hrv_freq->lf_power_ms2 = (uint16_t)(lf_power > 65535 ? 65535 : lf_power);
    hrv_freq->hf_power_ms2 = (uint16_t)(hf_power > 65535 ? 65535 : hf_power);
    hrv_freq->total_power = (uint16_t)(total_power > 65535 ? 65535 : total_power);

    LOG_INF("HRV freq raw: VLF=%.1f, LF=%.1f, HF=%.1f ms²",
            (double)vlf_power, (double)lf_power, (double)hf_power);

    /* Calculate LF/HF ratio (×10 for fixed point) */
    if (hf_power > 0.001f) {
        float32_t ratio = lf_power / hf_power;
        hrv_freq->lf_hf_ratio_x10 = (uint8_t)(ratio * 10.0f > 255 ? 255 : ratio * 10.0f);
    } else {
        hrv_freq->lf_hf_ratio_x10 = 0;
    }

    /* Calculate normalized units (LF% and HF% of LF+HF power) */
    float32_t lf_hf_sum = lf_power + hf_power;
    if (lf_hf_sum > 0.001f) {
        hrv_freq->lf_nu = (uint8_t)((lf_power / lf_hf_sum) * 100.0f);
        hrv_freq->hf_nu = (uint8_t)((hf_power / lf_hf_sum) * 100.0f);
    } else {
        hrv_freq->lf_nu = 0;
        hrv_freq->hf_nu = 0;
    }

    hrv_freq->valid = 1;

    LOG_INF("HRV freq: VLF=%u, LF=%u, HF=%u, LF/HF=%u.%u, LFnu=%u%%, HFnu=%u%%",
            hrv_freq->vlf_power_ms2, hrv_freq->lf_power_ms2, hrv_freq->hf_power_ms2,
            hrv_freq->lf_hf_ratio_x10 / 10, hrv_freq->lf_hf_ratio_x10 % 10,
            hrv_freq->lf_nu, hrv_freq->hf_nu);
}

/*----------------------------------------------------------------------------*/
/* Send ECG Vitals to M7 via IPC                                              */
/*----------------------------------------------------------------------------*/

/**
 * @brief Send ECG vitals (HR, RR, HRV) to M7 using proper message type
 *
 * Uses HPI_IPC_MSG_TYPE_ECG_VITALS (0x21) with struct hpi_ipc_ecg_vitals
 */
static void send_ecg_vitals(void)
{
    struct hpi_ipc_ecg_vitals vitals = {0};

    /* Fill vitals structure */
    vitals.timestamp_ms = k_uptime_get_32();
    vitals.heart_rate = ecg_state.current_hr;
    vitals.rr_interval_ms = ecg_state.current_rr_ms;
    vitals.qrs_count = (uint16_t)(ecg_state.qrs_count & 0xFFFF);
    vitals.qrs_confidence = ecg_state.hr_confidence;
    vitals.signal_quality = ecg_state.signal_quality;

    /* HRV Time Domain metrics */
    vitals.hrv_sdnn = ecg_state.hrv.sdnn_ms;
    vitals.hrv_rmssd = ecg_state.hrv.rmssd_ms;

    /* HRV Frequency Domain metrics */
    vitals.hrv_lf_power = ecg_state.hrv_freq.lf_power_ms2;
    vitals.hrv_hf_power = ecg_state.hrv_freq.hf_power_ms2;
    vitals.hrv_lf_hf_ratio_x10 = ecg_state.hrv_freq.lf_hf_ratio_x10;
    vitals.hrv_lf_nu = ecg_state.hrv_freq.lf_nu;
    vitals.hrv_hf_nu = ecg_state.hrv_freq.hf_nu;
    vitals.hrv_freq_valid = ecg_state.hrv_freq.valid;

    /* Algorithm state */
    if (ecg_state.rr_count < 3) {
        vitals.algorithm_state = HPI_ECG_ALGO_STATE_LEARNING;
    } else if (ecg_state.hrv.valid) {
        vitals.algorithm_state = HPI_ECG_ALGO_STATE_VALID;
    } else {
        vitals.algorithm_state = HPI_ECG_ALGO_STATE_TRACKING;
    }

    /* Arrhythmia flags (basic detection) */
    vitals.arrhythmia_flags = HPI_ECG_ARRHYTHMIA_NONE;
    if (ecg_state.current_hr > 0 && ecg_state.current_hr < 60) {
        vitals.arrhythmia_flags |= HPI_ECG_ARRHYTHMIA_BRADYCARDIA;
    }
    if (ecg_state.current_hr > 100) {
        vitals.arrhythmia_flags |= HPI_ECG_ARRHYTHMIA_TACHYCARDIA;
    }

    /* Status flags */
    vitals.flags = 0;
    if (ecg_state.signal_quality < 30) {
        vitals.flags |= HPI_ECG_FLAG_LOW_AMPLITUDE;
    }

    /* Log algorithm state for debugging */
    LOG_INF("Sending ECG vitals: HR=%u, state=%u (hrv.valid=%u, freq.valid=%u)",
            vitals.heart_rate, vitals.algorithm_state,
            ecg_state.hrv.valid, ecg_state.hrv_freq.valid);

    /* Send via IPC */
    int ret = hpi_ipc_send(HPI_IPC_MSG_TYPE_ECG_VITALS, &vitals, sizeof(vitals));
    if (ret < 0) {
        LOG_ERR("Failed to send ECG vitals: %d", ret);
    }
}

/**
 * @brief Enable or disable HRV calculation
 *
 * @param enable true to enable, false to disable
 */
void algorithm_set_hrv_enabled(bool enable)
{
    hrv_enabled = enable;
    LOG_INF("HRV calculation %s", enable ? "enabled" : "disabled");
}

/**
 * @brief Check if HRV calculation is enabled
 *
 * @return true if HRV is enabled, false otherwise
 */
bool algorithm_is_hrv_enabled(void)
{
    return hrv_enabled;
}

/*----------------------------------------------------------------------------*/
/* IPC Callback - Receive ECG batches from M7                                */
/*----------------------------------------------------------------------------*/

static void ecg_batch_received_callback(enum hpi_ipc_msg_type msg_type, 
                                       const void *data, size_t len, void *user_data)
{
    ARG_UNUSED(msg_type);
    ARG_UNUSED(user_data);
    
    if (len != sizeof(struct hpi_ipc_ecg_raw_batch)) {
        LOG_ERR("Invalid ECG batch size: %u (expected %u)", 
                len, (unsigned int)sizeof(struct hpi_ipc_ecg_raw_batch));
        return;
    }
    
    /* Queue batch for processing by algorithm thread */
    int ret = k_msgq_put(&q_ecg_algorithm, data, K_NO_WAIT);
    if (ret != 0) {
        LOG_WRN("ECG algorithm queue full, dropping batch");
        algo_stats.algorithm_overruns++;
    }
}

/* Register callback with IPC module during initialization */
int algorithm_register_ipc_callbacks(void)
{
    int ret;

    /* Register for ECG batches (type 0x20) */
    ret = hpi_ipc_register_callback(HPI_IPC_MSG_TYPE_ECG_RAW,
                                     ecg_batch_received_callback,
                                     NULL);
    if (ret < 0) {
        LOG_ERR("Failed to register ECG callback: %d", ret);
        return ret;
    }

    LOG_INF("Algorithm IPC callbacks registered");
    return 0;
}

