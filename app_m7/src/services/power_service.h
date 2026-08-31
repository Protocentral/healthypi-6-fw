/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Power service (L4) -- periodically samples the MAX17048 fuel gauge and caches
 * battery voltage / current / state-of-charge / charge state. The group-64
 * telemetry handler (0x0030) reads this cache; UI/events consume it later.
 * Non-fatal if the gauge is absent (status.valid = false).
 */

#ifndef HPI_SERVICES_POWER_SERVICE_H
#define HPI_SERVICES_POWER_SERVICE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum hpi_charge_state {
    HPI_CHG_DISCHARGING = 0,
    HPI_CHG_CHARGING    = 1,
    HPI_CHG_FULL        = 2,
    HPI_CHG_FAULT       = 3,
};

struct hpi_power_status {
    bool     valid;        /* false if the fuel gauge couldn't be read */
    uint32_t vbat_mv;
    /* Always 0: nothing on the board measures pack current (the MAX17048 has
     * no coulomb counter). Kept so the group-64 telemetry layout does not
     * change; do not derive anything from it. */
    int32_t  ibat_ma;
    uint32_t soc_pct;
    uint8_t  charge_state; /* enum hpi_charge_state */
    bool     usb_present;
};

int  hpi_power_service_init(void);
void hpi_power_get(struct hpi_power_status *out);

#ifdef __cplusplus
}
#endif

#endif /* HPI_SERVICES_POWER_SERVICE_H */
