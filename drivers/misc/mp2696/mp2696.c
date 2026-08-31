/*
 * Copyright (c) 2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * MP2696B switching battery charger driver (I2C). Register map per
 * "MP2696B Rev. 1.0 - 4/16/2021", pages 21-26.
 */

#define DT_DRV_COMPAT mps_mp2696

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <mp2696.h>

LOG_MODULE_REGISTER(mp2696, CONFIG_MP2696_LOG_LEVEL);

/* Register addresses */
#define MP2696_REG_00 0x00 /* VINMIN / IINLIM / EN_TIMER / REG_RST */
#define MP2696_REG_01 0x01 /* ICC / IPRE / EN_NTC */
#define MP2696_REG_02 0x02 /* BATT_REG / ITERM / JEITA_DIS / BATT_OVP_DIS / CHG_EN */
#define MP2696_REG_03 0x03 /* IOLIM / RSYS_CMP / NO_LOAD */
#define MP2696_REG_04 0x04 /* VBOOST / BST_EN / Q2_EN / SYS_DSC / USB2_* */
#define MP2696_REG_05 0x05 /* Status (RO) */
#define MP2696_REG_06 0x06 /* Fault (RO) */
#define MP2696_REG_07 0x07 /* NOLOAD_THR / BATT_OVP / NTC_STOP / VIN_OVP / SW_FREQ / BST_IPK */
#define MP2696_REG_08 0x08 /* JEITA control */

/* REG00h bits */
#define MP2696_REG00_REG_RST   BIT(7)
#define MP2696_REG00_EN_TIMER  BIT(6)
#define MP2696_REG00_VINMIN_MASK GENMASK(5, 3)
#define MP2696_REG00_IINLIM_MASK GENMASK(2, 0)

/* REG01h bits */
#define MP2696_REG01_ICC_MASK    GENMASK(7, 3)
#define MP2696_REG01_EN_NTC      BIT(2)
#define MP2696_REG01_IPRE_MASK   GENMASK(1, 0)

/* REG02h bits */
#define MP2696_REG02_BATT_OVP_DIS BIT(7)
#define MP2696_REG02_BATT_REG_MASK GENMASK(6, 4)
#define MP2696_REG02_JEITA_DIS    BIT(3)
#define MP2696_REG02_ITERM_MASK   GENMASK(2, 1)
#define MP2696_REG02_CHG_EN       BIT(0)

/* REG03h bits */
#define MP2696_REG03_IOLIM_MASK    GENMASK(7, 4)
#define MP2696_REG03_RSYS_CMP_MASK GENMASK(3, 1)
#define MP2696_REG03_NO_LOAD       BIT(0)

/* REG04h bits */
#define MP2696_REG04_VBOOST_MASK  GENMASK(7, 5)
#define MP2696_REG04_BST_EN       BIT(4)
#define MP2696_REG04_Q2_EN        BIT(3)
#define MP2696_REG04_SYS_DSC      BIT(2)
#define MP2696_REG04_USB2_EN_PLUG BIT(1)
#define MP2696_REG04_USB2_PLUG_IN BIT(0)

/* REG05h status fields */
#define MP2696_REG05_CHIP_STAT_MASK GENMASK(7, 6)
#define MP2696_REG05_CHG_STAT_MASK  GENMASK(5, 4)
#define MP2696_REG05_VPPM_STAT      BIT(3)
#define MP2696_REG05_IPPM_STAT      BIT(2)
#define MP2696_REG05_USB1_PLUG_IN   BIT(1)

/* REG06h fault fields */
#define MP2696_REG06_BATT_UVLO      BIT(7)
#define MP2696_REG06_SYS_SHORT      BIT(6)
#define MP2696_REG06_BST_LMT        BIT(5)
#define MP2696_REG06_CHG_FAULT_MASK GENMASK(4, 3)
#define MP2696_REG06_NTC_FAULT_MASK GENMASK(2, 0)

/* REG07h bits */
#define MP2696_REG07_BATT_OVP   BIT(5)
#define MP2696_REG07_VIN_OVP    BIT(3)
#define MP2696_REG07_SW_FREQ    BIT(2)

struct mp2696_config {
	struct i2c_dt_spec bus;
	struct gpio_dt_spec int_gpio;

	/* DT-sourced register overrides; -1 means "keep POR value". */
	int8_t iinlim_code;
	int8_t vinmin_code;
	int8_t icc_code;
	int8_t ipre_code;
	int8_t iterm_code;
	int8_t batt_reg_code;
	int8_t vboost_code;
	int8_t iolim_code;
	int8_t timer_hours; /* 10 or 20, or 0 = keep */

	bool charge_disable;
	bool jeita_disable;
	bool ntc_disable;
	bool safety_timer_disable;

	int8_t vin_ovp_sel;    /* 0=6V, 1=11V, -1=keep */
	int8_t sw_freq_sel;    /* 0=700k, 1=1200k, -1=keep */
};

struct mp2696_data {
	struct k_mutex lock;
};

static int mp2696_read(const struct device *dev, uint8_t reg, uint8_t *val)
{
	const struct mp2696_config *cfg = dev->config;

	return i2c_reg_read_byte_dt(&cfg->bus, reg, val);
}

static int mp2696_write(const struct device *dev, uint8_t reg, uint8_t val)
{
	const struct mp2696_config *cfg = dev->config;

	return i2c_reg_write_byte_dt(&cfg->bus, reg, val);
}

static int mp2696_update(const struct device *dev, uint8_t reg, uint8_t mask, uint8_t val)
{
	const struct mp2696_config *cfg = dev->config;

	return i2c_reg_update_byte_dt(&cfg->bus, reg, mask, val);
}

int mp2696_soft_reset(const struct device *dev)
{
	struct mp2696_data *data = dev->data;
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);
	ret = mp2696_update(dev, MP2696_REG_00, MP2696_REG00_REG_RST, MP2696_REG00_REG_RST);
	/* REG_RST self-clears; give the chip a moment before further access. */
	k_msleep(2);
	k_mutex_unlock(&data->lock);

	return ret;
}

int mp2696_set_charge_enable(const struct device *dev, bool enable)
{
	return mp2696_update(dev, MP2696_REG_02, MP2696_REG02_CHG_EN,
			     enable ? MP2696_REG02_CHG_EN : 0);
}

int mp2696_set_boost_enable(const struct device *dev, bool enable)
{
	return mp2696_update(dev, MP2696_REG_04, MP2696_REG04_BST_EN,
			     enable ? MP2696_REG04_BST_EN : 0);
}

int mp2696_read_status(const struct device *dev, struct mp2696_status *out)
{
	uint8_t reg;
	int ret;

	if (out == NULL) {
		return -EINVAL;
	}

	ret = mp2696_read(dev, MP2696_REG_05, &reg);
	if (ret < 0) {
		return ret;
	}

	out->chip_state = FIELD_GET(MP2696_REG05_CHIP_STAT_MASK, reg);
	out->charge_state = FIELD_GET(MP2696_REG05_CHG_STAT_MASK, reg);
	out->input_voltage_loop_active = (reg & MP2696_REG05_VPPM_STAT) != 0;
	out->input_current_loop_active = (reg & MP2696_REG05_IPPM_STAT) != 0;
	out->usb1_plugged_in = (reg & MP2696_REG05_USB1_PLUG_IN) != 0;

	return 0;
}

int mp2696_read_fault(const struct device *dev, struct mp2696_fault *out)
{
	uint8_t reg06;
	uint8_t reg07;
	int ret;

	if (out == NULL) {
		return -EINVAL;
	}

	ret = mp2696_read(dev, MP2696_REG_06, &reg06);
	if (ret < 0) {
		return ret;
	}

	ret = mp2696_read(dev, MP2696_REG_07, &reg07);
	if (ret < 0) {
		return ret;
	}

	out->battery_uvlo = (reg06 & MP2696_REG06_BATT_UVLO) != 0;
	out->sys_short_circuit = (reg06 & MP2696_REG06_SYS_SHORT) != 0;
	out->boost_current_limit = (reg06 & MP2696_REG06_BST_LMT) != 0;
	out->charge_fault = FIELD_GET(MP2696_REG06_CHG_FAULT_MASK, reg06);
	out->ntc_fault = FIELD_GET(MP2696_REG06_NTC_FAULT_MASK, reg06);
	out->battery_ovp = (reg07 & MP2696_REG07_BATT_OVP) != 0;

	return 0;
}

static int mp2696_apply_config(const struct device *dev)
{
	const struct mp2696_config *cfg = dev->config;
	int ret;

	/* REG00h: timer enable + VINMIN + IINLIM */
	{
		uint8_t mask = 0;
		uint8_t val = 0;

		if (cfg->safety_timer_disable) {
			mask |= MP2696_REG00_EN_TIMER;
			/* clear = disable */
		}
		if (cfg->vinmin_code >= 0) {
			mask |= MP2696_REG00_VINMIN_MASK;
			val |= FIELD_PREP(MP2696_REG00_VINMIN_MASK, cfg->vinmin_code);
		}
		if (cfg->iinlim_code >= 0) {
			mask |= MP2696_REG00_IINLIM_MASK;
			val |= FIELD_PREP(MP2696_REG00_IINLIM_MASK, cfg->iinlim_code);
		}
		if (mask) {
			ret = mp2696_update(dev, MP2696_REG_00, mask, val);
			if (ret < 0) {
				return ret;
			}
		}
	}

	/* REG01h: ICC + EN_NTC + IPRE */
	{
		uint8_t mask = 0;
		uint8_t val = 0;

		if (cfg->icc_code >= 0) {
			mask |= MP2696_REG01_ICC_MASK;
			val |= FIELD_PREP(MP2696_REG01_ICC_MASK, cfg->icc_code);
		}
		if (cfg->ntc_disable) {
			mask |= MP2696_REG01_EN_NTC;
		}
		if (cfg->ipre_code >= 0) {
			mask |= MP2696_REG01_IPRE_MASK;
			val |= FIELD_PREP(MP2696_REG01_IPRE_MASK, cfg->ipre_code);
		}
		if (mask) {
			ret = mp2696_update(dev, MP2696_REG_01, mask, val);
			if (ret < 0) {
				return ret;
			}
		}
	}

	/* REG02h: BATT_REG + JEITA_DIS + ITERM + CHG_EN */
	{
		uint8_t mask = 0;
		uint8_t val = 0;

		if (cfg->batt_reg_code >= 0) {
			mask |= MP2696_REG02_BATT_REG_MASK;
			val |= FIELD_PREP(MP2696_REG02_BATT_REG_MASK, cfg->batt_reg_code);
		}
		if (cfg->jeita_disable) {
			mask |= MP2696_REG02_JEITA_DIS;
			val |= MP2696_REG02_JEITA_DIS;
		}
		if (cfg->iterm_code >= 0) {
			mask |= MP2696_REG02_ITERM_MASK;
			val |= FIELD_PREP(MP2696_REG02_ITERM_MASK, cfg->iterm_code);
		}
		mask |= MP2696_REG02_CHG_EN;
		val |= cfg->charge_disable ? 0 : MP2696_REG02_CHG_EN;

		ret = mp2696_update(dev, MP2696_REG_02, mask, val);
		if (ret < 0) {
			return ret;
		}
	}

	/* REG03h: IOLIM */
	if (cfg->iolim_code >= 0) {
		ret = mp2696_update(dev, MP2696_REG_03, MP2696_REG03_IOLIM_MASK,
				    FIELD_PREP(MP2696_REG03_IOLIM_MASK, cfg->iolim_code));
		if (ret < 0) {
			return ret;
		}
	}

	/* REG04h: VBOOST */
	if (cfg->vboost_code >= 0) {
		ret = mp2696_update(dev, MP2696_REG_04, MP2696_REG04_VBOOST_MASK,
				    FIELD_PREP(MP2696_REG04_VBOOST_MASK, cfg->vboost_code));
		if (ret < 0) {
			return ret;
		}
	}

	/* REG07h: VIN_OVP + SW_FREQ */
	{
		uint8_t mask = 0;
		uint8_t val = 0;

		if (cfg->vin_ovp_sel >= 0) {
			mask |= MP2696_REG07_VIN_OVP;
			val |= cfg->vin_ovp_sel ? MP2696_REG07_VIN_OVP : 0;
		}
		if (cfg->sw_freq_sel >= 0) {
			mask |= MP2696_REG07_SW_FREQ;
			val |= cfg->sw_freq_sel ? MP2696_REG07_SW_FREQ : 0;
		}
		if (mask) {
			ret = mp2696_update(dev, MP2696_REG_07, mask, val);
			if (ret < 0) {
				return ret;
			}
		}
	}

	/* REG0Ah: safety timer length.
	 * Datasheet marks TMR as N/A for normal R/W access; we only touch
	 * it when the user explicitly asks, via write-through.
	 */
	if (cfg->timer_hours == 10 || cfg->timer_hours == 20) {
		uint8_t cur = 0;

		ret = mp2696_read(dev, 0x0A, &cur);
		if (ret < 0) {
			LOG_DBG("REG0Ah read failed (%d); skipping timer override", ret);
		} else {
			cur &= ~BIT(7);
			if (cfg->timer_hours == 10) {
				cur |= BIT(7);
			}
			(void)mp2696_write(dev, 0x0A, cur);
		}
	}

	return 0;
}

static int mp2696_init(const struct device *dev)
{
	const struct mp2696_config *cfg = dev->config;
	struct mp2696_data *data = dev->data;
	uint8_t probe;
	int ret;

	k_mutex_init(&data->lock);

	if (!i2c_is_ready_dt(&cfg->bus)) {
		LOG_ERR("I2C bus %s not ready", cfg->bus.bus->name);
		return -ENODEV;
	}

	/*
	 * The MP2696B does not expose a chip-ID register, so we probe by
	 * reading REG00h. A successful ACK at 0x6B is the strongest
	 * signal we can get.
	 */
	ret = mp2696_read(dev, MP2696_REG_00, &probe);
	if (ret < 0) {
		LOG_ERR("Probe read (REG00h) failed: %d", ret);
		return ret;
	}
	LOG_DBG("Probe OK; REG00h = 0x%02x", probe);

	ret = mp2696_apply_config(dev);
	if (ret < 0) {
		LOG_ERR("Failed to apply DT config: %d", ret);
		return ret;
	}

	if (cfg->int_gpio.port != NULL) {
		if (!gpio_is_ready_dt(&cfg->int_gpio)) {
			LOG_WRN("INT GPIO not ready; skipping");
		} else {
			(void)gpio_pin_configure_dt(&cfg->int_gpio, GPIO_INPUT);
		}
	}

	return 0;
}

/*
 * Code-table helpers: map a microvolt / microamp value coming from DT into
 * the nearest register code. Floor-round (picks the largest step <= value).
 */
static int8_t mp2696_code_from_step(uint32_t value, uint32_t offset, uint32_t step,
				    uint8_t max_code)
{
	if (value < offset) {
		return 0;
	}
	uint32_t code = (value - offset) / step;

	if (code > max_code) {
		code = max_code;
	}
	return (int8_t)code;
}

/* IINLIM: non-uniform. 000=500, 001=1000, 010=1500, 011=1800, 100=2100,
 * 101=2400, 110=3000, 111=3500 (mA) */
static int8_t mp2696_iinlim_code(int32_t uamp)
{
	static const int32_t tbl[] = {500000, 1000000, 1500000, 1800000,
				      2100000, 2400000, 3000000, 3500000};
	int best = 0;

	for (int i = 0; i < (int)ARRAY_SIZE(tbl); i++) {
		if (uamp >= tbl[i]) {
			best = i;
		}
	}
	return (int8_t)best;
}

/* IPRE: 00=reserved (maps to 150 per datasheet -- treat 0/150 same),
 *       01=150, 10=250, 11=350 (mA) */
static int8_t mp2696_ipre_code(int32_t uamp)
{
	if (uamp >= 350000) {
		return 3;
	}
	if (uamp >= 250000) {
		return 2;
	}
	return 1; /* 150 mA */
}

/* ITERM: 00=100, 01=200, 10=300, 11=400 (mA). Offset 100, step 100. */
static int8_t mp2696_iterm_code(int32_t uamp)
{
	return mp2696_code_from_step(uamp, 100000, 100000, 3);
}

/* BATT_REG: 000=3.6, 001=4.1, 010=4.2, 011=4.3, 100=4.35, 101=4.4, 110=4.45 */
static int8_t mp2696_batt_reg_code(int32_t uvolt)
{
	static const int32_t tbl[] = {3600000, 4100000, 4200000, 4300000,
				      4350000, 4400000, 4450000};
	int best = 0;

	for (int i = 0; i < (int)ARRAY_SIZE(tbl); i++) {
		if (uvolt >= tbl[i]) {
			best = i;
		}
	}
	return (int8_t)best;
}

/* VINMIN: offset 4.45V, 50 mV step, 3 bits (0..7 -> 4.45..4.80). */
static int8_t mp2696_vinmin_code(int32_t uvolt)
{
	return mp2696_code_from_step(uvolt, 4450000, 50000, 7);
}

/* VBOOST: 3 bits; centered at 5.15V, step 25 mV.
 * 000=-150, 001=-100, 010=-50, 011=-25, 100=0, 101=+25, 110=+50, 111=+75
 * -> absolute 5.00..5.225V? Datasheet says 5.05..5.225V range, default 5.15V
 * with weights {-100, -50, +25}. We build a lookup table for robustness.
 */
static int8_t mp2696_vboost_code(int32_t uvolt)
{
	static const int32_t tbl[] = {5000000, 5050000, 5100000, 5125000,
				      5150000, 5175000, 5200000, 5225000};
	int best = 4; /* default 5.15V (code 100) */

	for (int i = 0; i < (int)ARRAY_SIZE(tbl); i++) {
		if (uvolt >= tbl[i]) {
			best = i;
		}
	}
	return (int8_t)best;
}

/* ICC: offset 500 mA, step 100 mA, 5 bits. Range 500..3600 mA. */
static int8_t mp2696_icc_code(int32_t uamp)
{
	return mp2696_code_from_step(uamp, 500000, 100000, 31);
}

/* IOLIM: offset 1 A, step 200 mA, 4 bits. Range 1..4 A. */
static int8_t mp2696_iolim_code(int32_t uamp)
{
	return mp2696_code_from_step(uamp, 1000000, 200000, 15);
}

#define MP2696_DT_OPT_INT(n, prop, xform, default_val)                                             \
	COND_CODE_1(DT_INST_NODE_HAS_PROP(n, prop),                                                \
		    ((int8_t)xform(DT_INST_PROP(n, prop))), (default_val))

#define MP2696_DT_OPT_SEL(n, prop, map_high, map_low, default_val)                                 \
	COND_CODE_1(DT_INST_NODE_HAS_PROP(n, prop),                                                \
		    ((DT_INST_PROP(n, prop) == (map_high)) ? 1 : 0), (default_val))

#define MP2696_INIT(n)                                                                             \
	static const struct mp2696_config mp2696_cfg_##n = {                                       \
		.bus = I2C_DT_SPEC_INST_GET(n),                                                    \
		.int_gpio = GPIO_DT_SPEC_INST_GET_OR(n, int_gpios, {0}),                           \
		.iinlim_code = MP2696_DT_OPT_INT(n, input_current_limit_microamp,                  \
						 mp2696_iinlim_code, -1),                          \
		.vinmin_code = MP2696_DT_OPT_INT(n, input_voltage_regulation_microvolt,            \
						 mp2696_vinmin_code, -1),                          \
		.icc_code = MP2696_DT_OPT_INT(n, charge_current_microamp, mp2696_icc_code, -1),    \
		.ipre_code = MP2696_DT_OPT_INT(n, precharge_current_microamp, mp2696_ipre_code,    \
					       -1),                                                \
		.iterm_code = MP2696_DT_OPT_INT(n, termination_current_microamp,                   \
						mp2696_iterm_code, -1),                            \
		.batt_reg_code = MP2696_DT_OPT_INT(n, battery_regulation_microvolt,                \
						   mp2696_batt_reg_code, -1),                      \
		.vboost_code = MP2696_DT_OPT_INT(n, boost_output_microvolt, mp2696_vboost_code,    \
						 -1),                                              \
		.iolim_code = MP2696_DT_OPT_INT(n, boost_current_limit_microamp,                   \
						mp2696_iolim_code, -1),                            \
		.timer_hours = DT_INST_PROP_OR(n, charge_safety_timer_hours, 0),                   \
		.charge_disable = DT_INST_PROP(n, charge_disable),                                 \
		.jeita_disable = DT_INST_PROP(n, jeita_disable),                                   \
		.ntc_disable = DT_INST_PROP(n, ntc_disable),                                       \
		.safety_timer_disable = DT_INST_PROP(n, safety_timer_disable),                     \
		.vin_ovp_sel = MP2696_DT_OPT_SEL(n, vin_ovp_microvolt, 11000000, 6000000, -1),     \
		.sw_freq_sel = MP2696_DT_OPT_SEL(n, switching_frequency_hz, 1200000, 700000, -1),  \
	};                                                                                         \
	static struct mp2696_data mp2696_data_##n;                                                 \
	DEVICE_DT_INST_DEFINE(n, mp2696_init, NULL, &mp2696_data_##n, &mp2696_cfg_##n,             \
			      POST_KERNEL, CONFIG_MP2696_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(MP2696_INIT)
