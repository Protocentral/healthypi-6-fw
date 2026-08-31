/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * M4 IPC bridge (OpenAMP/RPMSG, M7 = HOST). Two directions:
 *   M7 -> M4 : forward raw ECG/PPG batches (the M4 owns no sensors; it needs
 *              the M7's samples to compute vitals). Sourced from the sample
 *              bus, so acquisition stays decoupled.
 *   M4 -> M7 : receive computed vitals (HR/HRV/SpO2) and publish them on the
 *              bus as HPI_CH_VITALS.
 *
 * Payload structs are the shared contract in hpi_common_types.h (the same
 * header the M4 uses). Dual-core / hardware-bound -- validate with the M4
 * flashed.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/ipc/ipc_service.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include "hpi_common_types.h"          /* raw batch + vitals payload structs */
#include "core/acquisition.h"          /* debounced ECG lead-off state */
#include "core/sample_bus.h"
#include "core/sample_formats.h"
#include "m4_ipc_protocol.h"           /* envelope + msg ids + ept name */
#include "ipc.h"
#include "health.h"                    /* M4 link heartbeat */

LOG_MODULE_REGISTER(hpi_ipc, CONFIG_HPI_APP_LOG_LEVEL);

/* Throttle period for the dev vitals debug lines (ECG/PPG). Vitals arrive at
 * several Hz; emit a summary at most this often so the console stays readable. */
#define HPI_VITALS_LOG_PERIOD_MS 5000

static struct ipc_ept ept;
static const struct device *ipc_inst;
static K_SEM_DEFINE(bound_sem, 0, 1);
static volatile bool ipc_ready;

bool hpi_ipc_ready(void)
{
    return ipc_ready;
}

/* Latest merged vitals; ECG fills HR/HRV, PPG fills SpO2. Published on update. */
static struct hp6_vitals g_vitals;

void hpi_ipc_last_vitals(struct hp6_vitals *out)
{
    if (out != NULL) {
        *out = g_vitals;   /* 12 bytes, single-word fields: a torn read here
                            * would cost more to prevent than it can hurt */
    }
}


/* ---------------- heart-rate source arbitration ----------------
 *
 * Two detectors can produce a heart rate -- the M4's ECG QRS detector and its
 * PPG pulse detector -- and they are NOT interchangeable. This block decides
 * which one hp6_vitals.hr_bpm carries and records that choice in
 * hp6_vitals.flags, so every consumer knows the number's provenance.
 *
 * Three rules:
 *  1. An ECG HR is only credible while the leads are ON -- a disconnected
 *     electrode rails and the QRS detector finds "beats" in the noise.
 *  2. Both HRs EXPIRE (HR_STALE_MS); a stale source never holds the field.
 *  3. The PPG fallback is EXPLICIT (HP6_VIT_HR_FROM_PPG) -- a PPG pulse rate
 *     must never be rendered under an ECG label.
 */
#define HR_STALE_MS       5000   /* a source older than this no longer counts */
#define LEAD_OFF_STALE_MS 5000   /* ECG quiet this long: lead state is unknown */

static struct {
    uint16_t ecg_hr;
    int64_t  ecg_at;
    uint16_t ppg_hr;
    int64_t  ppg_at;
    bool     ppg_weak;   /* M4 says low perfusion / poor quality */
} hr_src;

/* Fill hr_bpm + flags from whichever source is currently credible, then publish.
 * Recomputed on every vitals message rather than mutated in place: staleness is
 * a function of time, so "what is true now" cannot be maintained incrementally
 * by whichever message happened to arrive last. */
static void publish_vitals(void)
{
    int64_t now = k_uptime_get();
    uint32_t lead_age = UINT32_MAX;
    uint8_t lead_off = hpi_acquisition_lead_off(&lead_age);
    /* An old mask means the ECG front end went quiet, which is not evidence
     * that the electrodes are on -- but it is also not grounds to suppress a
     * rate, since no ECG vitals will be arriving either way. */
    bool leads_off = (lead_age <= LEAD_OFF_STALE_MS) && (lead_off != 0);

    bool ecg_ok = !leads_off && hr_src.ecg_hr != 0U &&
                  (now - hr_src.ecg_at) <= HR_STALE_MS;
    bool ppg_ok = hr_src.ppg_hr != 0U && (now - hr_src.ppg_at) <= HR_STALE_MS;

    g_vitals.flags = leads_off ? HP6_VIT_ECG_LEAD_OFF : 0;

    if (ecg_ok) {
        g_vitals.hr_bpm = hr_src.ecg_hr;
    } else if (ppg_ok) {
        /* The PPG rate is reported even when the M4 calls perfusion low -- with
         * HP6_VIT_PPG_WEAK set, so a consumer can present it as provisional
         * rather than being handed nothing at all. */
        g_vitals.hr_bpm = hr_src.ppg_hr;
        g_vitals.flags |= HP6_VIT_HR_FROM_PPG;
    } else {
        g_vitals.hr_bpm = 0U;   /* 0 = not available (never "measured zero") */
    }

    /* PPG_WEAK qualifies EVERYTHING the PPG produced, not just a PPG-sourced
     * rate. It used to be set only inside the branch above, i.e. only when the
     * ECG rate was unavailable -- so in the normal case, good ECG plus a poorly
     * perfused finger, spo2_x10 travelled with no quality indication at all.
     * SpO2 is the value most likely to be read off a poorly perfused finger and
     * the one a reader is least able to sanity-check, so the flag follows the
     * signal, not the arbitration outcome. */
    if (hr_src.ppg_weak) {
        g_vitals.flags |= HP6_VIT_PPG_WEAK;
    }

    /* HRV comes from the ECG beat series, so it dies with the ECG HR. */
    if (!ecg_ok) {
        g_vitals.hrv_sdnn_ms = 0U;
        g_vitals.hrv_rmssd_ms = 0U;
        g_vitals.hrv_lf_hf_x10 = 0U;
    }

    struct hpi_sample_frame f = {
        .channel = HPI_CH_VITALS,
        .sample_rate = 1,
        .sample_count = 1,
        .t_mono_us = (uint64_t)now * 1000ULL,
        .len = sizeof(g_vitals),
        .payload = &g_vitals,
    };
    (void)hpi_bus_publish(&f);
    hpi_health_alive(HPI_SUBSYS_M4_IPC);   /* M4 link heartbeat */
}

static void on_ecg_vitals(const struct hpi_ipc_ecg_vitals *v)
{
    hr_src.ecg_hr = v->heart_rate;
    hr_src.ecg_at = k_uptime_get();
    /* Passed through at full width since format 0x0300. These used to go
     * through clamp_u8(): SDNN over 255 ms occurs in healthy young adults at
     * rest and routinely in AF, and a clamped 255 could not be told apart from
     * a measured one. */
    g_vitals.hrv_sdnn_ms  = v->hrv_sdnn;
    g_vitals.hrv_rmssd_ms = v->hrv_rmssd;
    /* The M4 reports the LF/HF ratio x10 in a uint8 and flags its validity;
     * carry it only when the frequency-domain pass actually ran. */
    g_vitals.hrv_lf_hf_x10 = v->hrv_freq_valid ? v->hrv_lf_hf_ratio_x10 : 0U;
    publish_vitals();
#if defined(CONFIG_HPI_ACQ_DEBUG)
    /* Throttle to ~once per window; vitals arrive at several Hz and flood the
     * console otherwise. */
    static int64_t last_log;
    int64_t now = k_uptime_get();
    if (now - last_log >= HPI_VITALS_LOG_PERIOD_MS) {
        last_log = now;
        LOG_INF("vitals(ecg): HR=%u SDNN=%u RMSSD=%u q=%u",
                v->heart_rate, v->hrv_sdnn, v->hrv_rmssd, v->signal_quality);
    }
#endif
}

static void on_ppg_vitals(const struct hpi_ipc_ppg_vitals *v)
{
    g_vitals.spo2_x10 = (uint16_t)v->spo2 * 10U;
    hr_src.ppg_hr = v->heart_rate;
    hr_src.ppg_at = k_uptime_get();
    /* The M4 already judges the signal; take its word rather than inventing a
     * second threshold here. LEAD_OFF here means "finger off the sensor". */
    hr_src.ppg_weak = (v->flags & (HPI_PPG_FLAG_LOW_PERFUSION |
                                   HPI_PPG_FLAG_LEAD_OFF)) != 0U;
    publish_vitals();   /* ECG HR still wins when it is fresh and leads are on */
#if defined(CONFIG_HPI_ACQ_DEBUG)
    /* Throttle to ~once per window (see on_ecg_vitals). */
    static int64_t last_log;
    int64_t now = k_uptime_get();
    if (now - last_log < HPI_VITALS_LOG_PERIOD_MS) {
        return;
    }
    last_log = now;
    /* Full dump to localise SpO2 issues: PI = IR perfusion index (0.1%%),
     * pp = IR AC amplitude, st = algo state, flags bit1=low-perfusion
     * bit3=sat-low. Weak PI/pp -> AFE LED/gain; strong PI but wrong SpO2 ->
     * red/IR mapping or R-curve calibration. */
    LOG_INF("vitals(ppg): SpO2=%u%% HR=%u PI=%u.%u%% pp=%u q=%u conf=%u st=%u flags=0x%02x",
            v->spo2, v->heart_rate,
            v->perfusion_index / 10U, v->perfusion_index % 10U,
            v->peak_to_peak, v->signal_quality, v->spo2_confidence,
            v->algorithm_state, v->flags);
    /* SpO2 calibration telemetry stashed by the M4 in reserved[] (see
     * spo2_module.c). pp above is AC_ir. R should be ~0.4-0.7 for a healthy
     * finger; the two DCs should be comparable + mid-range. */
    {
        int32_t dc_red, dc_ir;
        int16_t ac_red, r16;
        memcpy(&dc_red, &v->reserved[0], sizeof(dc_red));
        memcpy(&dc_ir,  &v->reserved[4], sizeof(dc_ir));
        memcpy(&ac_red, &v->reserved[8], sizeof(ac_red));
        memcpy(&r16,    &v->reserved[10], sizeof(r16));
        LOG_INF("  ppg dbg: R=%d.%03d  AC(red=%d ir=%u)  DC(red=%d ir=%d)",
                r16 / 1000, (r16 < 0 ? -r16 : r16) % 1000,
                ac_red, v->peak_to_peak, dc_red, dc_ir);
    }
#endif
}

/* Latest version string reported by the M4. Empty until it binds and sends one;
 * an older M4 build that never sends HPI_IPC_MSG_TYPE_VERSION simply leaves it
 * empty rather than breaking the bind. */
static char m4_version[HPI_IPC_VERSION_STR_MAX];

const char *hpi_ipc_m4_version(void)
{
    return m4_version;
}

static void on_m4_version(const struct hpi_ipc_version *v)
{
    /* Copy defensively: the payload is fixed-size but the sender's string may
     * not be terminated if a future M4 fills the field completely. */
    memcpy(m4_version, v->version, sizeof(m4_version) - 1);
    m4_version[sizeof(m4_version) - 1] = '\0';
    LOG_INF("M4 firmware version: %s", m4_version);
}

static void ept_recv(const void *data, size_t len, void *priv)
{
    ARG_UNUSED(priv);
    if (len < sizeof(struct hpi_ipc_msg)) {
        return;
    }
    const struct hpi_ipc_msg *m = data;
    if (m->length != (len - sizeof(struct hpi_ipc_msg))) {
        return;
    }
    switch (m->type) {
    case HPI_IPC_MSG_TYPE_ECG_VITALS:
        if (m->length >= sizeof(struct hpi_ipc_ecg_vitals)) {
            on_ecg_vitals((const struct hpi_ipc_ecg_vitals *)m->data);
        }
        break;
    case HPI_IPC_MSG_TYPE_PPG_VITALS:
        if (m->length >= sizeof(struct hpi_ipc_ppg_vitals)) {
            on_ppg_vitals((const struct hpi_ipc_ppg_vitals *)m->data);
        }
        break;
    case HPI_IPC_MSG_TYPE_VERSION:
        if (m->length >= sizeof(struct hpi_ipc_version)) {
            on_m4_version((const struct hpi_ipc_version *)m->data);
        }
        break;
    default:
        break;
    }
}

static void ept_bound(void *priv)  { ARG_UNUSED(priv); k_sem_give(&bound_sem); }
static void ept_error(const char *msg, void *priv) { ARG_UNUSED(priv); LOG_ERR("IPC ept error: %s", msg); }

static struct ipc_ept_cfg ept_cfg = {
    .name = HPI_IPC_EPT_NAME,
    .cb = { .bound = ept_bound, .received = ept_recv, .error = ept_error },
};

/* ---------------- M7 -> M4 : forward raw ECG/PPG ---------------- */

/* Envelope + payload, sized for the largest raw batch. */
static uint8_t tx_buf[sizeof(struct hpi_ipc_msg) + 512];

static int ipc_send(uint8_t type, const void *payload, uint16_t len)
{
    if (len > (sizeof(tx_buf) - sizeof(struct hpi_ipc_msg))) {
        return -EMSGSIZE;
    }
    struct hpi_ipc_msg *m = (struct hpi_ipc_msg *)tx_buf;
    m->type = type;
    m->reserved = 0;
    m->length = len;
    memcpy(m->data, payload, len);
    return ipc_service_send(&ept, tx_buf, sizeof(struct hpi_ipc_msg) + len);
}

static void forward_ecg(const struct hpi_sample_frame *f, uint16_t *seq)
{
    const struct hp6_ecg_sample *s = f->payload;
    uint16_t n = f->sample_count;
    if (n > HPI_ECG_BATCH_SIZE) n = HPI_ECG_BATCH_SIZE;

    struct hpi_ipc_ecg_raw_batch b = { .sample_count = n, .reserved = 0 };
    uint32_t ts = (uint32_t)(f->t_mono_us / 1000ULL);
    for (uint16_t i = 0; i < n; i++) {
        b.samples[i].timestamp_ms    = ts;
        b.samples[i].ecg_lead1       = s[i].lead_i;
        b.samples[i].ecg_lead2       = s[i].lead_ii;
        b.samples[i].ecg_lead3       = s[i].v1;
        b.samples[i].sample_number   = (*seq)++;
        b.samples[i].lead_off_status = s[i].lead_off;
        b.samples[i].reserved        = 0;
    }
    (void)ipc_send(HPI_IPC_MSG_TYPE_ECG_RAW, &b,
                   (uint16_t)(4 + n * sizeof(struct hpi_ipc_ecg_raw_sample)));
}

/* The M4 SpO2 algorithm expects ~125 Hz PPG; the AFE4400 now runs 250 Hz, so
 * decimate 2:1 by averaging consecutive sample pairs (mild anti-alias + SNR),
 * batched 16 -> ~7.8 PPG_RAW/s @ 125 Hz. Decimation is over the sample stream
 * (frame-boundary agnostic), so it adapts if PPG_RATE changes (factor =
 * source / 125; =1 means pass-through). The bus still carries full-rate PPG. */
/* ECG is forwarded undecimated, so the rate the M4 assumes must be the rate
 * acquisition actually produces. */
BUILD_ASSERT(HPI_ECG_M4_RATE_HZ == HPI_ECG_RATE_HZ,
             "The M4 derives RR intervals from HPI_ECG_M4_RATE_HZ, but forward_ecg() "
             "sends ECG at HPI_ECG_RATE_HZ undecimated. They must agree.");

#define PPG_M4_RATE_HZ HPI_PPG_M4_RATE_HZ
/* Clamp to >= 1: the source rate is configurable (CONFIG_AFE4400_PRF_HZ, down to
 * 62 Hz) and a source below 125 Hz would otherwise make this 0 -- which both
 * skips the batching guard and divides by zero in the averaging below. At or
 * under 125 Hz the M4 simply gets the samples through unaveraged. */
#define PPG_DECIM_RAW  (HPI_PPG_RATE_HZ / PPG_M4_RATE_HZ)
#define PPG_DECIM      (PPG_DECIM_RAW < 1 ? 1 : PPG_DECIM_RAW)   /* 250/125 = 2 */

static struct hpi_ipc_ppg_raw_batch ppg_dec;
static uint16_t ppg_dec_n;
static int64_t  ppg_acc_red, ppg_acc_ir;
static uint16_t ppg_acc_cnt;
static uint8_t  ppg_acc_leadoff;

static void forward_ppg(const struct hpi_sample_frame *f, uint16_t *seq)
{
    const struct hp6_ppg_sample *s = f->payload;
    uint16_t n = f->sample_count;
    uint32_t ts = (uint32_t)(f->t_mono_us / 1000ULL);

    for (uint16_t i = 0; i < n; i++) {
        ppg_acc_red += s[i].red;
        ppg_acc_ir  += s[i].ir;
        ppg_acc_leadoff = s[i].lead_off;
        if (++ppg_acc_cnt < PPG_DECIM) {
            continue;
        }
        struct hpi_ipc_ppg_raw_sample *d = &ppg_dec.samples[ppg_dec_n];
        d->timestamp_ms    = ts;
        d->red_raw         = (int32_t)(ppg_acc_red / PPG_DECIM);
        d->ir_raw          = (int32_t)(ppg_acc_ir / PPG_DECIM);
        d->sample_number   = (*seq)++;
        d->lead_off_status = ppg_acc_leadoff;
        d->quality_flags   = 0;
        ppg_acc_red = ppg_acc_ir = 0;
        ppg_acc_cnt = 0;

        if (++ppg_dec_n < HPI_PPG_BATCH_SIZE) {
            continue;
        }
        ppg_dec.sample_count = ppg_dec_n;
        ppg_dec.reserved = 0;
        (void)ipc_send(HPI_IPC_MSG_TYPE_PPG_RAW, &ppg_dec,
                       (uint16_t)(4 + ppg_dec_n * sizeof(struct hpi_ipc_ppg_raw_sample)));
        ppg_dec_n = 0;
    }
}

/* ---------------- bind, then run the forward loop ---------------- */

static void ipc_thread(void *a, void *b, void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

    ipc_inst = DEVICE_DT_GET(DT_NODELABEL(ipc0));
    if (!device_is_ready(ipc_inst)) {
        LOG_ERR("ipc0 not ready; M4 link unavailable");
        return;
    }
    int rc = ipc_service_open_instance(ipc_inst);
    if (rc < 0 && rc != -EALREADY) {
        LOG_ERR("ipc open failed (%d)", rc);
        return;
    }
    rc = ipc_service_register_endpoint(ipc_inst, &ept, &ept_cfg);
    if (rc < 0) {
        LOG_ERR("ipc endpoint register failed (%d)", rc);
        return;
    }
    /* Wait for the remote (M4) to bind. The M4 self-delays ~7 s after a chip
     * reset and normally binds within this window. If it doesn't (M4 not
     * running), flag the link FAILED and continue degraded -- vitals are
     * non-critical and the rest of the device runs without the M4. Recovery is
     * a coordinated chip reset (both cores boot together); a live re-bind is not
     * possible with RPMSG static-vrings against an already-running host. */
    if (k_sem_take(&bound_sem, K_MSEC(12000)) != 0) {
        LOG_WRN("M4 IPC bind timeout -- no vitals (M4 not running?)");
        hpi_health_set(HPI_SUBSYS_M4_IPC, HPI_HEALTH_FAILED, "bind timeout");
        return;
    }
    ipc_ready = true;
    hpi_health_alive(HPI_SUBSYS_M4_IPC);   /* first beat: bound */
    LOG_INF("M4 IPC bound; forwarding ECG/PPG -> M4, vitals -> bus");

    /* Subscribe to raw ECG+PPG and forward each batch to the M4. */
    struct hpi_bus_sub_cfg cfg = {
        .name = "m4feed",
        .channel_mask = HPI_CH_BIT(HPI_CH_ECG) | HPI_CH_BIT(HPI_CH_PPG),
        .ring_frames = 8,
    };
    struct hpi_bus_sub *sub = hpi_bus_subscribe(&cfg);
    if (sub == NULL) {
        LOG_ERR("m4 feed: subscribe failed");
        return;
    }
    uint16_t ecg_seq = 0, ppg_seq = 0;
    struct hpi_sample_frame f;
    while (1) {
        while (hpi_bus_pull(sub, &f) == 0) {
            if (f.channel == HPI_CH_ECG) {
                forward_ecg(&f, &ecg_seq);
            } else if (f.channel == HPI_CH_PPG) {
                forward_ppg(&f, &ppg_seq);
            }
        }
        k_msleep(2);
    }
}

#define IPC_THREAD_STACK 4096
static K_THREAD_STACK_DEFINE(ipc_thread_stack, IPC_THREAD_STACK);
static struct k_thread ipc_thread_data;

int hpi_ipc_init(void)
{
    k_thread_create(&ipc_thread_data, ipc_thread_stack, IPC_THREAD_STACK,
                    ipc_thread, NULL, NULL, NULL, 6, 0, K_NO_WAIT);
    k_thread_name_set(&ipc_thread_data, "hpi_ipc");
    return 0;
}
