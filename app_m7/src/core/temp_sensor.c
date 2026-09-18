/*
 * Copyright (c) 2026 ProtoCentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * External AS6221 on I2C1 (PB6 SCL / PB7 SDA). ADDR straps 0x48 or 0x49.
 * The tmp108 driver init fails if the probe is unplugged at boot and never
 * retries; this file talks I2C directly so hot-plug still works.
 */

#include "temp_sensor.h"

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>

LOG_MODULE_REGISTER(hpi_temp, CONFIG_HPI_APP_LOG_LEVEL);

#define TEMP_POLL_MS     1000
#define TEMP_STACK_SIZE  1536
#define TEMP_PRIORITY    8
#define AS6221_REG_TEMP  0x00
/* AS6221 ADDR pin: 0x48 or 0x49. Probe both; cache the one that ACKs. */
#define AS6221_ADDR_A    0x48
#define AS6221_ADDR_B    0x49
/* AS6221: 16-bit, 0.0078125 C/LSB == Zephyr AMS_AS6221 TEMP_MULT/DIV. */
#define AS6221_MICRO_NUM 15625
#define AS6221_MICRO_DEN 2

static atomic_t g_temp_x100;
static uint16_t g_addr;
static bool g_logged_ok;
static bool g_logged_fail;
static struct k_thread temp_thread;
static K_THREAD_STACK_DEFINE(temp_stack, TEMP_STACK_SIZE);

#if DT_NODE_EXISTS(DT_NODELABEL(as6221))
static const struct i2c_dt_spec as6221 = I2C_DT_SPEC_GET(DT_NODELABEL(as6221));
#define AS6221_HAVE_SPEC 1
#else
#define AS6221_HAVE_SPEC 0
#endif

static int16_t raw_to_x100(int16_t raw)
{
	int32_t x100 = ((int32_t)raw * AS6221_MICRO_NUM) /
		       (AS6221_MICRO_DEN * 10000);

	if (x100 > INT16_MAX) {
		return INT16_MAX;
	}
	if (x100 < INT16_MIN) {
		return INT16_MIN;
	}
	return (int16_t)x100;
}

static int as6221_read_at(uint16_t addr, int16_t *raw)
{
#if AS6221_HAVE_SPEC
	struct i2c_dt_spec spec = as6221;
	uint8_t reg = AS6221_REG_TEMP;
	uint8_t buf[2];
	int rc;

	spec.addr = addr;
	if (!i2c_is_ready_dt(&spec)) {
		return -ENODEV;
	}
	rc = i2c_write_read_dt(&spec, &reg, 1, buf, sizeof(buf));
	if (rc != 0) {
		return rc;
	}
	*raw = (int16_t)sys_get_be16(buf);
	return 0;
#else
	ARG_UNUSED(addr);
	ARG_UNUSED(raw);
	return -ENODEV;
#endif
}

static int as6221_read(int16_t *raw)
{
	static const uint16_t addrs[] = { AS6221_ADDR_A, AS6221_ADDR_B };
	int last = -ENODEV;

	if (g_addr != 0) {
		int rc = as6221_read_at(g_addr, raw);

		if (rc == 0) {
			return 0;
		}
		g_addr = 0;
		last = rc;
	}
	for (size_t i = 0; i < ARRAY_SIZE(addrs); i++) {
		int rc = as6221_read_at(addrs[i], raw);

		if (rc == 0) {
			g_addr = addrs[i];
			return 0;
		}
		last = rc;
	}
	return last;
}

static void temp_poll_once(void)
{
	int16_t raw = 0;
	int rc = as6221_read(&raw);

	if (rc != 0) {
		atomic_set(&g_temp_x100, 0);
		if (!g_logged_fail) {
			LOG_INF("temp: AS6221 not present (rc=%d) - plug in the probe",
				rc);
			g_logged_fail = true;
			g_logged_ok = false;
		}
		return;
	}

	int16_t x100 = raw_to_x100(raw);

	atomic_set(&g_temp_x100, x100);
	if (!g_logged_ok) {
		int frac = x100 % 100;

		if (frac < 0) {
			frac = -frac;
		}
		LOG_INF("temp: AS6221 @0x%02x %d.%02d C (plug detected)",
			g_addr, x100 / 100, frac);
		g_logged_ok = true;
		g_logged_fail = false;
	}
}

static void temp_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		temp_poll_once();
		k_msleep(TEMP_POLL_MS);
	}
}

int hpi_temp_sensor_init(void)
{
	atomic_set(&g_temp_x100, 0);
#if AS6221_HAVE_SPEC
	LOG_INF("temp: poller start (I2C %s, try 0x%02x then 0x%02x)",
		as6221.bus ? as6221.bus->name : "?", AS6221_ADDR_A, AS6221_ADDR_B);
#else
	LOG_INF("temp: no as6221 node on this board");
#endif
	k_thread_create(&temp_thread, temp_stack, K_THREAD_STACK_SIZEOF(temp_stack),
			temp_thread_fn, NULL, NULL, NULL,
			TEMP_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&temp_thread, "hpi_temp");
	return 0;
}

int16_t hpi_temp_c_x100(void)
{
	return (int16_t)atomic_get(&g_temp_x100);
}
