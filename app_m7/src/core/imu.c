/*
 * Copyright (c) 2026 ProtoCentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Poll BMI323 accel (I2C2 @ 0x69 on v5) and set a motion flag when |a|
 * leaves the learned rest magnitude by ~25%. The local I2C fork reports
 * accel in g (not Zephyr m/s^2); thresholds are relative so either scale
 * works. Timer + getter — no sample-bus ring. Gyro off. Missing chip is
 * non-fatal (flag stays clear).
 */

#include "imu.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(hpi_imu, CONFIG_HPI_APP_LOG_LEVEL);

#define IMU_STACK_SIZE   2048
#define IMU_PRIORITY     8
#define IMU_POLL_MS      20          /* match 50 Hz ODR */
#define IMU_ODR_HZ       50
#define IMU_FS_G         4
#define IMU_STATS_MS     5000
#define IMU_LEARN_N      25          /* 0.5 s at 50 Hz */
#define IMU_ON_PCT       25          /* |mag-rest| > 25% of rest */
#define IMU_OFF_PCT      12
#define IMU_ON_RUN       3           /* 60 ms */
#define IMU_OFF_RUN      15          /* 300 ms */
#define IMU_REST_MIN     100         /* refuse motion if rest mag is nonsense */

static atomic_t g_motion;
static uint8_t g_on_run;
static uint8_t g_off_run;
static uint8_t g_learn_n;
static int64_t g_learn_sum;
static int32_t g_rest;
static int32_t g_dyn;
static int32_t g_ax;
static int32_t g_ay;
static int32_t g_az;
static int32_t g_mag;
static int64_t g_stats_ms;
static struct k_thread imu_thread;
static K_THREAD_STACK_DEFINE(imu_stack, IMU_STACK_SIZE);

#if DT_NODE_EXISTS(DT_NODELABEL(bmi323))
static const struct device *const bmi323 = DEVICE_DT_GET(DT_NODELABEL(bmi323));
#define IMU_HAVE_DT 1
#else
#define IMU_HAVE_DT 0
#endif

static uint32_t isqrt_u64(uint64_t x)
{
	uint64_t op = x;
	uint64_t res = 0;
	uint64_t one = (uint64_t)1 << 62;

	while (one > op) {
		one >>= 2;
	}
	while (one != 0U) {
		if (op >= res + one) {
			op -= res + one;
			res = (res >> 1) + one;
		} else {
			res >>= 1;
		}
		one >>= 2;
	}
	return (uint32_t)res;
}

static int imu_configure(const struct device *dev)
{
	struct sensor_value odr = { .val1 = IMU_ODR_HZ, .val2 = 0 };
	struct sensor_value fs = { .val1 = IMU_FS_G, .val2 = 0 };
	struct sensor_value on = { .val1 = 1, .val2 = 0 };
	int rc;

	rc = sensor_attr_set(dev, SENSOR_CHAN_ACCEL_XYZ,
			     SENSOR_ATTR_SAMPLING_FREQUENCY, &odr);
	if (rc != 0) {
		return rc;
	}
	rc = sensor_attr_set(dev, SENSOR_CHAN_ACCEL_XYZ,
			     SENSOR_ATTR_FULL_SCALE, &fs);
	if (rc != 0) {
		return rc;
	}
	/* FEATURE_MASK val1 != 0 puts ACC into high-power (enabled). */
	return sensor_attr_set(dev, SENSOR_CHAN_ACCEL_XYZ,
			       SENSOR_ATTR_FEATURE_MASK, &on);
}

static int imu_read_mag(const struct device *dev, int32_t *mag)
{
	struct sensor_value xyz[3];
	int rc = sensor_sample_fetch_chan(dev, SENSOR_CHAN_ACCEL_XYZ);

	if (rc != 0) {
		return rc;
	}
	rc = sensor_channel_get(dev, SENSOR_CHAN_ACCEL_XYZ, xyz);
	if (rc != 0) {
		return rc;
	}

	/* The local BMI323 fork stores accel in g, not Zephyr m/s^2.
	 * milli of that is milli-g (~1000 at rest). Thresholds are relative
	 * to a learned rest magnitude so a later m/s^2 fix still works. */
	g_ax = (int32_t)sensor_value_to_milli(&xyz[0]);
	g_ay = (int32_t)sensor_value_to_milli(&xyz[1]);
	g_az = (int32_t)sensor_value_to_milli(&xyz[2]);

	uint64_t mag2 = (uint64_t)((int64_t)g_ax * g_ax) +
			(uint64_t)((int64_t)g_ay * g_ay) +
			(uint64_t)((int64_t)g_az * g_az);

	*mag = (int32_t)isqrt_u64(mag2);
	g_mag = *mag;
	return 0;
}

static void imu_apply_mag(int32_t mag)
{
	int32_t on_th;
	int32_t off_th;
	int32_t dyn;
	bool was;
	bool now;

	if (g_learn_n < IMU_LEARN_N) {
		g_learn_sum += mag;
		g_learn_n++;
		if (g_learn_n == IMU_LEARN_N) {
			g_rest = (int32_t)(g_learn_sum / IMU_LEARN_N);
			LOG_INF("imu: rest mag=%d ax=%d ay=%d az=%d",
				g_rest, g_ax, g_ay, g_az);
		}
		return;
	}

	if (g_rest < IMU_REST_MIN) {
		return;
	}

	dyn = mag - g_rest;
	if (dyn < 0) {
		dyn = -dyn;
	}
	g_dyn = dyn;

	on_th = (g_rest * IMU_ON_PCT) / 100;
	off_th = (g_rest * IMU_OFF_PCT) / 100;
	was = atomic_get(&g_motion) != 0;
	now = was;

	if (dyn >= on_th) {
		g_off_run = 0;
		if (g_on_run < IMU_ON_RUN) {
			g_on_run++;
		}
		if (g_on_run >= IMU_ON_RUN) {
			now = true;
		}
	} else if (dyn <= off_th) {
		g_on_run = 0;
		if (g_off_run < IMU_OFF_RUN) {
			g_off_run++;
		}
		if (g_off_run >= IMU_OFF_RUN) {
			now = false;
		}
		/* Track gravity while still. /256 at 50 Hz ≈ 5 s. */
		g_rest += (mag - g_rest) / 256;
	} else {
		g_on_run = 0;
		g_off_run = 0;
	}

	if (now == was) {
		return;
	}
	atomic_set(&g_motion, now ? 1 : 0);
	LOG_INF("imu: motion %d (dyn=%d rest=%d)", now ? 1 : 0, dyn, g_rest);
}

static void imu_maybe_stats(void)
{
	int64_t now = k_uptime_get();

	if (now - g_stats_ms < IMU_STATS_MS) {
		return;
	}
	g_stats_ms = now;
	LOG_INF("imu: motion=%d mag=%d rest=%d dyn=%d ax=%d ay=%d az=%d",
		atomic_get(&g_motion) ? 1 : 0, g_mag, g_rest, g_dyn,
		g_ax, g_ay, g_az);
}

static void imu_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	g_stats_ms = k_uptime_get();
	for (;;) {
#if IMU_HAVE_DT
		int32_t mag;

		if (imu_read_mag(bmi323, &mag) == 0) {
			imu_apply_mag(mag);
		}
		imu_maybe_stats();
#endif
		k_msleep(IMU_POLL_MS);
	}
}

int hpi_imu_init(void)
{
	atomic_set(&g_motion, 0);

#if IMU_HAVE_DT
	if (!device_is_ready(bmi323)) {
		LOG_INF("imu: BMI323 not ready — motion flag stays 0");
		return 0;
	}

	int rc = imu_configure(bmi323);

	if (rc != 0) {
		LOG_WRN("imu: configure failed rc=%d — motion flag stays 0", rc);
		return 0;
	}
	/* First samples after MODE=HPWR are often -ENODATA. */
	k_msleep(50);
	LOG_INF("imu: BMI323 accel %u Hz, motion flag on HP6_VIT_MOTION",
		IMU_ODR_HZ);
	k_thread_create(&imu_thread, imu_stack, K_THREAD_STACK_SIZEOF(imu_stack),
			imu_thread_fn, NULL, NULL, NULL,
			IMU_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&imu_thread, "hpi_imu");
#else
	LOG_INF("imu: no bmi323 node on this board");
#endif
	return 0;
}

bool hpi_imu_motion(void)
{
	return atomic_get(&g_motion) != 0;
}
