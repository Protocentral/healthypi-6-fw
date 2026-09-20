/*
 * Copyright (c) 2026 ProtoCentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Subscribe to bus ECG, band-limit thoracic Z (~0.08–0.45 Hz), peak/trough
 * interval → breaths/min. Scratch and IIR state are file-static. Drop on
 * full; never back-pressure acquisition. RA/LA/LL off → rate 0 (impedance
 * is limb-driven). V1 is not required. Do not publish HPI_CH_RESP.
 */

#include "resp_rate.h"

#include "sample_bus.h"
#include "sample_formats.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <errno.h>

LOG_MODULE_REGISTER(hpi_resp, CONFIG_HPI_APP_LOG_LEVEL);

#define RESP_STACK_SIZE     2048
#define RESP_PRIORITY       8
#define RESP_RING_WANT      8
#define RESP_RING_FALLBACK  4
#define RESP_PULL_MS        100
#define RESP_STATS_MS       5000

/* 500 Hz → 25 Hz. Breathing is < 1 Hz; 25 Hz is plenty after the 16-sample
 * MA already on hp6_ecg_sample.resp. */
#define RESP_DS_N           20
#define RESP_DS_HZ          (HPI_ECG_RATE_HZ / RESP_DS_N)

#define RESP_BPM_MIN        6
#define RESP_BPM_MAX        40
#define RESP_INTERVAL_MIN_MS (60000u / RESP_BPM_MAX)  /* 1500 */
#define RESP_INTERVAL_MAX_MS (60000u / RESP_BPM_MIN)  /* 10000 */
#define RESP_STALE_MS       12000

/* Limb electrodes drive the resp excitation. V1 off is not a reason to drop. */
#define RESP_LIMB_OFF       (HP6_LEAD_OFF_RA | HP6_LEAD_OFF_LA | HP6_LEAD_OFF_LL)

/* First-order IIR in Q15. HP R = exp(-2π·0.08/25) ≈ 0.980; two LP stages
 * α = 1 − exp(-2π·0.45/25) ≈ 0.107. */
#define RESP_HP_R_Q15       32115
#define RESP_LP_A_Q15       3506
#define RESP_ENV_DECAY_Q15  32735   /* 0.999/sample ≈ 14 s tau at 25 Hz */
#define RESP_MIN_AMP        100     /* filtered µV; below this stay at 0 */

#define RESP_N_INT          3

static atomic_t g_rr_bpm;
static struct hpi_bus_sub *g_sub;
static struct k_thread resp_thread;
static K_THREAD_STACK_DEFINE(resp_stack, RESP_STACK_SIZE);

static int32_t  ds_acc;
static uint8_t  ds_n;
static int32_t  x_prev;
static int32_t  hp_y;
static int32_t  lp1_y;
static int32_t  lp2_y;
static int32_t  y_z1;
static int32_t  y_z2;
static int32_t  env;
static int8_t   polarity;       /* 0 unlocked, +1 peak, −1 trough */
static uint32_t last_ext_ms;
static uint32_t intervals[RESP_N_INT];
static uint8_t  n_int;
static uint8_t  int_i;
static bool     logged_lock;
static bool     logged_zero;
static int64_t  stats_ms;

static int32_t q15_mul(int32_t x, int32_t a)
{
    return (int32_t)(((int64_t)x * a) >> 15);
}

static uint32_t median3(uint32_t a, uint32_t b, uint32_t c)
{
    if (a > b) {
        uint32_t t = a;
        a = b;
        b = t;
    }
    if (b > c) {
        uint32_t t = b;
        b = c;
        c = t;
    }
    if (a > b) {
        uint32_t t = a;
        a = b;
        b = t;
    }
    return b;
}

static void resp_reset(void)
{
    ds_acc = 0;
    ds_n = 0;
    x_prev = 0;
    hp_y = 0;
    lp1_y = 0;
    lp2_y = 0;
    y_z1 = 0;
    y_z2 = 0;
    env = 0;
    polarity = 0;
    last_ext_ms = 0;
    n_int = 0;
    int_i = 0;
    atomic_set(&g_rr_bpm, 0);
}

static void resp_publish_from_intervals(void)
{
    uint32_t ms;

    if (n_int < RESP_N_INT) {
        atomic_set(&g_rr_bpm, 0);
        return;
    }

    ms = median3(intervals[0], intervals[1], intervals[2]);
    if (ms < RESP_INTERVAL_MIN_MS || ms > RESP_INTERVAL_MAX_MS) {
        atomic_set(&g_rr_bpm, 0);
        return;
    }

    uint32_t bpm = 60000u / ms;

    if (bpm < RESP_BPM_MIN || bpm > RESP_BPM_MAX) {
        atomic_set(&g_rr_bpm, 0);
        return;
    }
    atomic_set(&g_rr_bpm, (int)bpm);
}

static void resp_accept_ext(uint32_t t_ms)
{
    if (last_ext_ms != 0U) {
        uint32_t dt = t_ms - last_ext_ms;

        if (dt < RESP_INTERVAL_MIN_MS) {
            return;
        }
        if (dt <= RESP_INTERVAL_MAX_MS) {
            intervals[int_i] = dt;
            int_i = (uint8_t)((int_i + 1u) % RESP_N_INT);
            if (n_int < RESP_N_INT) {
                n_int++;
            }
            resp_publish_from_intervals();
        } else {
            n_int = 0;
            int_i = 0;
            atomic_set(&g_rr_bpm, 0);
        }
    }
    last_ext_ms = t_ms;
}

static void resp_on_ds(int32_t x, uint32_t t_ms)
{
    int32_t hp = x - x_prev + q15_mul(hp_y, RESP_HP_R_Q15);
    int32_t lp1 = lp1_y + q15_mul(hp - lp1_y, RESP_LP_A_Q15);
    int32_t y = lp2_y + q15_mul(lp1 - lp2_y, RESP_LP_A_Q15);

    x_prev = x;
    hp_y = hp;
    lp1_y = lp1;
    lp2_y = y;

    int32_t ay = y < 0 ? -y : y;

    if (ay > env) {
        env = ay;
    } else {
        env = q15_mul(env, RESP_ENV_DECAY_Q15);
    }

    int32_t thresh = env / 4;

    if (thresh < RESP_MIN_AMP) {
        thresh = RESP_MIN_AMP;
    }

    bool local_max = (y_z1 > y_z2) && (y_z1 >= y);
    bool local_min = (y_z1 < y_z2) && (y_z1 <= y);

    if (polarity == 0) {
        if (local_max && y_z1 > thresh) {
            polarity = 1;
        } else if (local_min && y_z1 < -thresh) {
            polarity = -1;
        }
    }

    if (polarity > 0 && local_max && y_z1 > thresh) {
        resp_accept_ext(t_ms);
    } else if (polarity < 0 && local_min && y_z1 < -thresh) {
        resp_accept_ext(t_ms);
    }

    y_z2 = y_z1;
    y_z1 = y;

    if (last_ext_ms != 0U && (t_ms - last_ext_ms) > RESP_STALE_MS) {
        n_int = 0;
        int_i = 0;
        last_ext_ms = 0;
        polarity = 0;
        atomic_set(&g_rr_bpm, 0);
    }
}

static void resp_on_sample(const struct hp6_ecg_sample *s, uint32_t t_ms)
{
    if ((s->lead_off & RESP_LIMB_OFF) != 0) {
        if (atomic_get(&g_rr_bpm) != 0 || n_int != 0) {
            resp_reset();
        } else {
            ds_acc = 0;
            ds_n = 0;
        }
        return;
    }

    ds_acc += s->resp;
    ds_n++;
    if (ds_n < RESP_DS_N) {
        return;
    }

    int32_t x = ds_acc / (int32_t)RESP_DS_N;

    ds_acc = 0;
    ds_n = 0;
    resp_on_ds(x, t_ms);
}

static void resp_on_frame(const struct hpi_sample_frame *f)
{
    if (f->channel != HPI_CH_ECG || f->payload == NULL ||
        f->sample_count == 0) {
        return;
    }

    uint16_t n = f->sample_count;
    size_t need = (size_t)n * sizeof(struct hp6_ecg_sample);

    if (need > f->len) {
        n = (uint16_t)(f->len / sizeof(struct hp6_ecg_sample));
    }

    const struct hp6_ecg_sample *s = f->payload;
    uint32_t t0_ms = (uint32_t)(f->t_mono_us / 1000ULL);
    uint32_t dt_ms = (f->sample_rate != 0) ? (1000u / f->sample_rate) : 2u;

    for (uint16_t k = 0; k < n; k++) {
        resp_on_sample(&s[k], t0_ms + (uint32_t)k * dt_ms);
    }
}

static void resp_maybe_log(void)
{
    int64_t now = k_uptime_get();
    uint16_t rr = (uint16_t)atomic_get(&g_rr_bpm);

    if (rr != 0 && !logged_lock) {
        LOG_INF("resp: %u bpm (locked)", rr);
        logged_lock = true;
        logged_zero = false;
    } else if (rr == 0 && logged_lock && !logged_zero) {
        LOG_INF("resp: 0 (leads off or stale)");
        logged_zero = true;
        logged_lock = false;
    }

    if (now - stats_ms < RESP_STATS_MS) {
        return;
    }
    stats_ms = now;

    struct hpi_bus_sub_stats st = { 0 };

    hpi_bus_sub_get_stats(g_sub, &st);
    LOG_INF("resp: %u bpm env=%d drop=%u", rr, env, st.frames_dropped);
}

static void resp_thread_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    stats_ms = k_uptime_get();

    for (;;) {
        struct hpi_sample_frame f;
        int rc = hpi_bus_pull_wait(g_sub, &f, RESP_PULL_MS);

        if (rc == 0) {
            resp_on_frame(&f);
        }
        resp_maybe_log();
    }
}

int hpi_resp_rate_init(void)
{
    atomic_set(&g_rr_bpm, 0);
    resp_reset();

    struct hpi_bus_sub_cfg cfg = {
        .name = "resp_rate",
        .channel_mask = HPI_CH_BIT(HPI_CH_ECG),
        .ring_frames = RESP_RING_WANT,
    };
    static const uint16_t depths[] = { RESP_RING_WANT, RESP_RING_FALLBACK };

    for (size_t i = 0; i < ARRAY_SIZE(depths); i++) {
        cfg.ring_frames = depths[i];
        g_sub = hpi_bus_subscribe(&cfg);
        if (g_sub != NULL) {
            if (i > 0) {
                LOG_WRN("resp: ring %u (wanted %u; heap tight)",
                        depths[i], depths[0]);
            }
            break;
        }
    }

    if (g_sub == NULL) {
        LOG_ERR("resp: subscribe failed");
        return -ENOMEM;
    }

    LOG_INF("resp: thoracic Z -> rr_bpm (ring %u, %u-%u bpm)",
            cfg.ring_frames, RESP_BPM_MIN, RESP_BPM_MAX);

    k_thread_create(&resp_thread, resp_stack, K_THREAD_STACK_SIZEOF(resp_stack),
                    resp_thread_fn, NULL, NULL, NULL,
                    RESP_PRIORITY, 0, K_NO_WAIT);
    k_thread_name_set(&resp_thread, "hpi_resp");
    return 0;
}

uint16_t hpi_resp_rate_bpm(void)
{
    return (uint16_t)atomic_get(&g_rr_bpm);
}
