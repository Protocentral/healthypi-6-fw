/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Sample bus -- Zephyr fan-out over per-consumer frame_rings.
 *
 * Producers call hpi_bus_publish(); the frame is copied into every matching
 * consumer's ring under a spinlock (never blocks the producer) and each such
 * consumer's signal semaphore is given. Consumers pull at their own pace; a
 * slow consumer only drops its own frames (counted), never back-pressuring
 * the producer or other consumers.
 *
 * NOTE (memory): ring storage is k_malloc'd from the system heap; if RAM
 * tightens, move the ring pool into SDRAM. The ring logic itself lives in
 * frame_ring.h.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "sample_bus.h"
#include "frame_ring.h"

LOG_MODULE_REGISTER(hpi_bus, CONFIG_HPI_APP_LOG_LEVEL);

#ifndef CONFIG_HPI_BUS_MAX_SUBS
#define CONFIG_HPI_BUS_MAX_SUBS 8
#endif

struct hpi_bus_sub {
    bool                    in_use;
    const char             *name;
    uint32_t                channel_mask;
    struct hpi_frame_ring   ring;
    struct hpi_frame_stored *slots;   /* k_malloc'd, ring.capacity entries */
    struct hpi_frame_stored  last;    /* holding buffer: borrowed by pull()  */
    struct k_sem            signal;   /* binary: data-available for pull_wait */
    uint32_t                delivered;
};

static struct hpi_bus_sub g_subs[CONFIG_HPI_BUS_MAX_SUBS];
static struct k_spinlock  g_lock;
static bool               g_inited;

/* Frames whose payload exceeded HPI_FRAME_MAX_PAYLOAD and were truncated. A
 * build-configuration fault rather than a runtime one, so it is a plain counter
 * rather than per-consumer: if it is ever non-zero, every consumer of that
 * channel has been fed corrupt frames. */
static uint32_t g_oversize_frames;

int hpi_bus_init(void)
{
    k_spinlock_key_t key = k_spin_lock(&g_lock);
    for (int i = 0; i < CONFIG_HPI_BUS_MAX_SUBS; i++) {
        g_subs[i].in_use = false;
    }
    g_inited = true;
    k_spin_unlock(&g_lock, key);
    LOG_INF("sample bus ready (max %d subscribers)", CONFIG_HPI_BUS_MAX_SUBS);
    return 0;
}

int hpi_bus_publish(const struct hpi_sample_frame *frame)
{
    if (!g_inited || frame == NULL) {
        return -EINVAL;
    }

    /* Build the stored (payload-copied) frame once. */
    struct hpi_frame_stored st;
    st.channel      = frame->channel;
    st.sample_rate  = frame->sample_rate;
    st.sample_count = frame->sample_count;
    st.t_mono_us    = frame->t_mono_us;
    st.flags        = frame->flags;
    uint16_t len    = frame->len;
    if (len > HPI_FRAME_MAX_PAYLOAD) {
        /* Truncation corrupts the frame -- a consumer decodes whatever
         * survived as if it were whole -- so it must never be silent. Nothing
         * trips it today (widest real payload: EEG 16 x 36 = 576 B vs the
         * 640 B cap), but a bigger batch or a wider sample would. */
        LOG_ERR("publish ch%u: payload %u B exceeds HPI_FRAME_MAX_PAYLOAD (%d) "
                "-- TRUNCATED; raise the cap or shrink the batch",
                frame->channel, len, HPI_FRAME_MAX_PAYLOAD);
        g_oversize_frames++;
        len = HPI_FRAME_MAX_PAYLOAD;
    }
    st.len = len;
    if (len && frame->payload) {
        memcpy(st.data, frame->payload, len);
    }

    const uint32_t bit = HPI_CH_BIT(frame->channel);
    int delivered = 0;
    bool signal[CONFIG_HPI_BUS_MAX_SUBS] = { false };

    k_spinlock_key_t key = k_spin_lock(&g_lock);
    for (int i = 0; i < CONFIG_HPI_BUS_MAX_SUBS; i++) {
        struct hpi_bus_sub *s = &g_subs[i];
        if (!s->in_use || !(s->channel_mask & bit)) {
            continue;
        }
        (void)hpi_fr_push(&s->ring, &st);  /* drop-on-full counted inside */
        s->delivered++;
        signal[i] = true;
        delivered++;
    }
    k_spin_unlock(&g_lock, key);

    /* Signal outside the lock. */
    for (int i = 0; i < CONFIG_HPI_BUS_MAX_SUBS; i++) {
        if (signal[i]) {
            k_sem_give(&g_subs[i].signal);
        }
    }
    return delivered;
}

struct hpi_bus_sub *hpi_bus_subscribe(const struct hpi_bus_sub_cfg *cfg)
{
    if (!g_inited || cfg == NULL || cfg->ring_frames == 0U) {
        return NULL;
    }

    struct hpi_frame_stored *slots =
        k_malloc((size_t)cfg->ring_frames * sizeof(struct hpi_frame_stored));
    if (slots == NULL) {
        LOG_ERR("subscribe '%s': out of heap for %u frames",
                cfg->name ? cfg->name : "?", cfg->ring_frames);
        return NULL;
    }

    struct hpi_bus_sub *s = NULL;
    k_spinlock_key_t key = k_spin_lock(&g_lock);
    for (int i = 0; i < CONFIG_HPI_BUS_MAX_SUBS; i++) {
        if (!g_subs[i].in_use) {
            s = &g_subs[i];
            break;
        }
    }
    if (s != NULL) {
        s->in_use = true;
        s->name = cfg->name;
        s->channel_mask = cfg->channel_mask;
        s->slots = slots;
        s->delivered = 0;
        hpi_fr_init(&s->ring, slots, cfg->ring_frames);
        k_sem_init(&s->signal, 0, 1);
    }
    k_spin_unlock(&g_lock, key);

    if (s == NULL) {
        k_free(slots);
        LOG_ERR("subscribe '%s': no free slot (max %d)",
                cfg->name ? cfg->name : "?", CONFIG_HPI_BUS_MAX_SUBS);
        return NULL;
    }
    LOG_INF("subscribe '%s': mask 0x%08x, ring %u",
            s->name ? s->name : "?", s->channel_mask, cfg->ring_frames);
    return s;
}

void hpi_bus_unsubscribe(struct hpi_bus_sub *sub)
{
    if (sub == NULL) {
        return;
    }
    k_spinlock_key_t key = k_spin_lock(&g_lock);
    sub->in_use = false;
    struct hpi_frame_stored *slots = sub->slots;
    sub->slots = NULL;
    k_spin_unlock(&g_lock, key);
    k_free(slots);
}

int hpi_bus_pull(struct hpi_bus_sub *sub, struct hpi_sample_frame *out)
{
    if (sub == NULL || out == NULL) {
        return -EINVAL;
    }
    bool more;
    bool got;
    k_spinlock_key_t key = k_spin_lock(&g_lock);
    got = hpi_fr_pop(&sub->ring, &sub->last);
    more = !hpi_fr_empty(&sub->ring);
    k_spin_unlock(&g_lock, key);

    if (!got) {
        return -EAGAIN;
    }
    if (more) {
        k_sem_give(&sub->signal);   /* keep the data-available signal latched */
    }
    out->channel      = sub->last.channel;
    out->sample_rate  = sub->last.sample_rate;
    out->sample_count = sub->last.sample_count;
    out->t_mono_us    = sub->last.t_mono_us;
    out->len          = sub->last.len;
    out->flags        = sub->last.flags;
    out->payload      = sub->last.data;   /* valid until next pull on this sub */
    return 0;
}

int hpi_bus_pull_wait(struct hpi_bus_sub *sub, struct hpi_sample_frame *out,
                      uint32_t timeout_ms)
{
    if (sub == NULL || out == NULL) {
        return -EINVAL;
    }
    int rc = hpi_bus_pull(sub, out);
    if (rc == 0) {
        return 0;
    }
    if (k_sem_take(&sub->signal, K_MSEC(timeout_ms)) != 0) {
        return -EAGAIN;
    }
    return hpi_bus_pull(sub, out);
}

void hpi_bus_sub_get_stats(const struct hpi_bus_sub *sub,
                           struct hpi_bus_sub_stats *out)
{
    if (sub == NULL || out == NULL) {
        return;
    }
    k_spinlock_key_t key = k_spin_lock(&g_lock);
    out->frames_delivered = sub->delivered;
    out->frames_dropped   = sub->ring.dropped;
    out->ring_high_water  = sub->ring.high_water;
    k_spin_unlock(&g_lock, key);
}
