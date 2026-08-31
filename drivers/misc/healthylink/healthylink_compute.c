/*
 * HealthyLink Compute Module Driver
 * Copyright (c) 2025 ProtoCentral
 * SPDX-License-Identifier: MIT
 *
 * Driver for external AI/ML compute accelerator module.
 * Supports inference offloading to external TPU/FPGA/MCU.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include <healthylink/healthylink.h>
#include <healthylink/healthylink_eeprom.h>
#include "healthylink_core.h"

LOG_MODULE_REGISTER(healthylink_compute, CONFIG_HEALTHYLINK_LOG_LEVEL);

/* HealthyLink Compute Commands (generic protocol) */
#define AI_CMD_NOP              0x00
#define AI_CMD_STATUS           0x01
#define AI_CMD_RESET            0x02
#define AI_CMD_LOAD_INPUT       0x10
#define AI_CMD_RUN_INFERENCE    0x20
#define AI_CMD_READ_OUTPUT      0x30
#define AI_CMD_GET_INFO         0xF0

/* Status bits */
#define AI_STATUS_READY         BIT(0)
#define AI_STATUS_BUSY          BIT(1)
#define AI_STATUS_ERROR         BIT(2)
#define AI_STATUS_OUTPUT_READY  BIT(3)

struct healthylink_compute_data {
	const struct device *spi_dev;
	struct spi_config spi_cfg;
	const struct device *irq_port;
	uint8_t irq_pin;
	const struct device *reset_port;
	uint8_t reset_pin;
	struct gpio_callback irq_cb;
	struct k_sem inference_done;

	uint8_t accelerator_type;
	uint16_t inference_timeout_ms;
	uint16_t input_size;
	uint16_t output_size;

	bool initialized;
};

static struct healthylink_compute_data ai_data;

/* SPI transaction helpers */
static int ai_send_cmd(struct healthylink_compute_data *data, uint8_t cmd)
{
	uint8_t tx_buf[1] = {cmd};
	struct spi_buf tx = {.buf = tx_buf, .len = 1};
	struct spi_buf_set tx_set = {.buffers = &tx, .count = 1};

	return spi_write(data->spi_dev, &data->spi_cfg, &tx_set);
}

static int ai_get_status(struct healthylink_compute_data *data, uint8_t *status)
{
	uint8_t tx_buf[2] = {AI_CMD_STATUS, 0x00};
	uint8_t rx_buf[2] = {0};

	struct spi_buf tx = {.buf = tx_buf, .len = 2};
	struct spi_buf_set tx_set = {.buffers = &tx, .count = 1};
	struct spi_buf rx = {.buf = rx_buf, .len = 2};
	struct spi_buf_set rx_set = {.buffers = &rx, .count = 1};

	int ret = spi_transceive(data->spi_dev, &data->spi_cfg, &tx_set, &rx_set);
	if (ret == 0) {
		*status = rx_buf[1];
	}
	return ret;
}

/* IRQ handler - inference complete */
static void ai_irq_handler(const struct device *dev,
			   struct gpio_callback *cb, uint32_t pins)
{
	k_sem_give(&ai_data.inference_done);
}

/* Hardware reset */
static int ai_reset(struct healthylink_compute_data *data)
{
	if (data->reset_port == NULL) {
		return ai_send_cmd(data, AI_CMD_RESET);
	}

	/* Hardware reset pulse */
	gpio_pin_set(data->reset_port, data->reset_pin, 0);
	k_msleep(10);
	gpio_pin_set(data->reset_port, data->reset_pin, 1);
	k_msleep(100);  /* Wait for accelerator to boot */

	return 0;
}

/*
 * Public API: Load input tensor
 */
int healthylink_compute_load_input(const uint8_t *input_data, size_t len)
{
	struct healthylink_compute_data *data = &ai_data;

	if (!data->initialized || len > data->input_size) {
		return -EINVAL;
	}

	/* Build command packet: CMD + LENGTH(2) + DATA */
	size_t pkt_len = 1 + 2 + len;
	uint8_t *tx_buf = k_malloc(pkt_len);
	if (tx_buf == NULL) {
		return -ENOMEM;
	}

	tx_buf[0] = AI_CMD_LOAD_INPUT;
	tx_buf[1] = (len >> 8) & 0xFF;
	tx_buf[2] = len & 0xFF;
	memcpy(&tx_buf[3], input_data, len);

	struct spi_buf tx = {.buf = tx_buf, .len = pkt_len};
	struct spi_buf_set tx_set = {.buffers = &tx, .count = 1};

	int ret = spi_write(data->spi_dev, &data->spi_cfg, &tx_set);
	k_free(tx_buf);

	return ret;
}

/*
 * Public API: Run inference
 */
int healthylink_compute_run_inference(uint8_t *output_data, size_t *output_len,
				 k_timeout_t timeout)
{
	struct healthylink_compute_data *data = &ai_data;
	int ret;

	if (!data->initialized) {
		return -EINVAL;
	}

	/* Reset semaphore */
	k_sem_reset(&data->inference_done);

	/* Start inference */
	ret = ai_send_cmd(data, AI_CMD_RUN_INFERENCE);
	if (ret < 0) {
		return ret;
	}

	/* Wait for completion (IRQ or timeout) */
	ret = k_sem_take(&data->inference_done, timeout);
	if (ret != 0) {
		LOG_ERR("Inference timeout");
		return -ETIMEDOUT;
	}

	/* Check status */
	uint8_t status;
	ret = ai_get_status(data, &status);
	if (ret < 0) {
		return ret;
	}

	if (status & AI_STATUS_ERROR) {
		LOG_ERR("Accelerator error");
		return -EIO;
	}

	if (!(status & AI_STATUS_OUTPUT_READY)) {
		LOG_ERR("No output ready");
		return -EIO;
	}

	/* Read output */
	size_t read_len = MIN(*output_len, data->output_size);
	uint8_t *tx_buf = k_malloc(1 + 2 + read_len);
	uint8_t *rx_buf = k_malloc(1 + 2 + read_len);
	if (tx_buf == NULL || rx_buf == NULL) {
		k_free(tx_buf);
		k_free(rx_buf);
		return -ENOMEM;
	}

	tx_buf[0] = AI_CMD_READ_OUTPUT;
	tx_buf[1] = (read_len >> 8) & 0xFF;
	tx_buf[2] = read_len & 0xFF;
	memset(&tx_buf[3], 0, read_len);

	struct spi_buf tx = {.buf = tx_buf, .len = 1 + 2 + read_len};
	struct spi_buf_set tx_set = {.buffers = &tx, .count = 1};
	struct spi_buf rx = {.buf = rx_buf, .len = 1 + 2 + read_len};
	struct spi_buf_set rx_set = {.buffers = &rx, .count = 1};

	ret = spi_transceive(data->spi_dev, &data->spi_cfg, &tx_set, &rx_set);
	if (ret == 0) {
		memcpy(output_data, &rx_buf[3], read_len);
		*output_len = read_len;
	}

	k_free(tx_buf);
	k_free(rx_buf);

	return ret;
}

/* Module Probe Function */
int healthylink_compute_probe(const struct device *parent,
			 const struct healthylink_header *header)
{
	int ret;

	LOG_INF("Probing HealthyLink Compute module (serial: %u)", header->serial);

	/* Parse module configuration from EEPROM */
	const struct healthylink_config_ai *cfg =
		(const struct healthylink_config_ai *)header->config;

	ai_data.accelerator_type = cfg->accelerator_type;
	ai_data.inference_timeout_ms = cfg->inference_timeout_ms ?: 1000;
	ai_data.input_size = cfg->input_size ?: 4096;
	ai_data.output_size = cfg->output_size ?: 256;

	LOG_INF("Config: type=%u, timeout=%u ms, input=%u, output=%u",
		ai_data.accelerator_type, ai_data.inference_timeout_ms,
		ai_data.input_size, ai_data.output_size);

	/* Get SPI device (SPI6) */
	ai_data.spi_dev = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(spi6));
	if (ai_data.spi_dev == NULL || !device_is_ready(ai_data.spi_dev)) {
		LOG_ERR("SPI6 not ready");
		return -ENODEV;
	}

	/* Configure SPI */
	uint8_t spi_speed = cfg->spi_speed_mhz ?: 20;
	ai_data.spi_cfg.frequency = spi_speed * 1000000;
	ai_data.spi_cfg.operation = SPI_WORD_SET(8) | SPI_TRANSFER_MSB;
	ai_data.spi_cfg.slave = 0;

	/* Initialize semaphore */
	k_sem_init(&ai_data.inference_done, 0, 1);

	/* Configure GPIO pins (from devicetree) */
	/* TODO: Get GPIO specs from devicetree node */
	ai_data.irq_port = DEVICE_DT_GET(DT_NODELABEL(gpioi));
	ai_data.irq_pin = 3;  /* PI3 */
	ai_data.reset_port = DEVICE_DT_GET(DT_NODELABEL(gpioi));
	ai_data.reset_pin = 6;  /* PI6 */

	if (device_is_ready(ai_data.reset_port)) {
		ret = gpio_pin_configure(ai_data.reset_port, ai_data.reset_pin,
					 GPIO_OUTPUT_ACTIVE);
		if (ret < 0) {
			LOG_WRN("Failed to configure reset GPIO: %d", ret);
			ai_data.reset_port = NULL;
		}
	}

	if (device_is_ready(ai_data.irq_port)) {
		ret = gpio_pin_configure(ai_data.irq_port, ai_data.irq_pin,
					 GPIO_INPUT | GPIO_PULL_UP);
		if (ret == 0) {
			gpio_init_callback(&ai_data.irq_cb, ai_irq_handler,
					   BIT(ai_data.irq_pin));
			gpio_add_callback(ai_data.irq_port, &ai_data.irq_cb);
			gpio_pin_interrupt_configure(ai_data.irq_port,
						     ai_data.irq_pin,
						     GPIO_INT_EDGE_FALLING);
		}
	}

	/* Reset accelerator */
	ret = ai_reset(&ai_data);
	if (ret < 0) {
		LOG_WRN("Reset failed: %d", ret);
	}

	/* Verify accelerator is responding */
	uint8_t status;
	ret = ai_get_status(&ai_data, &status);
	if (ret < 0) {
		LOG_ERR("Failed to read accelerator status: %d", ret);
		return ret;
	}

	if (!(status & AI_STATUS_READY)) {
		LOG_WRN("Accelerator not ready (status=0x%02X)", status);
		/* Continue anyway - may need more time */
	}

	ai_data.initialized = true;

	LOG_INF("HealthyLink Compute module initialized");
	LOG_INF("  Status: 0x%02X", status);

	return 0;
}

/* Module Remove Function */
void healthylink_compute_remove(const struct device *parent)
{
	if (!ai_data.initialized) {
		return;
	}

	LOG_INF("Removing HealthyLink Compute module");

	/* Disable IRQ */
	if (ai_data.irq_port != NULL) {
		gpio_pin_interrupt_configure(ai_data.irq_port, ai_data.irq_pin,
					     GPIO_INT_DISABLE);
		gpio_remove_callback(ai_data.irq_port, &ai_data.irq_cb);
	}

	/* Put accelerator in reset */
	if (ai_data.reset_port != NULL) {
		gpio_pin_set(ai_data.reset_port, ai_data.reset_pin, 0);
	}

	ai_data.initialized = false;

	LOG_INF("HealthyLink Compute module removed");
}
