/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * On-device sample-bus self-test (dev builds only, CONFIG_HPI_BUS_SELFTEST).
 *
 * Runs once at boot: subscribes a temporary consumer, publishes a known
 * sequence, and verifies fan-out filtering, FIFO order, payload integrity,
 * and drop-on-full counting, logging a single PASS/FAIL line plus stats.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "sample_bus.h"
#include "bus_selftest.h"

LOG_MODULE_REGISTER(hpi_bus_test, CONFIG_HPI_APP_LOG_LEVEL);

static int publish_tag(hpi_channel_id_t ch, uint32_t tag)
{
    struct hpi_sample_frame f = {
        .channel = ch,
        .sample_rate = 500,
        .sample_count = 1,
        .t_mono_us = k_uptime_get() * 1000ULL,
        .len = sizeof(tag),
        .flags = 0,
        .payload = &tag,
    };
    return hpi_bus_publish(&f);
}

void hpi_bus_selftest_run(void)
{
    const uint32_t mask = HPI_CH_BIT(HPI_CH_ECG) | HPI_CH_BIT(HPI_CH_PPG);
    struct hpi_bus_sub_cfg cfg = {
        .name = "selftest",
        .channel_mask = mask,
        .ring_frames = 4,
    };
    struct hpi_bus_sub *sub = hpi_bus_subscribe(&cfg);
    if (sub == NULL) {
        LOG_ERR("bus selftest: subscribe failed -> FAIL");
        return;
    }

    int fails = 0;
    struct hpi_sample_frame out;

    /* Phase A: fan-out filter (RESP must be ignored) + FIFO + payload. */
    publish_tag(HPI_CH_ECG, 10);
    publish_tag(HPI_CH_PPG, 20);
    publish_tag(HPI_CH_RESP, 99);   /* not in mask -> must be dropped at fan-out */
    publish_tag(HPI_CH_ECG, 11);

    uint32_t expect[] = {10, 20, 11};
    int got = 0;
    while (hpi_bus_pull(sub, &out) == 0 && got < (int)ARRAY_SIZE(expect)) {
        uint32_t tag = *(const uint32_t *)out.payload;
        if (out.channel == HPI_CH_RESP) { fails++; LOG_ERR("  RESP leaked through filter"); }
        if (tag != expect[got]) { fails++; LOG_ERR("  order/payload mismatch: got %u want %u", tag, expect[got]); }
        got++;
    }
    if (got != 3) { fails++; LOG_ERR("  phase A: pulled %d, expected 3", got); }
    if (hpi_bus_pull(sub, &out) == 0) { fails++; LOG_ERR("  phase A: ring not empty after drain"); }

    /* Phase B: overflow -> drop oldest, count drops. Ring depth 4, push 10. */
    for (uint32_t i = 0; i < 10; i++) {
        publish_tag(HPI_CH_ECG, 100 + i);
    }
    uint32_t bexp[] = {106, 107, 108, 109};   /* newest 4 survive */
    got = 0;
    while (hpi_bus_pull(sub, &out) == 0 && got < (int)ARRAY_SIZE(bexp)) {
        uint32_t tag = *(const uint32_t *)out.payload;
        if (tag != bexp[got]) { fails++; LOG_ERR("  phase B: got %u want %u", tag, bexp[got]); }
        got++;
    }
    if (got != 4) { fails++; LOG_ERR("  phase B: pulled %d, expected 4", got); }

    struct hpi_bus_sub_stats st;
    hpi_bus_sub_get_stats(sub, &st);
    /* delivered = 2 ECG + 1 PPG (phase A) + 10 ECG (phase B) = 13. dropped = 6. */
    if (st.frames_dropped != 6) { fails++; LOG_ERR("  dropped=%u, expected 6", st.frames_dropped); }
    if (st.ring_high_water != 4) { fails++; LOG_ERR("  high_water=%u, expected 4", st.ring_high_water); }

    LOG_INF("bus selftest: delivered=%u dropped=%u high_water=%u -> %s",
            st.frames_delivered, st.frames_dropped, st.ring_high_water,
            fails ? "FAIL" : "PASS");

    hpi_bus_unsubscribe(sub);
}
