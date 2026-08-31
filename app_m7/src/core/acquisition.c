/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Onboard sensor acquisition producer (ADS1294R/AFE4400, RTIO + DATA_READY
 * trigger). Publishes canonical frames to the sample bus. Conversions:
 * raw -> µV for ECG/resp; raw 22-bit sign-extend for PPG.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>

#include "ads129xx.h"
#include "afe4400.h"

#include "sample_bus.h"
#include "sample_formats.h"
#include "acquisition.h"
#include "../platform/health.h"   /* acquisition heartbeat */

LOG_MODULE_REGISTER(hpi_acq, CONFIG_HPI_APP_LOG_LEVEL);

#define ECG_NODE DT_NODELABEL(ads129xx)
#define PPG_NODE DT_NODELABEL(afe4400)

/* Per-sample rates come from the shared header (sample_formats.h):
 * ECG = ADS1294R 500 Hz; PPG = the AFE4400 PRF, set by CONFIG_AFE4400_PRF_HZ
 * (250 Hz by default). Both are DATA_READY-triggered, so these are what the
 * hardware actually delivers -- not a label applied on top of it. */
#define ECG_RATE_HZ HPI_ECG_RATE_HZ
#define PPG_RATE_HZ HPI_PPG_RATE_HZ

/* Samples per published frame (matches .HP6 + IPC batching). Batching
 * amortises the fixed per-frame ring-slot cost and keeps frame rates sane
 * (~31/s ECG, ~16/s PPG at the default 250 Hz). */
#define ECG_BATCH 16
#define PPG_BATCH 16

static const struct device *const ecg_dev = DEVICE_DT_GET(ECG_NODE);
static const struct device *const ppg_dev = DEVICE_DT_GET(PPG_NODE);

/* RTIO read contexts + iodevs. */
RTIO_DEFINE(ecg_rtio, 16, 16);
SENSOR_DT_READ_IODEV(ecg_iodev, ECG_NODE, {SENSOR_CHAN_VOLTAGE});
RTIO_DEFINE(ppg_rtio, 16, 16);
SENSOR_DT_READ_IODEV(ppg_iodev, PPG_NODE, {SENSOR_CHAN_RED, SENSOR_CHAN_IR});

static __aligned(4) uint8_t ecg_buf[512];
static __aligned(4) uint8_t ppg_buf[512];

/* ---- conversions ---- */
#define ADC_FULL_SCALE     8388607LL   /* 2^23 - 1 */
#define ECG_UV_MULTIPLIER  400000LL    /* VREF(2.4V) / PGA 6 */
#define RESP_UV_MULTIPLIER 600000LL    /* VREF(2.4V) / PGA 4 */

static inline int32_t adc_to_uv_ecg(int32_t c)
{
    return (int32_t)(((int64_t)c * ECG_UV_MULTIPLIER) / ADC_FULL_SCALE);
}
static inline int32_t adc_to_uv_resp(int32_t c)
{
    return (int32_t)(((int64_t)c * RESP_UV_MULTIPLIER) / ADC_FULL_SCALE);
}

#define RESP_FILTER_SIZE 16
static int32_t resp_buf[RESP_FILTER_SIZE];
static uint8_t resp_idx;
static int64_t resp_sum;
static bool    resp_filled;

static int32_t resp_moving_average(int32_t s)
{
    resp_sum -= resp_buf[resp_idx];
    resp_buf[resp_idx] = s;
    resp_sum += s;
    resp_idx = (resp_idx + 1) % RESP_FILTER_SIZE;
    if (!resp_filled && resp_idx == 0) {
        resp_filled = true;
    }
    uint8_t n = resp_filled ? RESP_FILTER_SIZE : (resp_idx ? resp_idx : 1);
    return (int32_t)(resp_sum / n);
}

static inline int32_t sext22(uint32_t v)   /* AFE4400 22-bit -> signed */
{
    v &= 0x3FFFFFu;
    return (v & (1u << 21)) ? (int32_t)(v | ~0x3FFFFFu) : (int32_t)v;
}

/* ---- ECG lead-off: status word -> canonical electrode mask, debounced ----
 *
 * The ADS1294R prepends a 24-bit status word to every conversion:
 *
 *   [23:20] = 1100   [19:12] = LOFF_STATP   [11:4] = LOFF_STATN   [3:0] = GPIO
 *
 * with LOFF_STATP/N bit n = channel n+1 (P = positive input, N = negative).
 * Against the canonical electrode map (LA=IN2P, LL=IN3P, V1=IN4P, RA=IN2N) the
 * electrodes we can see are therefore spread across BOTH bytes.
 */
static inline uint8_t ecg_lead_off_raw(uint32_t stat)
{
    uint8_t p = (uint8_t)((stat >> 12) & 0x0F);   /* LOFF_STATP: CH1P..CH4P */
    uint8_t n = (uint8_t)((stat >> 4) & 0x0F);    /* LOFF_STATN: CH1N..CH4N */

    return (uint8_t)(((n & BIT(1)) ? HP6_LEAD_OFF_RA : 0) |   /* RA = CH2N */
                     ((p & BIT(1)) ? HP6_LEAD_OFF_LA : 0) |   /* LA = CH2P */
                     ((p & BIT(2)) ? HP6_LEAD_OFF_LL : 0) |   /* LL = CH3P */
                     ((p & BIT(3)) ? HP6_LEAD_OFF_V1 : 0));   /* V1 = CH4P */
}

/* Debounce in the PRODUCER, once, so every consumer sees a state rather than
 * chatter. The comparators toggle sample-to-sample when an electrode sits near
 * the threshold (a drying gel pad, a subject moving), and at 500 Hz that would
 * reach the UI, the recording and the HR-source arbitration as a stream of
 * transitions. A change must hold for LEAD_OFF_DEBOUNCE consecutive samples
 * before it is published. */
#define LEAD_OFF_DEBOUNCE  (ECG_RATE_HZ / 10)   /* ~100 ms */

static uint8_t  lead_off_stable;    /* what consumers see */
static uint8_t  lead_off_cand;      /* candidate awaiting confirmation */
static uint16_t lead_off_run;       /* consecutive samples agreeing with cand */
/* Uptime of the last ECG sample, truncated to 32 bits ON PURPOSE: this is
 * written from the DRDY trigger and read from other threads, and a 32-bit
 * load/store is single-copy atomic on this core while a 64-bit one is not.
 * Unsigned subtraction gives the correct age across the ~49-day wrap. */
static uint32_t lead_off_at;

static uint8_t ecg_lead_off_debounced(uint32_t stat)
{
    uint8_t raw = ecg_lead_off_raw(stat);

    if (raw != lead_off_cand) {
        lead_off_cand = raw;
        lead_off_run = 0;
    } else if (raw != lead_off_stable && ++lead_off_run >= LEAD_OFF_DEBOUNCE) {
        LOG_INF("ECG lead-off: 0x%02x -> 0x%02x", lead_off_stable, raw);
        lead_off_stable = raw;
    }
    lead_off_at = (uint32_t)k_uptime_get();
    return lead_off_stable;
}

uint8_t hpi_acquisition_lead_off(uint32_t *age_ms)
{
    if (age_ms != NULL) {
        *age_ms = (uint32_t)k_uptime_get() - lead_off_at;
    }
    return lead_off_stable;
}

/* ---- ECG (+ respiration): accumulate ECG_BATCH samples per bus frame ---- */
static struct hp6_ecg_sample ecg_batch[ECG_BATCH];
static uint8_t  ecg_n;
static uint64_t ecg_t0_us;

static void ecg_publish(const struct ads129xx_encoded_data *d)
{
    if (ecg_n == 0) {
        ecg_t0_us = (uint64_t)k_uptime_get() * 1000ULL;  /* time of first sample */
    }
    ecg_batch[ecg_n] = (struct hp6_ecg_sample){
        .resp     = adc_to_uv_resp(resp_moving_average(d->data_ch0)),
        .lead_i   = adc_to_uv_ecg(d->data_ch1),
        .lead_ii  = adc_to_uv_ecg(d->data_ch2),
        .v1       = adc_to_uv_ecg(d->data_ch3),
        .lead_off = ecg_lead_off_debounced(d->data_stat),
    };
    if (++ecg_n < ECG_BATCH) {
        return;
    }
    struct hpi_sample_frame f = {
        .channel = HPI_CH_ECG,
        .sample_rate = ECG_RATE_HZ,
        .sample_count = ECG_BATCH,
        .t_mono_us = ecg_t0_us,
        .len = sizeof(ecg_batch),
        .payload = ecg_batch,
    };
    (void)hpi_bus_publish(&f);
    ecg_n = 0;
    hpi_health_alive(HPI_SUBSYS_ACQ);   /* acquisition heartbeat */
}

static void ecg_trigger(const struct device *dev, const struct sensor_trigger *trig)
{
    ARG_UNUSED(dev); ARG_UNUSED(trig);
    if (sensor_read(&ecg_iodev, &ecg_rtio, ecg_buf, sizeof(ecg_buf)) < 0) {
        LOG_ERR("ECG read failed");
        return;
    }
    ecg_publish((const struct ads129xx_encoded_data *)ecg_buf);
}

/* ---- PPG: accumulate PPG_BATCH samples per bus frame ---- */
static struct hp6_ppg_sample ppg_batch[PPG_BATCH];
static uint8_t  ppg_n;
static uint64_t ppg_t0_us;

static void ppg_publish(const struct afe4400_encoded_data *d)
{
    if (ppg_n == 0) {
        ppg_t0_us = (uint64_t)k_uptime_get() * 1000ULL;
    }
    ppg_batch[ppg_n] = (struct hp6_ppg_sample){
        .red = sext22(d->led1_val),
        .ir  = sext22(d->led2_val),
        .lead_off = 0,   /* TODO(hw): derive from diag_val if needed */
    };
    if (++ppg_n < PPG_BATCH) {
        return;
    }
    struct hpi_sample_frame f = {
        .channel = HPI_CH_PPG,
        .sample_rate = PPG_RATE_HZ,
        .sample_count = PPG_BATCH,
        .t_mono_us = ppg_t0_us,
        .len = sizeof(ppg_batch),
        .payload = ppg_batch,
    };
    (void)hpi_bus_publish(&f);
    ppg_n = 0;
}

static void ppg_trigger(const struct device *dev, const struct sensor_trigger *trig)
{
    ARG_UNUSED(dev); ARG_UNUSED(trig);
    if (sensor_read(&ppg_iodev, &ppg_rtio, ppg_buf, sizeof(ppg_buf)) < 0) {
        LOG_ERR("PPG read failed");
        return;
    }
    ppg_publish((const struct afe4400_encoded_data *)ppg_buf);
}

int hpi_acquisition_init(void)
{
    int rc = 0;

    /* NOTE: do not gate on device_is_ready() here -- these are deferred-init
     * devices and it returns false even when usable. Rely on the trigger-set
     * return instead. */
    struct sensor_trigger ecg_t = { .type = SENSOR_TRIG_DATA_READY, .chan = SENSOR_CHAN_ALL };
    if (sensor_trigger_set(ecg_dev, &ecg_t, ecg_trigger) < 0) {
        LOG_ERR("ECG (ADS1294R) trigger set failed");
        rc = -EIO;
    } else {
        LOG_INF("ECG acquisition started (%d Hz -> bus)", ECG_RATE_HZ);
    }

    struct sensor_trigger ppg_t = { .type = SENSOR_TRIG_DATA_READY, .chan = SENSOR_CHAN_ALL };
    if (sensor_trigger_set(ppg_dev, &ppg_t, ppg_trigger) < 0) {
        LOG_ERR("PPG (AFE4400) trigger set failed");
        rc = -EIO;
    } else {
        LOG_INF("PPG acquisition started (%d Hz -> bus)", PPG_RATE_HZ);
    }

    return rc;
}

/* ---- Optional boot debug consumer: proves real samples flow on the bus ----
 * Subscribes ECG+PPG and logs per-channel SAMPLE rate once a second:
 *   acq: ECG ~500/s PPG ~250/s (dropped=.. hw=..)
 * PPG should track CONFIG_AFE4400_PRF_HZ; a large mismatch means the device is
 * not running at the rate the rest of the system assumes.
 */
#if defined(CONFIG_HPI_ACQ_DEBUG)
static void acq_debug_thread(void *a, void *b, void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    struct hpi_bus_sub_cfg cfg = {
        .name = "acqdbg",
        .channel_mask = HPI_CH_BIT(HPI_CH_ECG) | HPI_CH_BIT(HPI_CH_PPG),
        /* Counts samples and drains continuously, so a small ring suffices --
         * a deep one would eat the 64 KB heap and starve later consumers. */
        .ring_frames = 8,
    };
    struct hpi_bus_sub *sub = hpi_bus_subscribe(&cfg);
    if (sub == NULL) {
        LOG_ERR("acq debug: subscribe failed");
        return;
    }
    /* Report window: accumulate samples, then emit one averaged line per
     * window so the console isn't flooded once-a-second. */
    const int64_t window_ms = 5000;
    uint32_t ecg = 0, ppg = 0;
    int64_t t0 = k_uptime_get();
    struct hpi_sample_frame f;
    while (1) {
        while (hpi_bus_pull(sub, &f) == 0) {
            if (f.channel == HPI_CH_ECG) ecg += f.sample_count;
            else if (f.channel == HPI_CH_PPG) ppg += f.sample_count;
        }
        int64_t elapsed = k_uptime_get() - t0;
        if (elapsed >= window_ms) {
            struct hpi_bus_sub_stats st;
            hpi_bus_sub_get_stats(sub, &st);
            uint32_t secs = (uint32_t)(elapsed / 1000);
            if (secs == 0) secs = 1;
            LOG_INF("acq: ECG %u samp/s PPG %u samp/s (avg over %us, frames dropped=%u hw=%u)",
                    ecg / secs, ppg / secs, secs, st.frames_dropped, st.ring_high_water);
            ecg = ppg = 0;
            t0 = k_uptime_get();
        }
        k_msleep(5);
    }
}
K_THREAD_DEFINE(acq_dbg_tid, 1280, acq_debug_thread, NULL, NULL, NULL, 7, 0, 0);
#endif
