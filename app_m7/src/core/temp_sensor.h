/*
 * Copyright (c) 2026 ProtoCentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * External AS6221 skin-temperature probe (I2C2). Same getter pattern as
 * hpi_acquisition_lead_off(): platform/ipc copies this into hp6_vitals.
 * 0 means the probe is unplugged or not ready — not a measured zero.
 */

#ifndef HPI_CORE_TEMP_SENSOR_H
#define HPI_CORE_TEMP_SENSOR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int hpi_temp_sensor_init(void);

/* Latest reading in °C × 100, or 0 if unavailable. */
int16_t hpi_temp_c_x100(void);

#ifdef __cplusplus
}
#endif

#endif /* HPI_CORE_TEMP_SENSOR_H */
