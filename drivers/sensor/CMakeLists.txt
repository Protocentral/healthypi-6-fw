# Copyright (c) 2021 Nordic Semiconductor ASA
# SPDX-License-Identifier: Apache-2.0

add_subdirectory_ifdef(CONFIG_SENSOR_ADS129XX  ads129xx)
add_subdirectory_ifdef(CONFIG_SENSOR_ADS1299 ads1299)
add_subdirectory_ifdef(CONFIG_SENSOR_MAX32664A max32664a)
add_subdirectory_ifdef(CONFIG_SENSOR_AFE4400 afe4400)
add_subdirectory_ifdef(CONFIG_BMI323_I2C bmi323_i2c)