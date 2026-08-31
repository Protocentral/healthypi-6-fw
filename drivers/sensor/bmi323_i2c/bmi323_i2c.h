/*
 * Copyright (c) 2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 */

#ifndef ZEPHYR_DRIVERS_SENSOR_BMI323_I2C_BMI323_I2C_H_
#define ZEPHYR_DRIVERS_SENSOR_BMI323_I2C_BMI323_I2C_H_

#include "bmi323.h"
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>

#define BMI323_DEVICE_I2C_BUS(inst)                                                                \
	extern const struct bosch_bmi323_bus_api bosch_bmi323_i2c_bus_api;                         \
                                                                                                   \
	static const struct i2c_dt_spec i2c_spec##inst = I2C_DT_SPEC_INST_GET(inst);               \
                                                                                                   \
	static const struct bosch_bmi323_bus bosch_bmi323_bus_api##inst = {                        \
		.context = &i2c_spec##inst, .api = &bosch_bmi323_i2c_bus_api}

#endif /* ZEPHYR_DRIVERS_SENSOR_BMI323_I2C_BMI323_I2C_H_ */
