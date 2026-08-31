/*
 * Copyright (c) 2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * I2C backend for the BMI323 driver (local fork of upstream bosch,bmi323).
 *
 * BMI323 I2C reads return the same 2-byte dummy/status preamble that SPI
 * reads do — confirmed empirically (chip ID 0x43 appears at offset 2). Never
 * point words[] straight at the I2C buffer or every register read is off by
 * 2; read into a local buffer, drop the first two bytes, and copy the rest
 * into the caller's word array.
 */

#include "bmi323.h"
#include "bmi323_i2c.h"

#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/__assert.h>

BUILD_ASSERT(sizeof(uint16_t) == 2, "BMI323 I2C backend assumes 16-bit words");

#define BMI323_I2C_DUMMY_BYTES 2
/* Largest single read in this driver is 3 words; 16 gives ample headroom. */
#define BMI323_I2C_MAX_WORDS_PER_READ 16

static int bosch_bmi323_i2c_read_words(const void *context, uint8_t offset, uint16_t *words,
				       uint16_t words_count)
{
	const struct i2c_dt_spec *i2c = (const struct i2c_dt_spec *)context;
	uint8_t buf[BMI323_I2C_DUMMY_BYTES +
		    BMI323_I2C_MAX_WORDS_PER_READ * sizeof(uint16_t)];
	const size_t payload = (size_t)words_count * sizeof(uint16_t);
	int ret;

	__ASSERT_NO_MSG(words_count <= BMI323_I2C_MAX_WORDS_PER_READ);

	ret = i2c_write_read_dt(i2c, &offset, sizeof(offset), buf,
				BMI323_I2C_DUMMY_BYTES + payload);
	if (ret == 0) {
		memcpy(words, &buf[BMI323_I2C_DUMMY_BYTES], payload);
	}

	k_usleep(2);

	return ret;
}

static int bosch_bmi323_i2c_write_words(const void *context, uint8_t offset, uint16_t *words,
					uint16_t words_count)
{
	const struct i2c_dt_spec *i2c = (const struct i2c_dt_spec *)context;
	int ret;

	ret = i2c_burst_write_dt(i2c, offset, (const uint8_t *)words,
				 (size_t)words_count * sizeof(uint16_t));

	k_usleep(2);

	return ret;
}

static int bosch_bmi323_i2c_init(const void *context)
{
	const struct i2c_dt_spec *i2c = (const struct i2c_dt_spec *)context;
	uint16_t sensor_id;
	int ret;

	if (!i2c_is_ready_dt(i2c)) {
		return -ENODEV;
	}

	/* Upstream SPI init performs a dummy chip-ID read; mirror that so the
	 * subsequent reset+ID verification in bmi323.c behaves identically.
	 */
	ret = bosch_bmi323_i2c_read_words(i2c, 0, &sensor_id, 1);
	if (ret < 0) {
		return ret;
	}

	k_usleep(1500);

	return 0;
}

const struct bosch_bmi323_bus_api bosch_bmi323_i2c_bus_api = {
	.read_words = bosch_bmi323_i2c_read_words,
	.write_words = bosch_bmi323_i2c_write_words,
	.init = bosch_bmi323_i2c_init,
};
