/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * HealthyLink GPIO breakout provider (module ID 0x000A).
 *
 * The board is passive: headers for every interface the connector carries
 * (SPI4, SPI6, I2C, FDCAN, ADC, GPIO, StackLink), the slot regulators, and the
 * ID EEPROM. Nothing on it is a device this firmware drives, so this provider
 * exists for one reason -- without a registered provider the framework leaves a
 * detected slot UNSUPPORTED and *unpowered* (healthylink_service.c), and an
 * unpowered breakout cannot supply the thing you plugged into its headers.
 *
 * It therefore claims nothing and starts nothing: probe/start/stop are the
 * minimum that take the slot to ACTIVE and switch its rail on. The pins stay
 * exactly as the devicetree left them (healthylink_pinmux.c gives this module
 * the same treatment as Compute), so whatever you wire to the headers behaves
 * the way the board's own DT says it does -- which is the point of a breakout.
 */

#include "hl_provider.h"

#include <healthylink/healthylink.h>   /* module IDs + capability bits */
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(mod_gpio, CONFIG_HPI_APP_LOG_LEVEL);

static int gpio_probe(struct hl_ctx *ctx)
{
    LOG_INF("GPIO breakout in slot %c: passive, claims no interface",
            ctx->slot == HL_SLOT_A ? 'A' : 'B');
    return 0;
}

static int gpio_start(struct hl_ctx *ctx)
{
    ARG_UNUSED(ctx);
    /* The framework powers the slot around this call; there is nothing to
     * start. The breakout produces no samples, so it publishes nothing. */
    return 0;
}

static int gpio_stop(struct hl_ctx *ctx)
{
    ARG_UNUSED(ctx);
    return 0;
}

static int gpio_selftest(struct hl_ctx *ctx, struct hl_test_result *out)
{
    ARG_UNUSED(ctx);
    /* MANUAL, not PASS: the only thing that could be tested is whatever the
     * user wired to the headers, and reporting PASS for a board we never
     * talk to is the failure mode that makes a suite worse than none. */
    out->status = 3;   /* MANUAL */
    strncpy(out->detail, "passive breakout -- check by hand",
            sizeof(out->detail) - 1);
    out->detail[sizeof(out->detail) - 1] = '\0';
    return 0;
}

HL_MODULE_REGISTER(mod_gpio) = {
    .module_id = HEALTHYLINK_MODULE_ID_GPIO,
    .name      = "GPIO breakout",
    .caps      = 0,   /* claims no interface: see the file comment */
    .probe     = gpio_probe,
    .start     = gpio_start,
    .stop      = gpio_stop,
    .selftest  = gpio_selftest,
    .ctrl      = NULL,
};
