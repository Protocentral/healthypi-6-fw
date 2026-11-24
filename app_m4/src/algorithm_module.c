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
#include <zephyr/toolchain.h>
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
#define RR_HISTORY_SIZE         256    /* Sized for frequency-domain HRV */

/* Heart rate: the median of the newest HR_WINDOW_RR intervals (fewer while
 * fewer exist), reported once HR_MIN_RR_COUNT intervals = 4 detected beats
 * are in. The RR history above is untouched by this: HRV still reads all of
 * it. The rate used to be the mean of the whole 256-interval history, which
 * took 236 beats (~142 s) to follow a step from 60 to 100 BPM; the median of
 * 8 follows it in 5 beats and is unmoved by a single ectopic, missed or extra
 * beat, where a mean of 8 settles in 8 and swings by up to 8 BPM on one bad
 * interval. Numbers: app_m4/tests/hrv/test_hrv.py. */
#define HR_WINDOW_RR            8
#define HR_MIN_RR_COUNT         3

/* A heart rate expires when no RR interval has been accepted for this many
 * ECG samples (5 s at 500 Hz). Counted in samples, so it is the signal's time,
 * not the time a batch happened to be processed.
 *
 * Why it is needed: ECG_VITALS is re-sent after every HRV pass (5 s, and 60 s
 * for the spectrum) whether or not a beat arrived, and the M7 treats an ECG
 * rate as fresh for 5 s from each message's ARRIVAL. Without an expiry here a
 * rate survived asystole, a flat line or a detector that stopped finding QRS
 * for as long as the electrodes stayed on.
 *
 * Why 5 s: the longest interval the gate accepts is 2000 ms (30 BPM), so a
 * steady 30 BPM refreshes the rate every 2 s, and a 60 BPM rhythm with one
 * missed beat (one 2000 ms interval) never comes close. Below ~36 BPM a missed
 * beat leaves a 3-interval gap between accepted intervals and does expire the
 * rate: at that rate a dropped beat is enough doubt to withdraw the number. It
 * is also the M7's own HR_STALE_MS, so with or without HRV re-sends a rate is
 * never shown for more than ~5 s after the last interval that supports it.
 *
 * Keyed on the last ACCEPTED interval, not the last detection: the rate is made
 * of accepted intervals only, so its age is theirs. A rhythm below 30 BPM (every
 * interval rejected) or a detector firing faster than the 300 ms gate therefore
 * expires the rate too, rather than leaving the last good one on screen. */
#define HR_STALE_SAMPLES        (5U * HPI_ECG_M4_RATE_HZ)

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

/* QRS detection: Pan-Tompkins (Pan & Tompkins, IEEE TBME 32(3), 1985), run
 * sample by sample with all filter state carried across batches.
 *
 * Replaces the 1.0.0 detector, which never produced a heart rate on v5. That
 * one filtered each 16-sample batch on its own: its 5-point derivative clamped
 * at the batch edges, its "integration" was 8 samples inside the batch, its
 * "5-15 Hz band-pass" was a bare second difference and its refractory period
 * was wall-clock. Fed ECG carrying a constant 250 Hz alternation (the ADS1294R
 * CONFIG1 fault, see drivers/sensor/ads129xx), the clamped edges turned the
 * alternation into a spike in EVERY batch. The noise estimate started at 0, so
 * the learning threshold was 10 and the first five spikes were learnt as beats;
 * from then on every batch crossed the threshold, the noise estimate was never
 * updated again, and a "beat" fired each time the 250 ms refractory ran out --
 * 256 ms intervals, every one rejected by the 300 ms RR gate. No intervals, no
 * rate, on a clean 80 BPM trace. Host regression: app_m4/tests/hrv/test_qrs.py.
 *
 * Pipeline, at HPI_ECG_M4_RATE_HZ (500 Hz):
 *   band-pass 5-15 Hz  two Butterworth biquads (HP 5 Hz, LP 15 Hz); the LP's
 *                      double zero at Nyquist nulls a 250 Hz alternation
 *                      outright, and 50/60 Hz mains is down 21/25 dB
 *   derivative         2x[n] + x[n-1] - x[n-3] - 2x[n-4]   (PT's, unscaled)
 *   square
 *   integrate          moving sum over 150 ms (75 samples), exact in int64
 *   peaks              a local maximum of the integral, confirmed once it has
 *                      not been exceeded for QRS_PEAK_CONFIRM samples
 *   classify           QRS if above THR1 = NPKI + (SPKI - NPKI)/4, outside the
 *                      200 ms refractory, and -- within 360 ms of the last
 *                      QRS -- with at least half its slope (else a T wave);
 *                      otherwise noise. SPKI/NPKI track each class (1/8).
 *   searchback         no QRS for 1.66 x the mean RR: take the largest noise
 *                      peak since the last QRS if it clears THR1 / 2
 *   (re)learning       thresholds start from the first 2 s (after 0.5 s for
 *                      the filters to settle): SPKI = max/3, NPKI = mean/2.
 *                      Learning restarts after QRS_RELEARN_SAMPLES without a
 *                      QRS, so a gain change or an artefact that inflated
 *                      SPKI cannot lock detection out for good.
 *
 * Positions are absolute sample indices: RR intervals come out at the ADC's
 * 2 ms resolution, free of IPC and scheduling jitter (see track_rhythm()). A
 * QRS is reported at its dominant lobe -- the band-passed extremum, of the
 * polarity learnt with the thresholds, in the integration window behind the
 * integrator peak. Not at the integrator peak: it sits on a plateau as wide as
 * the window minus the QRS and wanders across it with noise (5-9 samples sd
 * on test signals, an HRV-sized error). And not at the largest |band-passed|
 * sample: R and S lobes are often of similar size, and flipping between them
 * moves the point 30 ms from one beat to the next. */
#define QRS_FS                  HPI_ECG_M4_RATE_HZ
#define QRS_MS(ms)              ((uint32_t)(((ms) * QRS_FS) / 1000U))
#define QRS_MWI_LEN             QRS_MS(150)   /* integration window */
#define QRS_PEAK_CONFIRM        QRS_MS(100)   /* peak must hold this long */
#define QRS_REFRACTORY          QRS_MS(200)
#define QRS_TWAVE_WINDOW        QRS_MS(360)
#define QRS_SETTLE_SAMPLES      QRS_MS(500)   /* filter start-up, ignored */
#define QRS_LEARN_SAMPLES       QRS_MS(2000)
#define QRS_RELEARN_SAMPLES     QRS_MS(2500)  /* no QRS this long -> relearn */
#define QRS_SLOPE_HIST          128           /* power of 2, >= QRS_MWI_LEN */

/* Absolute floor for a QRS, in integrator units (squared µV-per-sample
 * derivative, averaged over the window). Without one a flat line learns
 * thresholds of ~0 and calls its own noise a rhythm. This is the integrator
 * peak of a ~100 µV QRS (calibrated in test_qrs.py, where a 1 mV synthetic QRS
 * peaks near 1.4e5 and the inputs-open noise of a v5 unit sits below 100); a
 * real R wave on any lead is several times larger. */
#define QRS_MIN_PEAK            1500

/* Band-pass coefficients for fs = 500 Hz: RBJ-cookbook Butterworth biquads
 * (Q = 1/sqrt(2)), normalised so a0 = 1. Regenerate for another rate with
 * test_qrs.py --coefficients. Response of the pair: -3 dB at 5 and 15 Hz,
 * -28 dB at 1 Hz, -21.5 dB at 50 Hz, -24.9 dB at 60 Hz, zero at 250 Hz. */
BUILD_ASSERT(HPI_ECG_M4_RATE_HZ == 500, "QRS band-pass coefficients are for 500 Hz");
static const float qrs_hp_b[3] = {9.565432256e-01f, -1.913086451e+00f, 9.565432256e-01f};
static const float qrs_hp_a[2] = {-1.911197067e+00f, 9.149758348e-01f};
static const float qrs_lp_b[3] = {7.820208033e-03f, 1.564041607e-02f, 7.820208033e-03f};
static const float qrs_lp_a[2] = {-1.734725769e+00f, 7.660066009e-01f};

enum qrs_phase {
    QRS_PHASE_SETTLE,   /* filters starting up: nothing classified */
    QRS_PHASE_LEARN,    /* collecting the integrator's max and mean */
    QRS_PHASE_RUN,      /* detecting */
};

struct qrs_detector {
    uint64_t n;                         /* samples fed so far */

    /* Band-pass (transposed direct form II, one state pair per biquad) */
    float hp_z1, hp_z2, lp_z1, lp_z2;
    bool primed;                        /* HP state set from the first sample */

    /* Derivative input history: band-passed x[n-1..n-4] */
    float bp_hist[4];

    /* |derivative| and band-passed history over the integration window: the
     * slope of a peak (T-wave test) and the R-wave position */
    int32_t slope_hist[QRS_SLOPE_HIST];
    float bp_hist_w[QRS_SLOPE_HIST];
    float polarity;                     /* +1 / -1: sign of the dominant QRS lobe */
    float learn_bp_pos, learn_bp_neg;   /* largest excursions while learning */

    /* Moving-window integrator */
    int64_t sq_ring[QRS_MWI_LEN];
    int64_t sq_sum;
    uint32_t ring_pos;

    /* Peak picking on the integrator output */
    int64_t cand_val;
    uint64_t cand_idx;                  /* integrator peak */
    uint64_t cand_fid;                  /* its R wave (fiducial point) */
    int32_t cand_slope;
    bool cand_rising;                   /* candidate is a real local max */

    /* Thresholds */
    enum qrs_phase phase;
    uint64_t phase_start;
    int64_t learn_max;
    int64_t learn_sum;
    uint32_t learn_count;
    int64_t spki;                       /* signal peak estimate */
    int64_t npki;                       /* noise peak estimate */
    int64_t thr1;

    /* Last QRS */
    bool have_qrs;
    uint64_t last_qrs;
    int32_t last_qrs_slope;
    uint32_t rr_avg;                    /* mean QRS-QRS distance, samples */

    /* Searchback: the largest noise peak since the last QRS */
    int64_t sb_val;
    uint64_t sb_idx;
    int32_t sb_slope;
};

static struct qrs_detector qrs;

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
    size_t sample_count;                   /* Total samples processed */

    /* QRS detection state (the detector's own state is in `qrs`) */
    int32_t detection_threshold;          /* THR1, saturated to int32, for debug reads */
    uint64_t last_qrs_sample;             /* Absolute sample index of last QRS (RR) */

    /* RR interval tracking (expanded for HRV) */
    uint16_t rr_intervals[RR_HISTORY_SIZE];  /* Last 64 RR intervals */
    size_t rr_count;                      /* Number of valid intervals */
    size_t rr_index;                      /* Circular buffer index */
    size_t rr_total_count;                /* Total RR intervals ever recorded */

    /* Heart-rate window: how many of the newest rr_intervals belong to the
     * current run of beats (0..HR_WINDOW_RR). Reset to 0 when the rate expires,
     * so a rate after a gap is built from post-gap intervals only and the
     * HR_MIN_RR_COUNT start-up rule applies again. HRV ignores it. */
    size_t hr_rr_count;
    uint64_t last_rr_sample;              /* Sample index closing the newest accepted RR */
    uint16_t last_sent_hr;                /* heart_rate of the last ECG_VITALS sent */

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
static void qrs_detector_reset(void);
static bool qrs_feed(int32_t x, uint64_t *qrs_sample);
static uint8_t assess_signal_quality(const int32_t *samples, size_t count);
static uint16_t rr_ms_from_sample_delta(uint64_t d);
static void update_rr_interval(uint16_t interval_ms);
static void update_heart_rate(struct ecg_algorithm_results *results);
static void expire_stale_heart_rate(void);
static void track_rhythm(bool qrs_found, uint64_t qrs_sample,
                         struct ecg_algorithm_results *results);
static void ecg_report(const struct ecg_algorithm_results *results,
                       uint32_t now_ms);
static void calculate_hrv_time_domain(struct hrv_time_domain *hrv);
static void calculate_hrv_freq_domain(struct hrv_freq_domain *hrv_freq);
static void send_ecg_vitals(void);
static void send_beat_notify(const struct ecg_algorithm_results *results);

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
    qrs_detector_reset();

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
    /* Clear detector state but keep configuration */
    ecg_state.sample_count = 0;
    ecg_state.detection_threshold = 0;
    ecg_state.last_qrs_sample = 0;
    qrs_detector_reset();

    memset(ecg_state.rr_intervals, 0, sizeof(ecg_state.rr_intervals));
    ecg_state.rr_count = 0;
    ecg_state.rr_index = 0;
    ecg_state.hr_rr_count = 0;
    ecg_state.last_rr_sample = 0;
    ecg_state.current_hr = 0;
    ecg_state.hr_confidence = 0;
    ecg_state.current_rr_ms = 0;
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
        
        ecg_report(&results, k_uptime_get_32());

        /* Periodic status logging - simplified QRS counter only (every 500 batches = ~16 seconds) */
        if (algo_stats.ecg_batches_processed % 500 == 0) {
            LOG_INF("QRS: %u detected, HR=%u bpm",
                    algo_stats.qrs_detections_total,
                    ecg_state.current_hr);
        }
    }
}

/* After each batch: run the HRV passes when due and decide whether the M7 gets
 * an ECG_VITALS (and a BEAT_NOTIFY). Pulled out of the thread loop, unchanged
 * apart from the hr_changed rule below, so the host test can drive it. */
static void ecg_report(const struct ecg_algorithm_results *results,
                       uint32_t now_ms)
{
    /* Calculate HRV periodically when enabled and we have enough data */
    uint32_t current_time = now_ms;
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
     * - When the reported HR changes, INCLUDING to 0. A rate that expires
     *   (HR_STALE_SAMPLES) is sent at once as 0 / confidence 0; this used
     *   to require heart_rate > 0, so the withdrawal would only have gone
     *   out with the next HRV pass, or never with HRV off.
     * - Immediately after HRV calculation (to send VALID state). These
     *   re-sends carry whatever current_hr is, which is why it must expire
     *   on the M4: they arrive every 5 s and would otherwise keep the M7's
     *   5 s freshness window open on the last rate indefinitely. */
    bool hr_changed = (results->heart_rate != ecg_state.last_sent_hr);

    if (results->qrs_detected) {
        send_beat_notify(results);
    }
    if (results->qrs_detected || hr_changed || hrv_just_calculated) {
        send_ecg_vitals();
        if (hr_changed) {
            algo_stats.hr_updates_sent++;
            ecg_state.last_sent_hr = results->heart_rate;
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
    
    /* Run the detector sample by sample. It confirms a QRS up to
     * QRS_PEAK_CONFIRM samples (or, by searchback, ~1.66 RR) after it happened
     * and reports its absolute sample index, so the QRS may lie in an earlier
     * batch; track_rhythm() works in absolute indices and does not care. The
     * 200 ms refractory allows at most one QRS per 32 ms batch. */
    bool qrs_found = false;
    uint64_t qrs_sample = 0;

    for (size_t i = 0; i < ECG_BATCH_SIZE; i++) {
        uint64_t s;

        if (qrs_feed(samples[i], &s)) {
            qrs_found = true;
            qrs_sample = s;
        }
        ecg_state.sample_count++;
    }
    ecg_state.detection_threshold =
        (qrs.thr1 > INT32_MAX) ? INT32_MAX : (int32_t)qrs.thr1;

    /* Build results */
    memset(results, 0, sizeof(*results));
    results->timestamp = k_uptime_get_32();
    results->signal_quality = quality;

    track_rhythm(qrs_found, qrs_sample, results);

    ecg_state.batches_processed++;

    return 0;
}

/* Everything ecg_process_batch() does once the detector has run: RR interval,
 * rate expiry, heart rate. qrs_sample is the absolute sample index of the QRS
 * (ignored when !qrs_found); ecg_state.sample_count must already include this
 * batch. Separate so the host test can drive it with beats at known samples. */
static void track_rhythm(bool qrs_found, uint64_t qrs_sample,
                         struct ecg_algorithm_results *results)
{
    if (qrs_found) {
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
         * qrs_sample is an absolute sample index (see ecg_process_batch()). At
         * 500 Hz that is 2 ms resolution, set by the ADC, and free of
         * scheduling noise.
         *
         * The interval that spans a pause long enough to expire the rate is
         * > HR_STALE_SAMPLES (5 s), far outside the 2000 ms gate, so it is
         * rejected here and enters neither the HR window nor HRV. */
        uint16_t rr_ms = 0;

        if (ecg_state.last_qrs_sample > 0) {
            rr_ms = rr_ms_from_sample_delta(qrs_sample - ecg_state.last_qrs_sample);

            if (rr_ms != 0) {
                update_rr_interval(rr_ms);
                results->rr_interval_ms = rr_ms;
                ecg_state.current_rr_ms = rr_ms;
                ecg_state.last_rr_sample = qrs_sample;
            }
        }

        ecg_state.last_qrs_sample = qrs_sample;
    }

    expire_stale_heart_rate();
    update_heart_rate(results);
}

/* Withdraw the heart rate when no RR interval has been accepted for
 * HR_STALE_SAMPLES (see its definition for the why and the why-5-s).
 *
 * On expiry:
 *  - the rate reads 0 with confidence 0 -- the "no rate" the M4 has always
 *    reported before its first 3 intervals, and what the M7 treats as no ECG
 *    rate (falls back to a fresh PPG rate, else none);
 *  - the HR window restarts (hr_rr_count = 0), so after the pause the first
 *    rate again needs HR_MIN_RR_COUNT new intervals and is the median of those
 *    alone: a pre-pause interval in the window would be a number about a
 *    rhythm that may no longer exist;
 *  - current_rr_ms is cleared: "latest RR" is as stale as the rate;
 *  - brady/tachy clear by themselves, being derived from current_hr in
 *    send_ecg_vitals(); on recovery they follow the new rate only.
 * The RR history (rr_intervals/rr_count) and HRV are left alone: HRV is its own
 * 5 s / 60 s pass over the full history, and the M7 blanks HRV whenever it has
 * no ECG rate. The QRS detector is not touched either. */
static void expire_stale_heart_rate(void)
{
    if (ecg_state.hr_rr_count == 0) {
        return;
    }
    if ((uint64_t)ecg_state.sample_count - ecg_state.last_rr_sample <= HR_STALE_SAMPLES) {
        return;
    }

    LOG_WRN("ECG rate expired: no RR interval for %u samples (HR was %u)",
            (uint32_t)((uint64_t)ecg_state.sample_count - ecg_state.last_rr_sample),
            ecg_state.current_hr);

    ecg_state.hr_rr_count = 0;
    ecg_state.current_hr = 0;
    ecg_state.hr_confidence = 0;
    ecg_state.current_rr_ms = 0;
}

/* Heart rate from the RR buffer into results and ecg_state.current_hr (which
 * ECG_VITALS and BEAT_NOTIFY carry to the M7). Pulled out of
 * ecg_process_batch() so the host test can drive the real start-up rule. */
static void update_heart_rate(struct ecg_algorithm_results *results)
{
    /* hr_rr_count, not rr_count: after an expiry the window holds only the
     * intervals since, so the start-up rule and the median both see the new
     * run of beats alone (calculate_heart_rate() reads the newest `count`). */
    if (ecg_state.hr_rr_count >= HR_MIN_RR_COUNT) {
        results->heart_rate = calculate_heart_rate(ecg_state.rr_intervals,
                                                   ecg_state.hr_rr_count,
                                                   ecg_state.rr_index);
        ecg_state.current_hr = results->heart_rate;

        /* Confidence from how much history there is (unchanged) */
        results->confidence = (ecg_state.hr_rr_count >= 8) ? 90 :
                             (ecg_state.hr_rr_count >= 5) ? 70 : 50;
        ecg_state.hr_confidence = results->confidence;
    } else {
        /* No rate: before the first HR_MIN_RR_COUNT intervals, and again after
         * an expiry until as many new ones. This used to report the "last
         * known" current_hr, which is 0 in both cases now that an expiry
         * clears it; say so rather than rely on it. */
        results->heart_rate = 0;
        results->confidence = 0;
        ecg_state.current_hr = 0;
        ecg_state.hr_confidence = 0;
    }
}

/*----------------------------------------------------------------------------*/
/* QRS Detection (Pan-Tompkins, streaming; see the QRS_* constants above)     */
/*----------------------------------------------------------------------------*/

static void qrs_detector_reset(void)
{
    memset(&qrs, 0, sizeof(qrs));
    qrs.phase = QRS_PHASE_SETTLE;
    qrs.polarity = 1.0f;
}

static void qrs_start_learning(uint64_t now)
{
    qrs.phase = QRS_PHASE_LEARN;
    qrs.phase_start = now;
    qrs.learn_max = 0;
    qrs.learn_sum = 0;
    qrs.learn_count = 0;
    qrs.learn_bp_pos = 0.0f;
    qrs.learn_bp_neg = 0.0f;
}

static void qrs_update_thr(void)
{
    qrs.thr1 = qrs.npki + (qrs.spki - qrs.npki) / 4;
    if (qrs.thr1 < QRS_MIN_PEAK) {
        qrs.thr1 = QRS_MIN_PEAK;
    }
}

/* Sample n has just become the peak candidate: over the integration window
 * ending at n, find the steepest slope (largest |derivative|, for the T-wave
 * test) and the fiducial point (band-passed extremum of the learnt polarity).
 * The window is still in the history at this moment. */
static void qrs_candidate_window(uint64_t n)
{
    int32_t m = 0;
    float best = 0.0f;
    bool have = false;
    uint64_t fid = n;

    for (uint32_t i = 0; i < QRS_MWI_LEN && i <= n; i++) {
        uint32_t k = (uint32_t)((n - i) & (QRS_SLOPE_HIST - 1U));

        if (qrs.slope_hist[k] > m) {
            m = qrs.slope_hist[k];
        }
        float v = qrs.polarity * qrs.bp_hist_w[k];

        if (!have || v > best) {
            best = v;
            fid = n - i;
            have = true;
        }
    }
    qrs.cand_slope = m;
    qrs.cand_fid = fid;
}

/* A confirmed QRS at absolute sample `idx`. */
static void qrs_accept(uint64_t idx, int32_t slope)
{
    if (qrs.have_qrs) {
        uint32_t rr = (uint32_t)(idx - qrs.last_qrs);

        /* Searchback uses the mean RR; only plausible intervals feed it
         * (the same 300-2000 ms window the HR gate applies). */
        if (rr >= QRS_MS(300) && rr <= QRS_MS(2000)) {
            qrs.rr_avg = (qrs.rr_avg == 0) ? rr : (qrs.rr_avg * 7U + rr) / 8U;
        }
    }
    qrs.have_qrs = true;
    qrs.last_qrs = idx;
    qrs.last_qrs_slope = slope;
    qrs.sb_val = 0;
}

/* Distance in samples from the last QRS to `idx`; 0 if `idx` is not after it.
 * A peak's fiducial point is searched for up to QRS_MWI_LEN samples behind the
 * peak, so it can land on or before the previous QRS; the unsigned difference
 * would then wrap and pass every "too soon" test below. */
static uint64_t qrs_since_last(uint64_t idx)
{
    return (qrs.have_qrs && idx > qrs.last_qrs) ? idx - qrs.last_qrs :
           (qrs.have_qrs ? 0U : UINT64_MAX);
}

/* Classify one confirmed integrator peak. Returns true if it is a QRS. */
static bool qrs_classify(int64_t val, uint64_t idx, int32_t slope)
{
    if (qrs.phase != QRS_PHASE_RUN) {
        return false;
    }

    const uint64_t since = qrs_since_last(idx);

    if (since < QRS_REFRACTORY) {
        return false;   /* physiologically impossible: ignore entirely */
    }

    if (val > qrs.thr1) {
        bool twave = since < QRS_TWAVE_WINDOW &&
                     slope < qrs.last_qrs_slope / 2;

        if (!twave) {
            qrs.spki = (val + 7 * qrs.spki) / 8;
            qrs_update_thr();
            qrs_accept(idx, slope);
            return true;
        }
    }

    /* Noise (or a T wave). Remember the largest since the last QRS that is
     * clear of the T-wave window, for searchback. */
    qrs.npki = (val + 7 * qrs.npki) / 8;
    qrs_update_thr();
    if (since >= QRS_TWAVE_WINDOW && val > qrs.sb_val) {
        qrs.sb_val = val;
        qrs.sb_idx = idx;
        qrs.sb_slope = slope;
    }
    return false;
}

/* Feed one raw ECG sample (µV). Returns true when a QRS is confirmed, with its
 * absolute sample index in *qrs_sample. The index of the first sample fed after
 * a reset is 0, matching ecg_state.sample_count. */
static bool qrs_feed(int32_t x_in, uint64_t *qrs_sample)
{
    const uint64_t n = qrs.n++;
    const float x = (float)x_in;
    bool found = false;

    /* Band-pass. Prime the high-pass with the first sample as a steady DC
     * level, so an electrode offset of tens of mV does not ring through. */
    if (!qrs.primed) {
        /* Steady state for a constant input x (the high-pass output is 0):
         *   z2 = b2*x,  z1 = b1*x + z2 = (b1 + b2)*x */
        qrs.hp_z2 = qrs_hp_b[2] * x;
        qrs.hp_z1 = (qrs_hp_b[1] + qrs_hp_b[2]) * x;
        qrs.primed = true;
    }
    float hp = qrs_hp_b[0] * x + qrs.hp_z1;
    qrs.hp_z1 = qrs_hp_b[1] * x - qrs_hp_a[0] * hp + qrs.hp_z2;
    qrs.hp_z2 = qrs_hp_b[2] * x - qrs_hp_a[1] * hp;

    float bp = qrs_lp_b[0] * hp + qrs.lp_z1;
    qrs.lp_z1 = qrs_lp_b[1] * hp - qrs_lp_a[0] * bp + qrs.lp_z2;
    qrs.lp_z2 = qrs_lp_b[2] * hp - qrs_lp_a[1] * bp;

    /* Derivative (Pan-Tompkins' five-point, without its 1/8: the extra
     * resolution matters once it is rounded to an integer). */
    float d = 2.0f * bp + qrs.bp_hist[0] - qrs.bp_hist[2] - 2.0f * qrs.bp_hist[3];
    qrs.bp_hist[3] = qrs.bp_hist[2];
    qrs.bp_hist[2] = qrs.bp_hist[1];
    qrs.bp_hist[1] = qrs.bp_hist[0];
    qrs.bp_hist[0] = bp;

    /* Clamp before squaring: 2^24 squared still fits in int64 with room for
     * a 75-sample sum. A real derivative is under ~1e4. */
    int32_t di;
    if (d > 16777215.0f) {
        di = 16777215;
    } else if (d < -16777215.0f) {
        di = -16777215;
    } else {
        di = (int32_t)d;
    }
    qrs.slope_hist[n & (QRS_SLOPE_HIST - 1)] = (di < 0) ? -di : di;
    qrs.bp_hist_w[n & (QRS_SLOPE_HIST - 1)] = bp;

    /* Moving-window integration, exact */
    int64_t sq = (int64_t)di * di;
    qrs.sq_sum += sq - qrs.sq_ring[qrs.ring_pos];
    qrs.sq_ring[qrs.ring_pos] = sq;
    qrs.ring_pos = (qrs.ring_pos + 1U) % QRS_MWI_LEN;
    const int64_t mwi = qrs.sq_sum / QRS_MWI_LEN;

    /* Phases */
    if (qrs.phase == QRS_PHASE_SETTLE) {
        if (n + 1U >= QRS_SETTLE_SAMPLES) {
            qrs_start_learning(n + 1U);
        }
    } else if (qrs.phase == QRS_PHASE_LEARN) {
        if (mwi > qrs.learn_max) {
            qrs.learn_max = mwi;
        }
        qrs.learn_sum += mwi;
        qrs.learn_count++;
        if (bp > qrs.learn_bp_pos) {
            qrs.learn_bp_pos = bp;
        }
        if (-bp > qrs.learn_bp_neg) {
            qrs.learn_bp_neg = -bp;
        }
        if (qrs.learn_count >= QRS_LEARN_SAMPLES) {
            if (qrs.learn_max / 3 < QRS_MIN_PEAK) {
                /* Nothing QRS-sized in 2 s (flat line, leads off): keep
                 * learning rather than adopt thresholds of ~0. */
                qrs_start_learning(n + 1U);
            } else {
                qrs.polarity = (qrs.learn_bp_neg > qrs.learn_bp_pos) ? -1.0f : 1.0f;
                qrs.spki = qrs.learn_max / 3;
                qrs.npki = qrs.learn_sum / (int64_t)qrs.learn_count / 2;
                qrs_update_thr();
                qrs.phase = QRS_PHASE_RUN;
                qrs.phase_start = n + 1U;
                qrs.sb_val = 0;
                LOG_INF("QRS thresholds learnt: spki=%d npki=%d thr=%d polarity=%d",
                        (int)qrs.spki, (int)qrs.npki, (int)qrs.thr1,
                        (int)qrs.polarity);
            }
        }
    }

    /* Peak picking: follow the integrator down to a valley, then up to a
     * maximum; confirm the maximum once QRS_PEAK_CONFIRM samples have passed
     * without a larger value. */
    if (!qrs.cand_rising) {
        if (mwi < qrs.cand_val || n == 0) {
            qrs.cand_val = mwi;
            qrs.cand_idx = n;
        } else if (mwi > qrs.cand_val) {
            qrs.cand_rising = true;
            qrs.cand_val = mwi;
            qrs.cand_idx = n;
            qrs_candidate_window(n);
        }
    } else if (mwi > qrs.cand_val) {
        qrs.cand_val = mwi;
        qrs.cand_idx = n;
        qrs_candidate_window(n);
    } else if (n - qrs.cand_idx >= QRS_PEAK_CONFIRM) {
        if (qrs_classify(qrs.cand_val, qrs.cand_fid, qrs.cand_slope)) {
            *qrs_sample = qrs.cand_fid;
            found = true;
        }
        qrs.cand_rising = false;
        qrs.cand_val = mwi;
        qrs.cand_idx = n;
    }

    if (qrs.phase != QRS_PHASE_RUN || found) {
        return found;
    }

    /* Searchback: overdue by 1.66 mean RR -> the largest noise peak since the
     * last QRS, if it clears THR1 / 2, was a QRS. */
    if (qrs.have_qrs && qrs.rr_avg != 0 && qrs.sb_val > qrs.thr1 / 2 &&
        n - qrs.last_qrs > (uint64_t)qrs.rr_avg * 166U / 100U) {
        uint64_t idx = qrs.sb_idx;

        qrs.spki = (qrs.sb_val + 3 * qrs.spki) / 4;
        qrs_update_thr();
        qrs_accept(idx, qrs.sb_slope);
        *qrs_sample = idx;
        return true;
    }

    /* Nothing for QRS_RELEARN_SAMPLES (from the last QRS, or from the start
     * of this run): start again from fresh thresholds. */
    uint64_t since = qrs.have_qrs && qrs.last_qrs > qrs.phase_start ?
                     qrs.last_qrs : qrs.phase_start;
    if (n > since && n - since > QRS_RELEARN_SAMPLES) {
        LOG_WRN("QRS: none for %u ms, relearning thresholds",
                (unsigned)((n - since) * 1000U / QRS_FS));
        qrs_start_learning(n + 1U);
    }

    return false;
}

static uint8_t assess_signal_quality(const int32_t *samples, size_t count)
{
    /* Simple signal quality assessment based on:
     * - Signal amplitude (should be in reasonable range)
     * - Variability (too flat = poor quality)
     */
    
    int32_t min_val = samples[0];
    int32_t max_val = samples[0];
    
    for (size_t i = 0; i < count; i++) {
        if (samples[i] < min_val) min_val = samples[i];
        if (samples[i] > max_val) max_val = samples[i];
    }
    
    int32_t amplitude = max_val - min_val;
    
    /* Quality based on amplitude (expected range: 100-5000 for good signal).
     * A flat line (poor contact) is the amplitude <= 100 case below. */
    uint8_t quality = 0;
    if (amplitude > 100 && amplitude < 10000) {
        quality = 80;  /* Good amplitude */
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

/* RR interval in ms from the distance, in samples, between two QRS detections.
 * Returns 0 when it falls outside the physiological range accepted for HR and
 * HRV (300-2000 ms = 30-200 BPM). Pulled out of ecg_process_batch() unchanged
 * so the host test (app_m4/tests/hrv) can exercise the gate directly. */
static uint16_t rr_ms_from_sample_delta(uint64_t d)
{
    uint32_t ms = (uint32_t)((d * 1000U) / HPI_ECG_M4_RATE_HZ);

    if (ms >= 300U && ms <= 2000U) {
        return (uint16_t)ms;
    }
    return 0;
}

static void update_rr_interval(uint16_t interval_ms)
{
    /* Add to circular buffer */
    ecg_state.rr_intervals[ecg_state.rr_index] = interval_ms;
    ecg_state.rr_index = (ecg_state.rr_index + 1) % RR_HISTORY_SIZE;
    
    if (ecg_state.rr_count < RR_HISTORY_SIZE) {
        ecg_state.rr_count++;
    }
    if (ecg_state.hr_rr_count < HR_WINDOW_RR) {
        ecg_state.hr_rr_count++;
    }
}

uint16_t calculate_heart_rate(const uint16_t *rr_intervals, size_t count,
                              size_t next_index)
{
    if (!rr_intervals || count == 0 || count > RR_HISTORY_SIZE) {
        return 0;
    }

    /* The newest min(count, HR_WINDOW_RR) intervals, walking back from the
     * slot before next_index (the buffer is circular), insertion-sorted. */
    uint16_t win[HR_WINDOW_RR];
    size_t n = (count < HR_WINDOW_RR) ? count : HR_WINDOW_RR;

    for (size_t i = 0; i < n; i++) {
        size_t idx = (next_index + RR_HISTORY_SIZE - 1 - i) % RR_HISTORY_SIZE;
        uint16_t v = rr_intervals[idx];
        size_t j = i;

        while (j > 0 && win[j - 1] > v) {
            win[j] = win[j - 1];
            j--;
        }
        win[j] = v;
    }

    /* Median; for an even count the integer mean of the middle two. A median
     * rather than a mean so one premature, missed or extra beat does not move
     * the reading at all (a mean of 8 moves by up to 8 BPM). */
    uint32_t med_rr_ms = (n & 1U) ? win[n / 2]
                                  : ((uint32_t)win[n / 2 - 1] + win[n / 2]) / 2U;

    /* Convert to BPM: HR = 60000 / RR_ms */
    if (med_rr_ms == 0) {
        return 0;
    }

    uint16_t hr = 60000 / med_rr_ms;

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
 * for FFT processing. Uses simple linear interpolation. If the intervals span
 * more than output_len samples, the most recent output_len are returned.
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
    size_t total_samples = (size_t)(total_time_sec * HRV_INTERP_RATE);
    size_t num_samples = total_samples;
    if (num_samples > output_len) {
        num_samples = output_len;
    }
    if (num_samples < 8) {
        return 0;  /* Not enough data */
    }

    /* When the buffer spans more than output_len samples, keep the NEWEST
     * ones: start the grid (total_samples - num_samples) samples in, so the
     * output is the tail of the full interpolation, on the same grid. Starting
     * at t = 0 here used to analyse the oldest 64 s of up to ~4 min of RR. */
    float32_t dt = 1.0f / HRV_INTERP_RATE;
    float32_t t_start = (float32_t)(total_samples - num_samples) * dt;

    /* Build cumulative time array and RR values */
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
    for (size_t i = 0; i < num_samples; i++) {
        float32_t t = t_start + i * dt;

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

    /* Step 3: Apply a Hann window of the segment's OWN length L and keep its
     * energy Σw² for the normalisation in Step 6.
     *
     * With 64 s of data (L = 256) that is the precomputed table. With 32-64 s
     * (128 <= L < 256) a window of length L is computed here; the segment is
     * then zero-padded to 256 for the FFT. This used to apply the first L
     * points of the 256-point window -- a taper that falls to zero at the old
     * end only and peaks near the new end, so it weighted the record unevenly
     * in time -- and normalise by the full window's energy, which read 20-50 %
     * low. Computing L cosines costs well under 0.1 ms once a minute, and needs
     * no extra buffer. */
    size_t fft_len = (interp_samples < HRV_FFT_SIZE) ? interp_samples : HRV_FFT_SIZE;
    float32_t window_power = 0.0f;  /* Σw² of the window actually applied */

    for (size_t n = 0; n < fft_len; n++) {
        float32_t w = (fft_len == HRV_FFT_SIZE)
            ? hrv_hanning_window[n]
            : 0.5f * (1.0f - arm_cos_f32(2.0f * PI * (float32_t)n /
                                         (float32_t)(fft_len - 1)));
        hrv_window_buffer[n] = hrv_interp_buffer[n] * w;
        window_power += w * w;
    }

    /* Zero-pad if necessary */
    if (fft_len < HRV_FFT_SIZE) {
        memset(&hrv_window_buffer[fft_len], 0, (HRV_FFT_SIZE - fft_len) * sizeof(float32_t));
    }

    /* Step 4: Compute FFT using CMSIS-DSP */
    arm_rfft_fast_f32(&hrv_fft_instance, hrv_window_buffer, hrv_fft_output, 0);

    /* Step 5: Compute power spectrum (magnitude squared) */
    /* FFT output is interleaved real/imag: [Re0, Im0, Re1, Im1, ...] */
    arm_cmplx_mag_squared_f32(hrv_fft_output, hrv_power_spectrum, HRV_FFT_SIZE / 2);

    /* Step 6: Normalize power spectrum to power per bin, in ms².
     *
     * The one-sided PSD is P[k] = 2|X[k]|² / (fs · Σw²) in ms²/Hz, and a
     * band's power is Σ P[k] · df with df = fs/N. The fs cancels:
     *     P[k] · df = 2|X[k]|² / (N · Σw²)
     * so this factor already yields ms² per bin and the band sums below are
     * the band powers directly. (They used to be multiplied by df once more,
     * which made every band power exactly 1/64 of the true value.)
     * N is the FFT length (256) even when the segment is shorter and
     * zero-padded; Σw² is that of the window applied (Step 3). */
    float32_t norm_factor = 2.0f / ((float32_t)HRV_FFT_SIZE * window_power);

    for (int i = 0; i < HRV_FFT_SIZE / 2; i++) {
        hrv_power_spectrum[i] *= norm_factor;
    }

    LOG_DBG("HRV FFT: N=%d, L=%u, window_power=%.1f, norm=%.6f",
            HRV_FFT_SIZE, (unsigned)fft_len, (double)window_power,
            (double)norm_factor);

    /* Step 7: Integrate power in frequency bands (bins are already ms², see
     * Step 6 -- do not scale by df again). */
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

BUILD_ASSERT(sizeof(struct hpi_ipc_beat_notify) == 16,
	     "BEAT_NOTIFY payload is 16 B (IPC <= 512)");

static void send_beat_notify(const struct ecg_algorithm_results *results)
{
	struct hpi_ipc_beat_notify n = {
		.timestamp_ms = results->timestamp,
		.sample_number = (uint32_t)ecg_state.last_qrs_sample,
		.rr_interval_ms = results->rr_interval_ms,
		.heart_rate_bpm = results->heart_rate != 0
					  ? results->heart_rate
					  : ecg_state.current_hr,
		.qrs_confidence = ecg_state.hr_confidence,
		.signal_quality = results->signal_quality,
	};
	int ret = hpi_ipc_send(HPI_IPC_MSG_TYPE_BEAT_NOTIFY, &n, sizeof(n));

	if (ret < 0) {
		LOG_WRN("BEAT_NOTIFY send failed: %d", ret);
	}
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

    /* Algorithm state. LEARNING whenever there is no rate -- at start-up and
     * after an expiry (hr_rr_count is reset then; rr_count never shrinks). */
    if (ecg_state.hr_rr_count < HR_MIN_RR_COUNT) {
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

