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
#include <zephyr/init.h>
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

/*
 * Both pins are OPEN-DRAIN on the charger and this board fits NO external
 * pull-ups, so the MCU's internal pull-up is the only thing holding the line
 * up when the part releases it. That makes GPIO_PULL_UP in the DTS
 * load-bearing rather than defensive: drop it and both signals float, read as
 * noise, and the device reports charging at random. It also means the pins
 * must be configured before the first read -- see power_pins_init().
 */
static bool pin_asserted(const struct gpio_dt_spec *g)
{
    /* gpio_pin_get_dt() already applies ACTIVE_LOW, so 1 == asserted. */
    return g->port != NULL && gpio_pin_get_dt(g) == 1;
}

/* Charge state, measured from the BQ24074 pins rather than inferred (PGOOD
 * also sees a dumb wall charger, which USB enumeration never can).
 *
 * The truth table, straight off the part's two open-drain outputs:
 *
 *   PGOOD  CHG  meaning                                   reported
 *   -----  ---  ----------------------------------------  --------------
 *   no     x    no valid input: running off the cell      DISCHARGING
 *   yes    yes  a charge cycle is running                 CHARGING
 *   yes    no   input present, no cycle: termination,     FULL if the gauge
 *               or suspended (timer/thermal/no battery)   agrees, else
 *                                                         DISCHARGING
 *
 * PGOOD asserted with CHG deasserted is genuinely ambiguous -- the part does
 * not distinguish "finished" from "gave up" on these two pins -- so only
 * termination is reported as FULL, and only when the gauge agrees.
 *
 * `soc_valid` gates FULL and nothing else. It deliberately does NOT feed the
 * result otherwise: the charger's state is a property of the charger, and a
 * fuel gauge that will not answer says nothing about whether current is
 * flowing into the cell. This used to return HPI_CHG_FAULT whenever the gauge
 * read failed, which both mislabelled a gauge problem as a charger fault and
 * -- once the status bar grew a charge bolt -- hid the charging indication on
 * a unit whose gauge was merely unhappy. Gauge health travels in `valid`.
 */
static uint8_t derive_charge_state(bool soc_valid, uint32_t soc, bool pgood, bool chg)
{
    if (!pgood) {
        return HPI_CHG_DISCHARGING;
    }
    if (chg) {
        return HPI_CHG_CHARGING;
    }
    return (soc_valid && soc >= 95U) ? HPI_CHG_FULL : HPI_CHG_DISCHARGING;
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
        /* Enumeration, kept separate from the supply above -- see the header.
         * Reported unconditionally, so a host that is plainly connected is
         * still visible on a board whose PGOOD never asserts. */
        g_status.usb_attached = hpi_usb_attached();
        k_mutex_unlock(&g_lock);

        /* Report a charge-state change whether or not the gauge answered.
         * This used to sit inside `if (ok)`, so on a unit with an unhappy
         * gauge the pgood/chg values -- the only view of the charger this
         * board has -- were never printed at all, and "no input power" was
         * indistinguishable from "no fuel gauge" in the log. */
        if (cs != last_state) {
            if (ok) {
                LOG_INF("battery: %u mV, %u%%, pgood=%d chg=%d, %s",
                        vbat, soc, (int)pgood, (int)chg,
                        cs == HPI_CHG_FULL ? "full" :
                        cs == HPI_CHG_CHARGING ? "charging" : "discharging");
            } else {
                LOG_INF("battery: gauge unavailable, pgood=%d chg=%d, %s",
                        (int)pgood, (int)chg,
                        cs == HPI_CHG_CHARGING ? "charging" : "not charging");
            }
            last_state = cs;
        }
        if (ok) {
            logged_fault = false;
        } else if (!logged_fault) {
            /* Say so once per fault episode. A silent gauge is why the UI shows
             * "--", and without this the only clue was the absence of a line. */
            LOG_WRN("fuel gauge read failed (%d); battery shows unavailable", rc);
            logged_fault = true;
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

/*
 * Configure the charger pins at POST_KERNEL, which runs BEFORE static threads
 * are started -- power_thread() is one, and it reads these pins on its first
 * pass. Doing it from hpi_power_service_init() (called from main) left a window
 * in which the thread could sample them unconfigured, and with no external
 * pull-ups on this board an unconfigured input floats rather than resting high:
 * the reading was not merely stale, it was undefined, and "charging" is one of
 * the values it could invent.
 */
static int power_pins_init(void)
{
    status_pin_init(&pgood_gpio, "PGOOD");
    status_pin_init(&chg_gpio, "CHG");
    return 0;
}
SYS_INIT(power_pins_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

int hpi_power_service_init(void)
{
    k_mutex_init(&g_lock);
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
