/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Power service -- MAX17048 fuel-gauge monitor. See power_service.h.
 */

#include "power_service.h"
#include "transport/usb_composite/usbd.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/fuel_gauge.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(hpi_power, CONFIG_HPI_APP_LOG_LEVEL);

#define POWER_POLL_MS 2000

static const struct device *const fg_dev =
    DEVICE_DT_GET_OR_NULL(DT_NODELABEL(max17048));

static struct hpi_power_status g_status;
static struct k_mutex g_lock;

/* Read V/SoC from the fuel gauge. Returns 0 on success.
 *
 * Voltage and state-of-charge ONLY: the MAX17048 has no coulomb counter, and
 * its Zephyr driver returns -ENOTSUP for anything but VOLTAGE,
 * RELATIVE_STATE_OF_CHARGE and the two RUNTIME props. Never add an unsupported
 * property here -- fuel_gauge_get_props() stops at the first failing property,
 * so one -ENOTSUP aborts the whole read and `valid` is never set.
 *
 * MAX17048 reports voltage in µV on some HALs and mV on others; normalise. */
static int read_gauge(uint32_t *vbat_mv, uint32_t *soc)
{
    *vbat_mv = 0; *soc = 0;

    if (fg_dev == NULL || !device_is_ready(fg_dev)) {
        return -ENODEV;
    }
    fuel_gauge_prop_t props[] = {
        FUEL_GAUGE_VOLTAGE,
        FUEL_GAUGE_RELATIVE_STATE_OF_CHARGE,
    };
    union fuel_gauge_prop_val vals[ARRAY_SIZE(props)];
    int rc = fuel_gauge_get_props(fg_dev, props, vals, ARRAY_SIZE(props));
    if (rc < 0) {
        return rc;
    }

    uint32_t v = vals[0].voltage;       /* µV (or mV) */
    if (v > 100000U) { v /= 1000U; }
    *vbat_mv = v;

    *soc = vals[1].relative_state_of_charge;
    return 0;
}

/* BQ24074 status pins -- the only thing this charger tells the MCU (it has no
 * register interface). CHG and PGOOD are open-drain, active low, declared
 * under zephyr,user in the board DTS; absent (port == NULL) on a board that
 * does not wire them, in which case nothing is claimed about charging.
 *
 * Charge current is NOT here: R_ISET and the EN1/EN2 strap fix it in hardware.
 * Firmware cannot raise the charge rate -- its only lever is drawing less
 * itself (power-path feeds the system first; backlight is the largest term). */
static const struct gpio_dt_spec chg_gpio =
    GPIO_DT_SPEC_GET_OR(DT_PATH(zephyr_user), chg_gpios, {0});
static const struct gpio_dt_spec pgood_gpio =
    GPIO_DT_SPEC_GET_OR(DT_PATH(zephyr_user), pgood_gpios, {0});

static bool pin_asserted(const struct gpio_dt_spec *g)
{
    /* gpio_pin_get_dt() already applies ACTIVE_LOW, so 1 == asserted. */
    return g->port != NULL && gpio_pin_get_dt(g) == 1;
}

/* Charge state, measured from the BQ24074 pins rather than inferred (PGOOD
 * also sees a dumb wall charger, which USB enumeration never can).
 *
 * PGOOD asserted with CHG deasserted means input power present but no charge
 * cycle running -- either termination (cell full) or the safety timer / a
 * fault. Only termination is reported as FULL, and only when the gauge
 * agrees; otherwise the honest answer is "not charging". */
static uint8_t derive_charge_state(bool ok, uint32_t soc, bool pgood, bool chg)
{
    if (!ok) {
        return HPI_CHG_FAULT;
    }
    if (!pgood) {
        return HPI_CHG_DISCHARGING;
    }
    if (chg) {
        return HPI_CHG_CHARGING;
    }
    return (soc >= 95U) ? HPI_CHG_FULL : HPI_CHG_DISCHARGING;
}

static void power_thread(void *a, void *b, void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

    if (fg_dev == NULL) {
        LOG_WRN("no fuel gauge in DT; battery telemetry unavailable");
    } else if (!device_is_ready(fg_dev)) {
        LOG_WRN("fuel gauge not ready; battery telemetry unavailable");
    }

    uint8_t last_state = 0xFF;
    bool logged_fault = false;

    while (1) {
        uint32_t vbat = 0, soc = 0;
        int rc = read_gauge(&vbat, &soc);
        bool ok = (rc == 0);
        bool pgood = pin_asserted(&pgood_gpio);
        bool chg = pin_asserted(&chg_gpio);
        /* Input power present, from the charger -- not USB enumeration, which
         * only sees a host that talks to us. */
        bool usb = pgood_gpio.port ? pgood : hpi_usb_attached();
        uint8_t cs = derive_charge_state(ok, soc, pgood, chg);

        k_mutex_lock(&g_lock, K_FOREVER);
        g_status.valid = ok;
        g_status.vbat_mv = vbat;
        g_status.ibat_ma = 0;   /* not measurable on this part -- see read_gauge */
        g_status.soc_pct = soc;
        g_status.charge_state = cs;
        /* Measured (PGOOD), not inferred from charge_state. */
        g_status.usb_present = usb;
        k_mutex_unlock(&g_lock);

        if (ok) {
            logged_fault = false;
            if (cs != last_state) {
                LOG_INF("battery: %u mV, %u%%, pgood=%d chg=%d, %s",
                        vbat, soc, (int)pgood, (int)chg,
                        cs == HPI_CHG_FULL ? "full" :
                        cs == HPI_CHG_CHARGING ? "charging" : "discharging");
                last_state = cs;
            }
        } else {
            /* Say so once per fault episode. A silent gauge is why the UI shows
             * "--", and without this the only clue was the absence of a line. */
            last_state = 0xFF;
            if (!logged_fault) {
                LOG_WRN("fuel gauge read failed (%d); battery shows unavailable", rc);
                logged_fault = true;
            }
        }
        k_msleep(POWER_POLL_MS);
    }
}

K_THREAD_DEFINE(hpi_power_tid, 1536, power_thread, NULL, NULL, NULL,
                10 /* low prio */, 0, 0);

/* Configure a charger status pin as an input. Non-fatal: a board that does not
 * wire it just loses that half of the reading. */
static void status_pin_init(const struct gpio_dt_spec *g, const char *name)
{
    if (g->port == NULL) {
        LOG_INF("charger %s not wired on this board", name);
        return;
    }
    if (!gpio_is_ready_dt(g)) {
        LOG_WRN("charger %s GPIO not ready", name);
        return;
    }
    int rc = gpio_pin_configure_dt(g, GPIO_INPUT);

    if (rc < 0) {
        LOG_WRN("charger %s configure failed (%d)", name, rc);
    }
}

int hpi_power_service_init(void)
{
    k_mutex_init(&g_lock);
    status_pin_init(&pgood_gpio, "PGOOD");
    status_pin_init(&chg_gpio, "CHG");
    LOG_INF("power service ready (%s, charger status %s)",
            fg_dev ? "MAX17048" : "no fuel gauge",
            pgood_gpio.port ? "BQ24074 PGOOD/CHG" : "none");
    return 0;
}

void hpi_power_get(struct hpi_power_status *out)
{
    if (!out) {
        return;
    }
    k_mutex_lock(&g_lock, K_FOREVER);
    *out = g_status;
    k_mutex_unlock(&g_lock);
}
