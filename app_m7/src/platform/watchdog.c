/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 */

#include <stdbool.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include "watchdog.h"

LOG_MODULE_REGISTER(hpi_wdt, CONFIG_HPI_APP_LOG_LEVEL);

/* Resolve a usable watchdog node: prefer the `watchdog0` alias (added by the
 * app overlay), fall back to the STM32 IWDG node label. If neither is
 * enabled, the module compiles to no-ops so bring-up still boots. */
#if DT_NODE_HAS_STATUS(DT_ALIAS(watchdog0), okay)
#define HPI_WDT_NODE DT_ALIAS(watchdog0)
#define HPI_HAVE_WDT 1
#elif DT_NODE_HAS_STATUS(DT_NODELABEL(iwdg1), okay)
#define HPI_WDT_NODE DT_NODELABEL(iwdg1)
#define HPI_HAVE_WDT 1
#else
#define HPI_HAVE_WDT 0
#endif

#if HPI_HAVE_WDT && defined(CONFIG_WATCHDOG)
#include <zephyr/drivers/watchdog.h>

static const struct device *const wdt_dev = DEVICE_DT_GET(HPI_WDT_NODE);
static int wdt_channel = -1;

int platform_watchdog_init(void)
{
    if (!device_is_ready(wdt_dev)) {
        LOG_WRN("watchdog device not ready; running without IWDG");
        return -ENODEV;
    }

    struct wdt_timeout_cfg cfg = {
        .flags = WDT_FLAG_RESET_SOC,
        .window = { .min = 0U, .max = CONFIG_HPI_WATCHDOG_TIMEOUT_MS },
        .callback = NULL,
    };

    wdt_channel = wdt_install_timeout(wdt_dev, &cfg);
    if (wdt_channel < 0) {
        LOG_ERR("wdt_install_timeout failed (%d)", wdt_channel);
        return wdt_channel;
    }

    int rc = wdt_setup(wdt_dev, WDT_OPT_PAUSE_HALTED_BY_DBG);
    if (rc < 0) {
        LOG_ERR("wdt_setup failed (%d)", rc);
        return rc;
    }

    LOG_INF("IWDG armed: timeout %d ms (ch %d)",
            CONFIG_HPI_WATCHDOG_TIMEOUT_MS, wdt_channel);
    return 0;
}

void platform_watchdog_feed(void)
{
    if (wdt_channel >= 0) {
        (void)wdt_feed(wdt_dev, wdt_channel);
    }
}

bool platform_watchdog_active(void)
{
    return wdt_channel >= 0;
}

#else  /* no watchdog node */

int platform_watchdog_init(void)
{
    LOG_WRN("no enabled watchdog node; IWDG disabled for bring-up");
    return -ENODEV;
}
void platform_watchdog_feed(void) { }
bool platform_watchdog_active(void) { return false; }

#endif
