/*
 * Copyright (c) 2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Driver API for MP2696B switching battery charger.
 */

#ifndef ZEPHYR_INCLUDE_MP2696_H_
#define ZEPHYR_INCLUDE_MP2696_H_

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/* REG05h CHIP_STAT[1:0] */
enum mp2696_chip_state {
	MP2696_CHIP_IDLE = 0,
	MP2696_CHIP_CHARGE = 1,
	MP2696_CHIP_BOOST = 2,
	MP2696_CHIP_POWER_PATH_AND_CHARGE = 3,
};

/* REG05h CHG_STAT[1:0] */
enum mp2696_charge_state {
	MP2696_CHG_NOT_CHARGING = 0,
	MP2696_CHG_PRECHARGE = 1,
	MP2696_CHG_CC_OR_CV = 2,
	MP2696_CHG_DONE = 3,
};

/* REG06h CHG_FAULT[1:0] */
enum mp2696_charge_fault {
	MP2696_CHG_FAULT_NONE = 0,
	MP2696_CHG_FAULT_USB1_UV = 1,
	MP2696_CHG_FAULT_USB1_OV = 2,
	MP2696_CHG_FAULT_TIMER_EXPIRED = 3,
};

/* REG06h NTC_FAULT[2:0] */
enum mp2696_ntc_fault {
	MP2696_NTC_NORMAL = 0,
	MP2696_NTC_WARM = 1,
	MP2696_NTC_COOL = 2,
	MP2696_NTC_COLD = 3,
	MP2696_NTC_HOT = 4,
};

struct mp2696_status {
	enum mp2696_chip_state chip_state;
	enum mp2696_charge_state charge_state;
	bool input_voltage_loop_active; /* VINPPM */
	bool input_current_loop_active; /* IINPPM */
	bool usb1_plugged_in;
};

struct mp2696_fault {
	bool battery_uvlo;
	bool sys_short_circuit;
	bool boost_current_limit;
	bool battery_ovp;
	enum mp2696_charge_fault charge_fault;
	enum mp2696_ntc_fault ntc_fault;
};

int mp2696_read_status(const struct device *dev, struct mp2696_status *out);
int mp2696_read_fault(const struct device *dev, struct mp2696_fault *out);

int mp2696_set_charge_enable(const struct device *dev, bool enable);
int mp2696_set_boost_enable(const struct device *dev, bool enable);

/*
 * Trigger REG_RST (REG00h[7]) to restore all R/W registers to their POR
 * defaults. The driver will NOT re-apply the devicetree overrides after
 * this call — invoke it only when you want a clean slate.
 */
int mp2696_soft_reset(const struct device *dev);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_MP2696_H_ */
