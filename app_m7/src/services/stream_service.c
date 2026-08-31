/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Stream service (L4) -- the single implementation of "streaming".
 *
 * Subscribes to the sample bus and encodes each frame as a .HP6 DBLK block
 * (hp6_frame.h) to a pluggable sink (CDC0 today). Control surfaces
 * (control/mcumgr_hpi, control/shell_hpi) are thin adapters that call
 * hpi_stream_enable/disable/get_status -- they never touch the bus or the wire.
 *
 * A dedicated thread owns the bus subscription so a slow host never blocks
 * producers: the per-consumer ring drops oldest on overflow and we count it.
 */

#include "stream_service.h"
#include "hp6_frame.h"
#include "core/sample_bus.h"
#include "core/channel_registry.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(hpi_stream, CONFIG_HPI_APP_LOG_LEVEL);

/* Largest DBLK we emit: ECG batch 16 x 20 B = 320 + 32 overhead = 352. */
#define STREAM_TX_BUF_SZ 512
#define STREAM_RING_FRAMES 16

static const struct hpi_stream_sink *g_sink;
static struct hpi_bus_sub *g_sub;

static volatile bool     g_active;
static volatile uint8_t  g_ch_mask;     /* HPI_STREAM_CH_* requested by host */
static volatile uint8_t  g_ann_mask;
static uint32_t          g_seq;
static uint32_t          g_frames_sent;
static uint32_t          g_frames_dropped;

/* Auto-stream mode: when 1, the stream follows the CDC0 connection -- opening
 * the port (DTR) starts ECG+PPG with no control-plane command. DISABLED so the
 * device stays silent on CDC0 until an explicit stream_start arrives on the
 * CDC1 control pipe. An explicit stream_start/stop always takes manual control
 * regardless of this default. */
#define STREAM_AUTO_ON_CONNECT 0
static volatile bool     g_auto = (STREAM_AUTO_ON_CONNECT != 0);
#define STREAM_AUTO_DEFAULT_MASK (HPI_STREAM_CH_ECG | HPI_STREAM_CH_PPG)

/* Map a bus channel id to the host-facing HPI_STREAM_CH_* bit so we can honor
 * the requested channel mask. RESP rides inside the ECG frame, so an ECG frame
 * is gated by ECG (resp is a payload field, not a separate bus channel here).
 * VITALS is always streamed while active (low rate, annotations). */
static bool channel_selected(hpi_channel_id_t ch, uint8_t mask)
{
    switch (ch) {
    case HPI_CH_ECG:    return (mask & HPI_STREAM_CH_ECG) != 0;
    case HPI_CH_PPG:    return (mask & HPI_STREAM_CH_PPG) != 0;
    case HPI_CH_EEG:    return (mask & HPI_STREAM_CH_EEG) != 0;
    case HPI_CH_VITALS: return true;   /* always streamed while active */
    default:            return false;
    }
}

/* Resolve the effective active flag + channel mask, honoring auto vs manual. */
static bool stream_effective(uint8_t *mask_out)
{
    if (g_auto) {
        bool connected = (g_sink && g_sink->is_connected && g_sink->is_connected());
        *mask_out = STREAM_AUTO_DEFAULT_MASK;
        return connected;
    }
    *mask_out = g_ch_mask;
    return g_active;
}

/* Encode one bus frame into a DBLK block and hand it to the sink. */
static void emit_frame(const struct hpi_sample_frame *f)
{
    static uint8_t buf[STREAM_TX_BUF_SZ];

    const uint32_t payload_len = f->len;
    const uint32_t block_len = HP6_DBLK_OVERHEAD + payload_len;

    if (block_len > sizeof(buf)) {
        g_frames_dropped++;
        return;   /* should never happen with the canonical batch sizes */
    }

    buf[HP6_DBLK_OFF_MAGIC + 0] = HP6_DBLK_MAGIC0;
    buf[HP6_DBLK_OFF_MAGIC + 1] = HP6_DBLK_MAGIC1;
    buf[HP6_DBLK_OFF_MAGIC + 2] = HP6_DBLK_MAGIC2;
    buf[HP6_DBLK_OFF_MAGIC + 3] = HP6_DBLK_MAGIC3;
    sys_put_le32(block_len,        &buf[HP6_DBLK_OFF_BLOCK_LEN]);
    sys_put_le32(g_seq++,          &buf[HP6_DBLK_OFF_SEQ]);
    sys_put_le64(f->t_mono_us / 1000ULL, &buf[HP6_DBLK_OFF_T_MS]);
    buf[HP6_DBLK_OFF_CHANNEL] = (uint8_t)f->channel;
    buf[HP6_DBLK_OFF_FLAGS]   = (uint8_t)f->flags;
    sys_put_le16(f->sample_count,  &buf[HP6_DBLK_OFF_SAMPLE_COUNT]);
    sys_put_le32(0,                &buf[HP6_DBLK_OFF_RESERVED]);

    if (payload_len > 0 && f->payload != NULL) {
        memcpy(&buf[HP6_DBLK_OFF_PAYLOAD], f->payload, payload_len);
    }

    uint32_t crc = crc32_ieee(buf, HP6_DBLK_HDR_LEN + payload_len);
    sys_put_le32(crc, &buf[HP6_DBLK_HDR_LEN + payload_len]);

    if (g_sink && g_sink->write(buf, block_len)) {
        g_frames_sent++;
    } else {
        g_frames_dropped++;   /* host not reading fast enough / not connected */
    }
}

static void stream_thread(void *a, void *b, void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

    struct hpi_bus_sub_cfg cfg = {
        .name = "stream",
        .channel_mask = HPI_CH_BIT(HPI_CH_ECG) | HPI_CH_BIT(HPI_CH_PPG) |
                        HPI_CH_BIT(HPI_CH_EEG) | HPI_CH_BIT(HPI_CH_VITALS) |
                        HPI_CH_BIT(HPI_CH_INFER),
        .ring_frames = STREAM_RING_FRAMES,
    };
    g_sub = hpi_bus_subscribe(&cfg);
    if (g_sub == NULL) {
        LOG_ERR("stream: bus subscribe failed");
        return;
    }

    bool was_active = false;
    struct hpi_sample_frame f;
    while (1) {
        if (hpi_bus_pull_wait(g_sub, &f, 100) != 0) {
            continue;   /* timeout: loop so state changes take effect promptly */
        }
        /* Always drain; only encode when active + selected so an idle stream
         * doesn't back up the ring. */
        uint8_t mask;
        bool active = stream_effective(&mask);
        if (active != was_active) {
            LOG_INF("stream %s (%s, ch=0x%02x)", active ? "START" : "STOP",
                    g_auto ? "auto/CDC0" : "manual", mask);
            was_active = active;
        }
        if (active && channel_selected(f.channel, mask)) {
            emit_frame(&f);
        }
    }
}

K_THREAD_DEFINE(hpi_stream_tid, 2048, stream_thread, NULL, NULL, NULL,
                6 /* prio */, 0, 0);

int hpi_stream_service_init(const struct hpi_stream_sink *sink)
{
    g_sink = sink;
    LOG_INF("stream service ready (sink=%s)", sink ? sink->name : "none");
    return 0;
}

int hpi_stream_enable(uint8_t ch_mask, uint8_t ann_mask)
{
    /* EEG requested but no module/driver yet -> channel not available. */
    if (ch_mask & HPI_STREAM_CH_EEG) {
        return -ENOTSUP;
    }
    g_auto = false;        /* host takes explicit control */
    g_ch_mask = ch_mask;
    g_ann_mask = ann_mask;
    g_active = true;
    LOG_INF("stream enable (manual): ch=0x%02x ann=0x%02x", ch_mask, ann_mask);
    return 0;
}

void hpi_stream_disable(void)
{
    g_auto = false;        /* explicit stop overrides auto-on-connect */
    g_active = false;
    LOG_INF("stream disable (manual)");
}

void hpi_stream_get_status(struct hpi_stream_status *out)
{
    if (out == NULL) {
        return;
    }
    uint8_t mask;
    out->active         = stream_effective(&mask);
    out->ch_mask        = mask;
    out->ann_mask       = g_ann_mask;
    out->frames_sent    = g_frames_sent;
    out->frames_dropped = g_frames_dropped;
}
