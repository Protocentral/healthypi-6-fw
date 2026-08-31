/*
 * Texas Instruments ADS1299 - 8-Channel 24-Bit EEG Analog Front-End
 * Copyright (c) 2025 ProtoCentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Zephyr sensor driver for ADS1299 EEG AFE.
 * Used on HealthyLink EEG-8CH expansion module.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>

#include "ads1299.h"

LOG_MODULE_REGISTER(ads1299, CONFIG_SENSOR_LOG_LEVEL);

#define DT_DRV_COMPAT ti_ads1299

/*
 * Dedicated workqueue for ADS1299 DRDY handling: avoids system-workqueue
 * contention and allows priority tuning for real-time EEG acquisition.
 */
#define ADS1299_WORKQ_STACK_SIZE 2048
#define ADS1299_WORKQ_PRIORITY 5  /* Higher priority than default (7) */

K_THREAD_STACK_DEFINE(ads1299_workq_stack, ADS1299_WORKQ_STACK_SIZE);
static struct k_work_q ads1299_workq;
static bool ads1299_workq_started = false;

/*
 * SPI Communication Helpers
 */
static int ads1299_send_cmd(const struct device *dev, uint8_t cmd)
{
	const struct ads1299_config *cfg = dev->config;
	uint8_t tx_buf[1] = {cmd};
	struct spi_buf tx = {.buf = tx_buf, .len = 1};
	struct spi_buf_set tx_set = {.buffers = &tx, .count = 1};

	return spi_write_dt(&cfg->spi, &tx_set);
}

/* Drive the START pin if the board has one. Optional per the binding: without
 * it the START/STOP opcodes this driver already sends are what start and stop
 * conversion, so every call site can stay unconditional. */
static void ads1299_start_pin(const struct ads1299_config *cfg, int value)
{
	if (cfg->start_gpio.port != NULL) {
		gpio_pin_set(cfg->start_gpio.port, cfg->start_gpio.pin, value);
	}
}

static int ads1299_read_reg(const struct device *dev, uint8_t reg, uint8_t *val)
{
	const struct ads1299_config *cfg = dev->config;
	int ret;

	/*
	 * ADS1299 RREG command format (datasheet pg. 35):
	 *   Byte 1: 0x20 | reg  (RREG opcode + register address)
	 *   Byte 2: 0x00        (number of registers - 1, so 0 = 1 register)
	 *   Byte 3: 0x00        (clock out dummy byte while device sends data)
	 *
	 * The response data appears on MISO during byte 3.
	 */
	uint8_t tx_buf[3] = {ADS1299_CMD_RREG | reg, 0x00, 0x00};
	uint8_t rx_buf[3] = {0};

	struct spi_buf tx = {.buf = tx_buf, .len = 3};
	struct spi_buf_set tx_set = {.buffers = &tx, .count = 1};
	struct spi_buf rx = {.buf = rx_buf, .len = 3};
	struct spi_buf_set rx_set = {.buffers = &rx, .count = 1};

	ret = spi_transceive_dt(&cfg->spi, &tx_set, &rx_set);
	if (ret == 0) {
		*val = rx_buf[2];
		LOG_DBG("RREG 0x%02X = 0x%02X", reg, *val);
	}
	return ret;
}

static int ads1299_write_reg(const struct device *dev, uint8_t reg, uint8_t val)
{
	const struct ads1299_config *cfg = dev->config;
	uint8_t tx_buf[3] = {ADS1299_CMD_WREG | reg, 0x00, val};
	struct spi_buf tx = {.buf = tx_buf, .len = 3};
	struct spi_buf_set tx_set = {.buffers = &tx, .count = 1};

	return spi_write_dt(&cfg->spi, &tx_set);
}

/*
 * Data Acquisition
 */
static int ads1299_read_data(const struct device *dev)
{
	const struct ads1299_config *cfg = dev->config;
	struct ads1299_data *data = dev->data;

	/* ADS1299 data format: 3 status bytes + 8 channels × 3 bytes = 27 bytes */
	uint8_t rx_buf[27] = {0};

	struct spi_buf rx = {.buf = rx_buf, .len = 27};
	struct spi_buf_set rx_set = {.buffers = &rx, .count = 1};

	int ret = spi_read_dt(&cfg->spi, &rx_set);
	if (ret < 0) {
		return ret;
	}

	/* Extract 24-bit status word */
	data->status = ((uint32_t)rx_buf[0] << 16) |
		       ((uint32_t)rx_buf[1] << 8) |
		       ((uint32_t)rx_buf[2]);

	/* Extract 8 channels (24-bit signed, big-endian) */
	for (int ch = 0; ch < 8; ch++) {
		int idx = 3 + (ch * 3);
		uint32_t raw = ((uint32_t)rx_buf[idx] << 16) |
			       ((uint32_t)rx_buf[idx + 1] << 8) |
			       ((uint32_t)rx_buf[idx + 2]);

		/* Sign-extend 24-bit to 32-bit */
		if (raw & 0x800000) {
			data->channels[ch] = (int32_t)(raw | 0xFF000000);
		} else {
			data->channels[ch] = (int32_t)raw;
		}
	}

	return 0;
}

/*
 * Interrupt Handling
 */
static void ads1299_drdy_callback(const struct device *gpio_dev,
				  struct gpio_callback *cb, uint32_t pins)
{
	struct ads1299_data *data = CONTAINER_OF(cb, struct ads1299_data, drdy_cb);

	/* Submit to dedicated ADS1299 workqueue (not system workqueue) */
	k_work_submit_to_queue(&ads1299_workq, &data->work);
}

static void ads1299_work_handler(struct k_work *work)
{
	struct ads1299_data *data = CONTAINER_OF(work, struct ads1299_data, work);

	/* Verify device pointer is valid before use */
	if (data->dev == NULL) {
		LOG_ERR("ADS1299 work handler: data->dev is NULL!");
		return;
	}

	const struct ads1299_config *cfg = data->dev->config;
	if (cfg == NULL) {
		LOG_ERR("ADS1299 work handler: config is NULL!");
		return;
	}

	/* Disable interrupt during processing */
	gpio_pin_interrupt_configure_dt(&cfg->drdy_gpio, GPIO_INT_DISABLE);

	/* Call user handler */
	if (data->handler != NULL) {
		data->handler(data->dev, data->trigger);
	}

	/* Re-enable interrupt */
	gpio_pin_interrupt_configure_dt(&cfg->drdy_gpio, GPIO_INT_EDGE_TO_ACTIVE);
}

/*
 * Sensor API: sample_fetch
 */
static int ads1299_sample_fetch(const struct device *dev, enum sensor_channel chan)
{
	struct ads1299_data *data = dev->data;

	if (!data->running) {
		return -ENODATA;
	}

	return ads1299_read_data(dev);
}

/*
 * Sensor API: channel_get
 */
static int ads1299_channel_get(const struct device *dev,
			       enum sensor_channel chan,
			       struct sensor_value *val)
{
	struct ads1299_data *data = dev->data;
	int ch_idx;

	switch ((int)chan) {
	case SENSOR_CHAN_ADS1299_CH1:
		ch_idx = 0;
		break;
	case SENSOR_CHAN_ADS1299_CH2:
		ch_idx = 1;
		break;
	case SENSOR_CHAN_ADS1299_CH3:
		ch_idx = 2;
		break;
	case SENSOR_CHAN_ADS1299_CH4:
		ch_idx = 3;
		break;
	case SENSOR_CHAN_ADS1299_CH5:
		ch_idx = 4;
		break;
	case SENSOR_CHAN_ADS1299_CH6:
		ch_idx = 5;
		break;
	case SENSOR_CHAN_ADS1299_CH7:
		ch_idx = 6;
		break;
	case SENSOR_CHAN_ADS1299_CH8:
		ch_idx = 7;
		break;
	default:
		return -ENOTSUP;
	}

	val->val1 = data->channels[ch_idx];
	val->val2 = 0;
	return 0;
}

/*
 * Sensor API: attr_set
 */
static int ads1299_attr_set(const struct device *dev,
			    enum sensor_channel chan,
			    enum sensor_attribute attr,
			    const struct sensor_value *val)
{
	const struct ads1299_config *cfg = dev->config;
	struct ads1299_data *data = dev->data;
	int ret = 0;

	switch ((int)attr) {
	case ADS1299_ATTR_START:
		if (val->val1) {
			/* Start conversion - use physical pin values */
			ads1299_start_pin(cfg, 1);
			k_usleep(100);
			ret = ads1299_send_cmd(dev, ADS1299_CMD_START);
			if (ret == 0) {
				k_usleep(100);
				ret = ads1299_send_cmd(dev, ADS1299_CMD_RDATAC);
				data->running = true;
				data->continuous_mode = true;
			}
		} else {
			/* Stop conversion */
			ret = ads1299_send_cmd(dev, ADS1299_CMD_SDATAC);
			if (ret == 0) {
				ret = ads1299_send_cmd(dev, ADS1299_CMD_STOP);
				ads1299_start_pin(cfg, 0);
				data->running = false;
				data->continuous_mode = false;
			}
		}
		return ret;

	case ADS1299_ATTR_GAIN: {
		/* Set gain for specified channel (or all if SENSOR_CHAN_ALL) */
		uint8_t gain_bits = ads1299_gain_to_bits(val->val1);
		int start_ch = 0, end_ch = 8;

		if ((int)chan >= SENSOR_CHAN_ADS1299_CH1 &&
		    (int)chan <= SENSOR_CHAN_ADS1299_CH8) {
			start_ch = (int)chan - SENSOR_CHAN_ADS1299_CH1;
			end_ch = start_ch + 1;
		}

		/* Must stop continuous mode to write registers */
		bool was_running = data->continuous_mode;
		if (was_running) {
			ads1299_send_cmd(dev, ADS1299_CMD_SDATAC);
		}

		for (int ch = start_ch; ch < end_ch; ch++) {
			uint8_t reg_val;
			ret = ads1299_read_reg(dev, ADS1299_REG_CH1SET + ch, &reg_val);
			if (ret < 0) {
				break;
			}
			reg_val = (reg_val & 0x8F) | gain_bits;  /* Preserve other bits */
			ret = ads1299_write_reg(dev, ADS1299_REG_CH1SET + ch, reg_val);
			if (ret < 0) {
				break;
			}
		}

		if (was_running) {
			ads1299_send_cmd(dev, ADS1299_CMD_RDATAC);
		}
		return ret;
	}

	case ADS1299_ATTR_SAMPLE_RATE: {
		uint8_t dr_bits = ads1299_sps_to_bits(val->val1);

		bool was_running = data->continuous_mode;
		if (was_running) {
			ads1299_send_cmd(dev, ADS1299_CMD_SDATAC);
		}

		uint8_t config1;
		ret = ads1299_read_reg(dev, ADS1299_REG_CONFIG1, &config1);
		if (ret == 0) {
			config1 = (config1 & 0xF8) | dr_bits;
			ret = ads1299_write_reg(dev, ADS1299_REG_CONFIG1, config1);
		}

		if (was_running) {
			ads1299_send_cmd(dev, ADS1299_CMD_RDATAC);
		}
		return ret;
	}

	case ADS1299_ATTR_SRB1: {
		bool was_running = data->continuous_mode;
		if (was_running) {
			ads1299_send_cmd(dev, ADS1299_CMD_SDATAC);
		}

		uint8_t misc1 = val->val1 ? ADS1299_MISC1_SRB1 : 0x00;
		ret = ads1299_write_reg(dev, ADS1299_REG_MISC1, misc1);

		if (was_running) {
			ads1299_send_cmd(dev, ADS1299_CMD_RDATAC);
		}
		return ret;
	}

	default:
		return -ENOTSUP;
	}
}

/*
 * Sensor API: trigger_set
 */
static int ads1299_trigger_set(const struct device *dev,
			       const struct sensor_trigger *trig,
			       sensor_trigger_handler_t handler)
{
	const struct ads1299_config *cfg = dev->config;
	struct ads1299_data *data = dev->data;

	if (trig->type != SENSOR_TRIG_DATA_READY) {
		return -ENOTSUP;
	}

	data->trigger = trig;
	data->handler = handler;

	if (handler != NULL) {
		/* Enable DRDY interrupt */
		return gpio_pin_interrupt_configure_dt(&cfg->drdy_gpio,
						       GPIO_INT_EDGE_TO_ACTIVE);
	} else {
		/* Disable DRDY interrupt */
		return gpio_pin_interrupt_configure_dt(&cfg->drdy_gpio,
						       GPIO_INT_DISABLE);
	}
}

/*
 * Device Initialization (Deferred)
 *
 * NOTE: This driver uses DEFERRED initialization. The ads1299_init() function
 * only sets up data structures. The actual hardware initialization happens
 * when ads1299_hw_init() is called from the application (e.g., hw_module.c).
 * This prevents boot hangs when the HealthyLink module is not connected.
 */
static int ads1299_init(const struct device *dev)
{
	struct ads1299_data *data = dev->data;

	/* Just initialize state - hardware init is deferred */
	data->dev = dev;
	data->running = false;
	data->continuous_mode = false;
	data->hw_present = false;

	LOG_DBG("ADS1299: Device registered (hardware init deferred)");
	return 0;
}

/*
 * Hardware initialization, called explicitly from the application: GPIOs,
 * power-up sequence, ID check, EEG acquisition config.
 *
 * Returns 0 on success, -ENODEV if SPI/GPIO not ready, -ENOENT if no ADS1299
 * responds, other negative errno on failure.
 */
int ads1299_hw_init(const struct device *dev)
{
	const struct ads1299_config *cfg = dev->config;
	struct ads1299_data *data = dev->data;
	int ret;
	uint8_t id;

	if (data->hw_present) {
		LOG_DBG("ADS1299: Already initialized");
		return 0;
	}

	LOG_INF("ADS1299: Starting hardware initialization...");

	/*
	 * CRITICAL: With zephyr,deferred-init, the driver's init function
	 * (ads1299_init) is NOT called at boot. We must initialize the data
	 * structure here that would normally be set in ads1299_init().
	 */
	data->dev = dev;
	data->running = false;
	data->continuous_mode = false;

	/* Start dedicated workqueue if not already running */
	if (!ads1299_workq_started) {
		k_work_queue_init(&ads1299_workq);
		k_work_queue_start(&ads1299_workq, ads1299_workq_stack,
				   K_THREAD_STACK_SIZEOF(ads1299_workq_stack),
				   ADS1299_WORKQ_PRIORITY, NULL);
		k_thread_name_set(&ads1299_workq.thread, "ads1299_workq");
		ads1299_workq_started = true;
		LOG_DBG("ADS1299: Dedicated workqueue started");
	}

	/* Verify SPI bus is ready */
	if (!spi_is_ready_dt(&cfg->spi)) {
		LOG_ERR("ADS1299: SPI bus not ready");
		return -ENODEV;
	}

	/* Configure GPIOs */
	if (!gpio_is_ready_dt(&cfg->drdy_gpio)) {
		LOG_ERR("ADS1299: DRDY GPIO not ready");
		return -ENODEV;
	}
	ret = gpio_pin_configure_dt(&cfg->drdy_gpio, GPIO_INPUT);
	if (ret < 0) {
		LOG_ERR("ADS1299: Failed to configure DRDY GPIO: %d", ret);
		return ret;
	}

	if (!gpio_is_ready_dt(&cfg->reset_gpio)) {
		LOG_ERR("ADS1299: RESET GPIO not ready");
		return -ENODEV;
	}
	ret = gpio_pin_configure_dt(&cfg->reset_gpio, GPIO_OUTPUT);
	if (ret < 0) {
		LOG_ERR("ADS1299: Failed to configure RESET GPIO: %d", ret);
		return ret;
	}

	if (!gpio_is_ready_dt(&cfg->pwdn_gpio)) {
		LOG_ERR("ADS1299: PWDN GPIO not ready");
		return -ENODEV;
	}
	ret = gpio_pin_configure_dt(&cfg->pwdn_gpio, GPIO_OUTPUT);
	if (ret < 0) {
		LOG_ERR("ADS1299: Failed to configure PWDN GPIO: %d", ret);
		return ret;
	}

	/* START is optional (see the binding): a board that does not route it
	 * relies on the START/STOP SPI opcodes, which this driver already sends
	 * either way. Absent = start_gpio.port is NULL. */
	if (cfg->start_gpio.port != NULL) {
		if (!gpio_is_ready_dt(&cfg->start_gpio)) {
			LOG_ERR("ADS1299: START GPIO not ready");
			return -ENODEV;
		}
		ret = gpio_pin_configure_dt(&cfg->start_gpio, GPIO_OUTPUT);
		if (ret < 0) {
			LOG_ERR("ADS1299: Failed to configure START GPIO: %d", ret);
			return ret;
		}
	} else {
		LOG_DBG("ADS1299: no START GPIO; using START/STOP commands");
	}

	/* Setup DRDY interrupt callback */
	gpio_init_callback(&data->drdy_cb, ads1299_drdy_callback,
			   BIT(cfg->drdy_gpio.pin));
	ret = gpio_add_callback(cfg->drdy_gpio.port, &data->drdy_cb);
	if (ret < 0) {
		LOG_ERR("ADS1299: Failed to add DRDY callback: %d", ret);
		return ret;
	}

	k_work_init(&data->work, ads1299_work_handler);

	/* Power-up sequence.
	 *
	 * IMPORTANT: use gpio_pin_set() with PHYSICAL values (0/1), NOT
	 * gpio_pin_set_dt() -- the _dt variant applies the ACTIVE_LOW flag and
	 * inverts the values. Chip logic (active-high):
	 *   PWDN: HIGH = operating, LOW = powered down
	 *   RESET: HIGH = normal, LOW = reset
	 *   START: HIGH = start conversion, LOW = stop
	 *
	 * Datasheet timing: tPOR = 32 ms; tRST >= 2 tCLK (~1 us) reset pulse;
	 * wait 18 tCLK (~9 us) after RESET high; wait 4 tCLK (~2 us) after
	 * SDATAC.
	 */
	LOG_DBG("ADS1299: Starting power-up sequence");

	/* Set START pin HIGH (matching reference driver); no-op without the pin */
	ads1299_start_pin(cfg, 1);

	/* Set PWDN pin HIGH (device powered on) */
	gpio_pin_set(cfg->pwdn_gpio.port, cfg->pwdn_gpio.pin, 1);
	k_msleep(10);

	/* Set RESET pin HIGH initially */
	gpio_pin_set(cfg->reset_gpio.port, cfg->reset_gpio.pin, 1);
	k_msleep(500);  /* Reference uses 500ms here */

	/* Hardware reset pulse - toggle RESET LOW then HIGH */
	gpio_pin_set(cfg->reset_gpio.port, cfg->reset_gpio.pin, 0);  /* Physical LOW = reset */
	k_usleep(100);  /* Reference uses 100µs */
	gpio_pin_set(cfg->reset_gpio.port, cfg->reset_gpio.pin, 1);  /* Physical HIGH = normal */
	k_msleep(200);  /* Reference uses 200ms settling time */

	/* Send SDATAC to exit continuous read mode */
	ret = ads1299_send_cmd(dev, ADS1299_CMD_SDATAC);
	if (ret < 0) {
		LOG_ERR("ADS1299: SDATAC command failed (err=%d)", ret);
		return -ENOENT;
	}
	k_msleep(50);  /* Reference uses 50ms here */

	/* CONFIG3 = 0xE0 before reading ID: enables PD_REFBUF (internal
	 * reference) and sets the required constant bits. */
	ret = ads1299_write_reg(dev, ADS1299_REG_CONFIG3, 0xE0);
	if (ret < 0) {
		LOG_WRN("ADS1299: CONFIG3 write failed (err=%d)", ret);
	}
	k_msleep(100);  /* Wait for reference to settle */

	/* Read device ID */
	ret = ads1299_read_reg(dev, ADS1299_REG_ID, &id);
	if (ret < 0) {
		LOG_ERR("ADS1299: Failed to read device ID (err=%d)", ret);
		return -ENOENT;
	}

	/* Validate device ID */
	if (id == 0x00) {
		LOG_ERR("ADS1299: Device ID is 0x00 - no response (check MISO, power, CS)");
		return -ENOENT;
	}

	/* Check for valid ADS1299 family ID
	 * - 0x3E: ADS1299 (8-channel, REV 0)
	 * - 0x3D: ADS1299 (variant/different revision)
	 * - 0x3C: ADS1299-4 (4-channel)
	 * - 0xDE, etc.: Other ADS129x family devices
	 */
	if (id == 0x3E || id == 0x3D || id == 0x3C ||
	    (id & 0xE0) == 0xC0 || (id & 0xE0) == 0x60) {
		LOG_INF("ADS1299 detected, ID: 0x%02X", id);
	} else {
		LOG_WRN("ADS1299: Unexpected ID 0x%02X - continuing anyway", id);
	}

	/* Configure for EEG acquisition */

	/* CONFIG1: HR mode, configured sample rate */
	uint8_t config1 = ADS1299_CONFIG1_HR | ads1299_sps_to_bits(cfg->sample_rate);
	ret = ads1299_write_reg(dev, ADS1299_REG_CONFIG1, config1);
	if (ret < 0) {
		LOG_ERR("ADS1299: Failed to write CONFIG1");
		return ret;
	}

	/* CONFIG2: Default (no test signals) */
	ret = ads1299_write_reg(dev, ADS1299_REG_CONFIG2, 0xC0);
	if (ret < 0) {
		LOG_ERR("ADS1299: Failed to write CONFIG2");
		return ret;
	}

	/* CONFIG4: Disable lead-off comparators to prevent current injection artifacts
	 * Bit 1 (PD_LOFF_COMP) = 1 powers down lead-off comparators
	 */
	ret = ads1299_write_reg(dev, ADS1299_REG_CONFIG4, ADS1299_CONFIG4_PD_LOFF_COMP);
	if (ret < 0) {
		LOG_WRN("ADS1299: Failed to write CONFIG4");
	}

	/* Disable lead-off detection on all channels */
	ret = ads1299_write_reg(dev, ADS1299_REG_LOFF_SENSP, 0x00);
	if (ret < 0) {
		LOG_WRN("ADS1299: Failed to write LOFF_SENSP");
	}
	ret = ads1299_write_reg(dev, ADS1299_REG_LOFF_SENSN, 0x00);
	if (ret < 0) {
		LOG_WRN("ADS1299: Failed to write LOFF_SENSN");
	}

	/* CONFIG3: Internal reference, BIAS enabled */
	uint8_t config3 = ADS1299_CONFIG3_CONST | ADS1299_CONFIG3_PD_REFBUF;
	if (cfg->bias_enabled) {
		config3 |= ADS1299_CONFIG3_BIASREF_INT | ADS1299_CONFIG3_PD_BIAS;
	}
	ret = ads1299_write_reg(dev, ADS1299_REG_CONFIG3, config3);
	if (ret < 0) {
		LOG_ERR("ADS1299: Failed to write CONFIG3");
		return ret;
	}
	k_msleep(100);  /* Wait for reference to settle */

	/* BIAS (DRL) configuration: BIAS_SENSP/SENSN = 0x00 -- no channel
	 * derivation, BIAS outputs buffered internal BIASREF (mid-supply).
	 * Do NOT derive BIAS from CH1 in single-channel operation: the
	 * feedback amplifies noise. */
	if (cfg->bias_enabled) {
		/* No channel derivation - use internal BIASREF only */
		ret = ads1299_write_reg(dev, ADS1299_REG_BIAS_SENSP, 0x00);
		if (ret < 0) {
			LOG_WRN("ADS1299: Failed to write BIAS_SENSP");
		}
		ret = ads1299_write_reg(dev, ADS1299_REG_BIAS_SENSN, 0x00);
		if (ret < 0) {
			LOG_WRN("ADS1299: Failed to write BIAS_SENSN");
		}
		LOG_INF("ADS1299: BIAS buffer enabled (no derivation)");
	}

	/* Configure channels 1 and 2 with specified gain, normal input
	 * 2-channel referential montage: CH1 and CH2 vs SRB1 (common reference)
	 * Electrodes: IN1P (CH1+), IN2P (CH2+), SRB1 (REF), BIASOUT (DRL)
	 */
	uint8_t ch_set = ads1299_gain_to_bits(cfg->gain) | ADS1299_MUX_NORMAL;

	/* Channel 1: Normal input mode */
	ret = ads1299_write_reg(dev, ADS1299_REG_CH1SET, ch_set);
	if (ret < 0) {
		LOG_ERR("ADS1299: Failed to configure channel 1");
		return ret;
	}

	/* Channel 2: Normal input mode */
	ret = ads1299_write_reg(dev, ADS1299_REG_CH2SET, ch_set);
	if (ret < 0) {
		LOG_ERR("ADS1299: Failed to configure channel 2");
		return ret;
	}

	/* Power down unused channels 3-8 and short their inputs
	 * This prevents floating inputs from injecting noise.
	 * PD bit (7) = 1 powers down channel, MUX = 001 shorts inputs
	 */
	uint8_t ch_pd = ADS1299_CHNSET_PD | ADS1299_MUX_SHORTED;
	for (int ch = 2; ch < 8; ch++) {  /* CH3-CH8 (indices 2-7) */
		ret = ads1299_write_reg(dev, ADS1299_REG_CH1SET + ch, ch_pd);
		if (ret < 0) {
			LOG_WRN("ADS1299: Failed to power down channel %d", ch + 1);
		}
	}
	LOG_INF("ADS1299: CH1+CH2 active (gain=%d), CH3-8 powered down", cfg->gain);

	/* SRB1 configuration - explicitly set or clear */
	if (cfg->srb1_enabled) {
		ret = ads1299_write_reg(dev, ADS1299_REG_MISC1, ADS1299_MISC1_SRB1);
		if (ret < 0) {
			LOG_ERR("ADS1299: Failed to enable SRB1");
			return ret;
		}
		LOG_INF("ADS1299: SRB1 enabled as reference");
	} else {
		ret = ads1299_write_reg(dev, ADS1299_REG_MISC1, 0x00);
		if (ret < 0) {
			LOG_WRN("ADS1299: Failed to disable SRB1");
		}
	}

	/* Set START LOW after configuration (ready for trigger) */
	ads1299_start_pin(cfg, 0);
	k_msleep(1);

	/* Mark hardware as present and fully initialized */
	data->hw_present = true;

	LOG_INF("ADS1299 initialized: gain=%d, rate=%d SPS",
		cfg->gain, cfg->sample_rate);

	return 0;
}

/*
 * Power Management
 */
#ifdef CONFIG_PM_DEVICE
static int ads1299_pm_action(const struct device *dev,
			     enum pm_device_action action)
{
	const struct ads1299_config *cfg = dev->config;
	struct ads1299_data *data = dev->data;

	switch (action) {
	case PM_DEVICE_ACTION_SUSPEND:
		if (data->running) {
			ads1299_send_cmd(dev, ADS1299_CMD_SDATAC);
			ads1299_send_cmd(dev, ADS1299_CMD_STOP);
		}
		ads1299_send_cmd(dev, ADS1299_CMD_STANDBY);
		gpio_pin_set(cfg->pwdn_gpio.port, cfg->pwdn_gpio.pin, 0);  /* Physical LOW = Power down */
		break;

	case PM_DEVICE_ACTION_RESUME:
		gpio_pin_set(cfg->pwdn_gpio.port, cfg->pwdn_gpio.pin, 1);  /* Physical HIGH = Power on */
		k_msleep(10);
		ads1299_send_cmd(dev, ADS1299_CMD_WAKEUP);
		k_msleep(10);
		break;

	default:
		return -ENOTSUP;
	}

	return 0;
}
#endif /* CONFIG_PM_DEVICE */

/*
 * Sensor Driver API
 */
static DEVICE_API(sensor, ads1299_driver_api) = {
	.sample_fetch = ads1299_sample_fetch,
	.channel_get = ads1299_channel_get,
	.attr_set = ads1299_attr_set,
	.trigger_set = ads1299_trigger_set,
};

/*
 * Device Instantiation
 */
#define ADS1299_SPI_OPERATION (SPI_WORD_SET(8) | SPI_TRANSFER_MSB | SPI_MODE_CPHA)

#define ADS1299_INIT(inst)                                                     \
	static struct ads1299_data ads1299_data_##inst;                        \
                                                                               \
	static const struct ads1299_config ads1299_config_##inst = {           \
		.spi = SPI_DT_SPEC_INST_GET(inst, ADS1299_SPI_OPERATION, 2),   \
		.drdy_gpio = GPIO_DT_SPEC_INST_GET(inst, drdy_gpios),          \
		.reset_gpio = GPIO_DT_SPEC_INST_GET(inst, reset_gpios),        \
		.pwdn_gpio = GPIO_DT_SPEC_INST_GET(inst, pwdn_gpios),          \
		.start_gpio = GPIO_DT_SPEC_INST_GET_OR(inst, start_gpios, {0}),\
		.gain = DT_INST_PROP_OR(inst, gain, 24),                       \
		.sample_rate = DT_INST_PROP_OR(inst, sample_rate, 250),        \
		.bias_enabled = DT_INST_PROP_OR(inst, bias_enabled, false),    \
		.srb1_enabled = DT_INST_PROP_OR(inst, srb1_enabled, false),    \
	};                                                                     \
                                                                               \
	PM_DEVICE_DT_INST_DEFINE(inst, ads1299_pm_action);                     \
                                                                               \
	SENSOR_DEVICE_DT_INST_DEFINE(inst,                                     \
				     ads1299_init,                              \
				     PM_DEVICE_DT_INST_GET(inst),               \
				     &ads1299_data_##inst,                      \
				     &ads1299_config_##inst,                    \
				     POST_KERNEL,                               \
				     CONFIG_SENSOR_ADS1299_INIT_PRIORITY,       \
				     &ads1299_driver_api);

DT_INST_FOREACH_STATUS_OKAY(ADS1299_INIT)
