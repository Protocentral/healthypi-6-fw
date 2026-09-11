/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * HealthyLink EEG module provider (ADS1299, 8-ch). Registers in the iterable
 * provider section; the framework starts it when an EEG module is detected.
 * It publishes HPI_CH_EEG frames (hp6_eeg_sample) to the sample bus exactly
 * like the onboard sensors, so EEG reaches stream/recording/UI with zero
 * special-casing.
 *
 * Bus: the ADS1299 shares **SPI4** with the NPU (the arbiter runs one SPI4
 * module at a time). There is one ADS1299 node per slot (`ads1299`,
 * `ads1299_b`; they differ only in reset), and start() brings up the one for
 * the module's slot. The full path (init -> DATA_READY -> read 8 ch -> batch
 * 16 -> publish) compiles only when CONFIG_SENSOR_ADS1299 is enabled AND the
 * slot-A node is okay; otherwise start() logs and no-ops.
 */

#include "hl_provider.h"

#include <healthylink/healthylink.h>   /* module IDs + capability bits */
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(mod_eeg, CONFIG_HPI_APP_LOG_LEVEL);

#define EEG_NODE   DT_NODELABEL(ads1299)
#define EEG_NODE_B DT_NODELABEL(ads1299_b)

/* Active path requires BOTH the node enabled AND the ADS1299 driver built --
 * the node alone is in the base DT as deferred-init even with no board. */
#if DT_NODE_HAS_STATUS(EEG_NODE, okay) && IS_ENABLED(CONFIG_SENSOR_ADS1299)
#define EEG_PRESENT 1
#else
#define EEG_PRESENT 0
#endif

#if EEG_PRESENT
#include "core/sample_bus.h"
#include "core/channel_registry.h"
#include "core/sample_formats.h"
#include "ads1299.h"                   /* SENSOR_CHAN_ADS1299_*, ADS1299_ATTR_* */
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>

#define EEG_BATCH 16

/* The ADS1299 for a slot, or NULL if this board has none there. */
static const struct device *eeg_dev_for(hl_slot_t slot)
{
    switch (slot) {
    case HL_SLOT_A:
        return DEVICE_DT_GET(EEG_NODE);
#if DT_NODE_HAS_STATUS(EEG_NODE_B, okay)
    case HL_SLOT_B:
        return DEVICE_DT_GET(EEG_NODE_B);
#endif
    default:
        return NULL;
    }
}

static const struct device *eeg_dev;   /* set by start(), for the slot in use */
static struct hp6_eeg_sample eeg_batch[EEG_BATCH];
static uint16_t eeg_n;
static uint64_t eeg_t0_us;
static volatile bool eeg_running;

/* DATA_READY trigger handler: read 8 channels, accumulate, publish a frame. */
static void eeg_drdy(const struct device *dev, const struct sensor_trigger *trig)
{
    ARG_UNUSED(trig);
    if (!eeg_running) {
        return;
    }
    if (sensor_sample_fetch_chan(dev, SENSOR_CHAN_ADS1299_ALL) != 0) {
        return;
    }
    if (eeg_n == 0) {
        eeg_t0_us = (uint64_t)k_uptime_get() * 1000ULL;
    }
    struct hp6_eeg_sample *s = &eeg_batch[eeg_n];
    for (int i = 0; i < 8; i++) {
        struct sensor_value v = {0};
        (void)sensor_channel_get(dev, (enum sensor_channel)(SENSOR_CHAN_ADS1299_CH1 + i), &v);
        s->ch[i] = v.val1;     /* microvolts */
    }
    s->lead_off = 0;           /* TODO(7b): fill from LOFF_STAT registers */
    s->_pad[0] = s->_pad[1] = s->_pad[2] = 0;

    if (++eeg_n >= EEG_BATCH) {
        struct hpi_sample_frame f = {
            .channel = HPI_CH_EEG,
            .sample_rate = HPI_EEG_RATE_HZ,
            .sample_count = EEG_BATCH,
            .t_mono_us = eeg_t0_us,
            .len = sizeof(eeg_batch),
            .flags = 0,
            .payload = eeg_batch,
        };
        (void)hpi_bus_publish(&f);
        eeg_n = 0;
    }
}

static int eeg_bringup(void)
{
    if (eeg_dev == NULL) {
        LOG_ERR("EEG: no ADS1299 node for this slot");
        return -ENODEV;
    }
    if (!device_is_ready(eeg_dev)) {
        /* Try the driver's explicit HW init once (deferred-init parts). */
        if (ads1299_hw_init(eeg_dev) != 0 || !device_is_ready(eeg_dev)) {
            LOG_ERR("EEG: ADS1299 not ready");
            return -ENODEV;
        }
    }
    struct sensor_value sr = { .val1 = HPI_EEG_RATE_HZ, .val2 = 0 };
    (void)sensor_attr_set(eeg_dev, SENSOR_CHAN_ADS1299_ALL,
                          (enum sensor_attribute)ADS1299_ATTR_SAMPLE_RATE, &sr);
    struct sensor_value gain = { .val1 = 24, .val2 = 0 };
    (void)sensor_attr_set(eeg_dev, SENSOR_CHAN_ADS1299_ALL,
                          (enum sensor_attribute)ADS1299_ATTR_GAIN, &gain);

    struct sensor_trigger trig = {
        .type = SENSOR_TRIG_DATA_READY,
        .chan = (enum sensor_channel)SENSOR_CHAN_ADS1299_ALL,
    };
    if (sensor_trigger_set(eeg_dev, &trig, eeg_drdy) != 0) {
        LOG_ERR("EEG: trigger set failed");
        return -EIO;
    }
    struct sensor_value start = { .val1 = 1, .val2 = 0 };
    (void)sensor_attr_set(eeg_dev, SENSOR_CHAN_ADS1299_ALL,
                          (enum sensor_attribute)ADS1299_ATTR_START, &start);
    return 0;
}
#endif /* EEG_PRESENT */

static int eeg_probe(struct hl_ctx *ctx)
{
    LOG_INF("EEG probe (slot %c): claiming SPI4", 'A' + ctx->slot);
    return 0;
}

static int eeg_start(struct hl_ctx *ctx)
{
#if EEG_PRESENT
    LOG_INF("EEG start (slot %c): bringing up ADS1299 -> bus", 'A' + ctx->slot);
    eeg_dev = eeg_dev_for(ctx->slot);
    int rc = eeg_bringup();
    if (rc != 0) {
        return rc;
    }
    eeg_n = 0;
    eeg_running = true;
    LOG_INF("EEG: ADS1299 streaming %d Hz -> HPI_CH_EEG", HPI_EEG_RATE_HZ);
    return 0;
#else
    ARG_UNUSED(ctx);
    LOG_WRN("EEG start: no ads1299 DT node (module unavailable) -- "
            "code ready; add the module overlay to activate");
    return 0;
#endif
}

static int eeg_stop(struct hl_ctx *ctx)
{
    ARG_UNUSED(ctx);
#if EEG_PRESENT
    eeg_running = false;
    if (eeg_dev != NULL && device_is_ready(eeg_dev)) {
        struct sensor_value stop = { .val1 = 0, .val2 = 0 };

        (void)sensor_attr_set(eeg_dev, SENSOR_CHAN_ADS1299_ALL,
                              (enum sensor_attribute)ADS1299_ATTR_START, &stop);
    }
#endif
    LOG_INF("EEG stop");
    return 0;
}

static int eeg_selftest(struct hl_ctx *ctx, struct hl_test_result *out)
{
    ARG_UNUSED(ctx);
#if EEG_PRESENT
    bool ready = eeg_dev != NULL && device_is_ready(eeg_dev);
    out->status = ready ? 0 /*PASS*/ : 1 /*FAIL*/;
    strncpy(out->detail, ready ? "ADS1299 ready" : "ADS1299 not ready",
            sizeof(out->detail) - 1);
#else
    out->status = 2;   /* SKIP */
    strncpy(out->detail, "EEG module absent", sizeof(out->detail) - 1);
#endif
    out->detail[sizeof(out->detail) - 1] = '\0';
    return 0;
}

HL_MODULE_REGISTER(mod_eeg) = {
    .module_id = HEALTHYLINK_MODULE_ID_EEG_8CH,
    .name      = "EEG 8ch (ADS1299)",
    .caps      = HEALTHYLINK_CAP_REQUIRES_SPI4 | HEALTHYLINK_CAP_REALTIME_STREAM,
    .probe     = eeg_probe,
    .start     = eeg_start,
    .stop      = eeg_stop,
    .selftest  = eeg_selftest,
    .ctrl      = NULL,
};
