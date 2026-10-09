/*
 * Copyright (c) 2026 ProtoCentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * BMI323 6-axis artifact flag. Same getter pattern as hpi_temp_c_x100():
 * platform/ipc ORs HP6_VIT_MOTION into hp6_vitals.flags. Flag only — do not
 * hide HR/SpO2. No HPI_CH_IMU (that would be format 0x0400).
 */

#ifndef HPI_CORE_IMU_H
#define HPI_CORE_IMU_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

int hpi_imu_init(void);

/* True while RMS accel is above the motion threshold. */
bool hpi_imu_motion(void);

#ifdef __cplusplus
}
#endif

#endif /* HPI_CORE_IMU_H */
