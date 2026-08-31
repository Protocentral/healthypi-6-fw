/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal GC9503V MIPI-DSI display driver skeleton based on the ILI9881C driver
 * Uses the project's device tree and timings macros defined in display_gc9503v_dsi.h
 *
 * The init sequence below is intentionally small; consult the datasheet in
 * references/display for a full init sequence and any required register writes.
 */

#define DT_DRV_COMPAT galaxycore_gc9503v_dsi

#include <zephyr/kernel.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/mipi_dsi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>
#include "display_gc9503v_dsi.h"

LOG_MODULE_REGISTER(display_gc9503v_dsi, CONFIG_DISPLAY_LOG_LEVEL);

#define GC9503V_COLMOD_RGB565 0x50
#define GC9503V_COLMOD_RGB888 0x70

struct gc9503v_config {
	const struct device *mipi_dsi;
	const struct gpio_dt_spec reset;
	const struct gpio_dt_spec backlight;
	enum display_pixel_format pixel_format;
	uint8_t data_lanes;
	uint16_t width;
	uint16_t height;
	uint8_t channel;
};

struct gc9503v_init_cmd {
	uint8_t reg;
	uint8_t cmd_len;
	uint8_t cmd[64];
} __packed;

/* Initialization sequence taken from references/display GC9503V file.
 * The sequence ends with Sleep Out (0x11) and Display On (0x29) which
 * are handled in gc9503v_config with the required delays.
 */
static const struct gc9503v_init_cmd init_cmds[] = {
	{.reg = 0xF0, .cmd_len = 5, .cmd = {0x55,0xAA,0x52,0x08,0x00}},
	{.reg = 0xF6, .cmd_len = 2, .cmd = {0x5A,0x87}},
	{.reg = 0xC1, .cmd_len = 1, .cmd = {0x3F}},
	{.reg = 0xCD, .cmd_len = 1, .cmd = {0x25}},
	{.reg = 0xC9, .cmd_len = 1, .cmd = {0x10}},
	{.reg = 0xF8, .cmd_len = 1, .cmd = {0x8A}},
	{.reg = 0xAC, .cmd_len = 1, .cmd = {0x45}},
	{.reg = 0xA7, .cmd_len = 1, .cmd = {0x47}},
	{.reg = 0xA0, .cmd_len = 1, .cmd = {0xCC}},
	{.reg = 0x86, .cmd_len = 4, .cmd = {0x99,0xA3,0xA3,0x41}},

	{.reg = 0xFA, .cmd_len = 4, .cmd = {0x08,0x08,0x00,0x04}},
	{.reg = 0xA3, .cmd_len = 1, .cmd = {0x6E}},
	{.reg = 0xFD, .cmd_len = 3, .cmd = {0x3C,0x3C,0x00}},
	{.reg = 0x9A, .cmd_len = 1, .cmd = {0x6C}},
	{.reg = 0x9B, .cmd_len = 1, .cmd = {0x6C}},
	{.reg = 0x82, .cmd_len = 2, .cmd = {0x42,0x42}},
	{.reg = 0xB1, .cmd_len = 1, .cmd = {0x10}},
	{.reg = 0x7A, .cmd_len = 2, .cmd = {0x0F,0x13}},
	{.reg = 0x7B, .cmd_len = 2, .cmd = {0x0F,0x13}},
	{.reg = 0x69, .cmd_len = 7, .cmd = {0x14,0x22,0x14,0x22,0x44,0x22,0x08}},
	{.reg = 0x6B, .cmd_len = 1, .cmd = {0x07}},
	{.reg = 0x6D, .cmd_len = 32, .cmd = {0x07,0x1E,0x01,0x00,0x1F,0x09,0x0B,0x0D,0x0F,0x1E,0x1E,0x1E,0x1E,0x1E,0x1E,0x1E,0x1E,0x1E,0x1E,0x1E,0x1E,0x1E,0x1E,0x10,0x0E,0x0C,0x0A,0x1F,0x00,0x02,0x1E,0x08}},

	{.reg = 0x60, .cmd_len = 8, .cmd = {0x18,0x07,0x7A,0x7A,0x18,0x06,0x7A,0x7A}},
	{.reg = 0x63, .cmd_len = 8, .cmd = {0x13,0x1F,0x7A,0x7A,0x13,0x20,0x7A,0x7A}},
	{.reg = 0x64, .cmd_len = 16, .cmd = {0x18,0x03,0x03,0x25,0x03,0x03,0x18,0x02,0x03,0x26,0x03,0x03,0x7A,0x7A,0x7A,0x7A}},
	{.reg = 0x65, .cmd_len = 16, .cmd = {0x18,0x01,0x03,0x27,0x03,0x03,0x18,0x00,0x03,0x28,0x03,0x03,0x7A,0x7A,0x7A,0x7A}},
	{.reg = 0x66, .cmd_len = 16, .cmd = {0x10,0x01,0x03,0x29,0x03,0x03,0x10,0x02,0x03,0x2A,0x03,0x03,0x7A,0x7A,0x7A,0x7A}},
	{.reg = 0x67, .cmd_len = 16, .cmd = {0x10,0x03,0x03,0x2B,0x03,0x03,0x10,0x04,0x03,0x2C,0x03,0x03,0x7A,0x7A,0x7A,0x7A}},

	{.reg = 0xD1, .cmd_len = 52, .cmd = {0x00,0x00,0x00,0x08,0x00,0x10,0x00,0x24,0x00,0x30,0x00,0x40,0x00,0x59,0x00,0x80,0x00,0x9D,0x00,0xCF,0x00,0xFC,0x01,0x50,0x01,0x9E,0x01,0xA0,0x01,0xE8,0x02,0x46,0x02,0x81,0x02,0xCC,0x03,0x02,0x03,0x60,0x03,0xB0,0x03,0xC1,0x03,0xCD,0x03,0xD8,0x03,0xE0,0x03,0xFF}},
	{.reg = 0xD2, .cmd_len = 52, .cmd = {0x00,0x00,0x00,0x08,0x00,0x10,0x00,0x24,0x00,0x30,0x00,0x40,0x00,0x59,0x00,0x80,0x00,0x9D,0x00,0xCF,0x00,0xFC,0x01,0x50,0x01,0x9E,0x01,0xA0,0x01,0xE8,0x02,0x46,0x02,0x81,0x02,0xCC,0x03,0x02,0x03,0x60,0x03,0xB0,0x03,0xC1,0x03,0xCD,0x03,0xD8,0x03,0xE0,0x03,0xFF}},
	{.reg = 0xD3, .cmd_len = 52, .cmd = {0x00,0x00,0x00,0x08,0x00,0x10,0x00,0x24,0x00,0x30,0x00,0x40,0x00,0x59,0x00,0x80,0x00,0x9D,0x00,0xCF,0x00,0xFC,0x01,0x50,0x01,0x9E,0x01,0xA0,0x01,0xE8,0x02,0x46,0x02,0x81,0x02,0xCC,0x03,0x02,0x03,0x60,0x03,0xB0,0x03,0xC1,0x03,0xCD,0x03,0xD8,0x03,0xE0,0x03,0xFF}},
	{.reg = 0xD4, .cmd_len = 52, .cmd = {0x00,0x00,0x00,0x08,0x00,0x10,0x00,0x24,0x00,0x30,0x00,0x40,0x00,0x59,0x00,0x80,0x00,0x9D,0x00,0xCF,0x00,0xFC,0x01,0x50,0x01,0x9E,0x01,0xA0,0x01,0xE8,0x02,0x46,0x02,0x81,0x02,0xCC,0x03,0x02,0x03,0x60,0x03,0xB0,0x03,0xC1,0x03,0xCD,0x03,0xD8,0x03,0xE0,0x03,0xFF}},
	{.reg = 0xD5, .cmd_len = 52, .cmd = {0x00,0x00,0x00,0x08,0x00,0x10,0x00,0x24,0x00,0x30,0x00,0x40,0x00,0x59,0x00,0x80,0x00,0x9D,0x00,0xCF,0x00,0xFC,0x01,0x50,0x01,0x9E,0x01,0xA0,0x01,0xE8,0x02,0x46,0x02,0x81,0x02,0xCC,0x03,0x02,0x03,0x60,0x03,0xB0,0x03,0xC1,0x03,0xCD,0x03,0xD8,0x03,0xE0,0x03,0xFF}},
	{.reg = 0xD6, .cmd_len = 52, .cmd = {0x00,0x00,0x00,0x08,0x00,0x10,0x00,0x24,0x00,0x30,0x00,0x40,0x00,0x59,0x00,0x80,0x00,0x9D,0x00,0xCF,0x00,0xFC,0x01,0x50,0x01,0x9E,0x01,0xA0,0x01,0xE8,0x02,0x46,0x02,0x81,0x02,0xCC,0x03,0x02,0x03,0x60,0x03,0xB0,0x03,0xC1,0x03,0xCD,0x03,0xD8,0x03,0xE0,0x03,0xFF}},
	

};

static int gc9503v_write_reg(const struct device *dev, uint8_t reg, const uint8_t *buf, size_t len)
{
	int ret;
	const struct gc9503v_config *cfg = dev->config;

	struct mipi_dsi_msg msg = {
		.cmd = reg,
		.tx_buf = buf,
		.tx_len = len,
		.flags = MIPI_DSI_MSG_USE_LPM,
	};

	switch (len) {
	case 0U:
		msg.type = MIPI_DSI_DCS_SHORT_WRITE;
		break;
	case 1U:
		msg.type = MIPI_DSI_DCS_SHORT_WRITE_PARAM;
		break;
	default:
		msg.type = MIPI_DSI_DCS_LONG_WRITE;
		break;
	}

	ret = mipi_dsi_transfer(cfg->mipi_dsi, cfg->channel, &msg);
	if (ret < 0) {
		LOG_ERR("Failed writing reg: 0x%x result: (%d)", reg, ret);
		return ret;
	}

	return 0;
}

static int gc9503v_write_reg_val(const struct device *dev, uint8_t reg, uint8_t value)
{
	return gc9503v_write_reg(dev, reg, &value, 1);
}

static int gc9503v_write_sequence(const struct device *dev, const struct gc9503v_init_cmd *cmd,
					   uint8_t nr_cmds)
{
	int ret = 0;

	for (int i = 0; i < nr_cmds && ret == 0; i++) {
		ret = gc9503v_write_reg(dev, cmd->reg, cmd->cmd, cmd->cmd_len);
		if (ret < 0) {
			LOG_ERR("Failed writing sequence: 0x%x result: (%d)", cmd->reg, ret);
			return ret;
		}
		cmd++;
	}

	return ret;
}

static int gc9503v_config(const struct device *dev)
{
	const struct gc9503v_config *cfg = dev->config;
	int ret;

	/* Send the software init sequence */
	ret = gc9503v_write_sequence(dev, init_cmds, ARRAY_SIZE(init_cmds));
	if (ret < 0) {
		return ret;
	}

	/* Small delay to allow registers to settle */
	k_msleep(50);

	/* Set Memory Access Control (MADCTL) - 0x36
	 * Bit 7: MY (Page Address Order) - 0 = top to bottom
	 * Bit 6: MX (Column Address Order) - 0 = left to right  
	 * Bit 5: MV (Page/Column Exchange) - 0 = normal
	 * Bit 4: ML (Line Address Order) - 0 = LCD refresh top to bottom
	 * Bit 3: BGR (RGB/BGR Order) - 0 = RGB
	 * Bit 2: MH (Display Data Latch) - 0 = LCD refresh left to right
	 * Value 0x00 = normal orientation, RGB order
	 */
	ret = gc9503v_write_reg(dev, 0x36, (const uint8_t[]){0x00}, 1);
	if (ret < 0) {
		LOG_ERR("Failed to set MADCTL");
		return ret;
	}

	/* Set pixel format to RGB565 */
	ret = gc9503v_write_reg(dev, 0x3A, (const uint8_t[]){0x50}, 1);
	if (ret < 0) {
		return ret;
	}

	/* Follow reference: Exit sleep (0x11), wait 200 ms, then Display ON (0x29) */
	ret = gc9503v_write_reg(dev, MIPI_DCS_EXIT_SLEEP_MODE, NULL, 0);
	if (ret < 0) {
		return ret;
	}

	/* Datasheet requests a 200 ms delay after Sleep Out */
	k_msleep(200);

	ret = gc9503v_write_reg(dev, MIPI_DCS_SET_DISPLAY_ON, NULL, 0);
	if (ret < 0) {
		return ret;
	}

	return 0;
}

static int gc9503v_blanking_on(const struct device *dev)
{
	const struct gc9503v_config *cfg = dev->config;
	int ret;

	if (cfg->backlight.port != NULL) {
		ret = gpio_pin_set_dt(&cfg->backlight, 0);
		if (ret) {
			LOG_ERR("Disable backlight failed! (%d)", ret);
			return ret;
		}
	}

	return gc9503v_write_reg(dev, MIPI_DCS_SET_DISPLAY_OFF, NULL, 0);
}

static int gc9503v_blanking_off(const struct device *dev)
{
	const struct gc9503v_config *cfg = dev->config;
	int ret;

	if (cfg->backlight.port != NULL) {
		ret = gpio_pin_set_dt(&cfg->backlight, 1);
		if (ret) {
			LOG_ERR("Enable backlight failed! (%d)", ret);
			return ret;
		}
	}

	return gc9503v_write_reg(dev, MIPI_DCS_SET_DISPLAY_ON, NULL, 0);
}

static void gc9503v_get_capabilities(const struct device *dev,
					  struct display_capabilities *capabilities)
{
	const struct gc9503v_config *cfg = dev->config;

	memset(capabilities, 0, sizeof(struct display_capabilities));
	capabilities->x_resolution = cfg->width;
	capabilities->y_resolution = cfg->height;
	capabilities->supported_pixel_formats = cfg->pixel_format;
	capabilities->current_pixel_format = cfg->pixel_format;
}

static int gc9503v_pixel_format(const struct device *dev,
				 const enum display_pixel_format pixel_format)
{
	const struct gc9503v_config *config = dev->config;

	LOG_WRN("Pixel format change not implemented");
	if (pixel_format == config->pixel_format) {
		return 0;
	}

	return -ENOTSUP;
}

static int gc9503v_set_orientation(const struct device *dev,
				 const enum display_orientation orientation)
{
	LOG_WRN("Orientation change not implemented");
	return -ENOTSUP;
}

static DEVICE_API(display, gc9503v_api) = {
	.blanking_on = gc9503v_blanking_on,
	.blanking_off = gc9503v_blanking_off,
	.set_pixel_format = gc9503v_pixel_format,
	.get_capabilities = gc9503v_get_capabilities,
};

static int gc9503v_init(const struct device *dev)
{
	const struct gc9503v_config *cfg = dev->config;
	struct mipi_dsi_device mdev;
	int ret;

	LOG_INF("GC9503V disp init !");

	if (cfg->reset.port) {
		if (!gpio_is_ready_dt(&cfg->reset)) {
			LOG_ERR("Reset GPIO device is not ready!");
			return -ENODEV;
		}
		k_sleep(K_MSEC(1));

		ret = gpio_pin_configure_dt(&cfg->reset, GPIO_OUTPUT_INACTIVE);
		if (ret < 0) {
			LOG_ERR("Reset display failed! (%d)", ret);
			return ret;
		}

		ret = gpio_pin_set_dt(&cfg->reset, 0);
		if (ret < 0) {
			LOG_ERR("Reset display failed! (%d)", ret);
			return ret;
		}
		k_sleep(K_MSEC(1));

		ret = gpio_pin_set_dt(&cfg->reset, 1);
		if (ret < 0) {
			LOG_ERR("Enable display failed! (%d)", ret);
			return ret;
		}
		k_sleep(K_MSEC(50));
	}

	/* attach to MIPI-DSI host */
	if (cfg->pixel_format == PIXEL_FORMAT_RGB_565) {
		mdev.pixfmt = MIPI_DSI_PIXFMT_RGB565;
	} else {
		mdev.pixfmt = MIPI_DSI_PIXFMT_RGB888;
	}
	mdev.data_lanes = cfg->data_lanes;
	/* CRITICAL: Use VIDEO_BURST mode instead of LPM for proper timing
	 * VIDEO_BURST ensures continuous clock and proper horizontal sync
	 * This should fix the horizontal shift/wrapping issue
	 */
	mdev.mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST;
	mdev.timings.hactive = cfg->width;
	mdev.timings.hbp = GC9503V_HBP;
	mdev.timings.hfp = GC9503V_HFP;
	mdev.timings.hsync = GC9503V_HSYNC;
	mdev.timings.vactive = cfg->height;
	mdev.timings.vbp = GC9503V_VBP;
	mdev.timings.vfp = GC9503V_VFP;
	mdev.timings.vsync = GC9503V_VSYNC;

	ret = mipi_dsi_attach(cfg->mipi_dsi, cfg->channel, &mdev);
	if (ret < 0) {
		LOG_ERR("Could not attach to MIPI-DSI host");
		return ret;
	}

	if (cfg->backlight.port != NULL) {
		ret = gpio_pin_configure_dt(&cfg->backlight, GPIO_OUTPUT_ACTIVE);
		if (ret < 0) {
			LOG_ERR("Could not configure backlight GPIO (%d)", ret);
			return ret;
		}
	}

	ret = gc9503v_config(dev);
	if (ret) {
		LOG_ERR("DSI init sequence failed! (%d)", ret);
		return ret;
	}

	return 0;
}

#define GC9503V_DEFINE(n) \
	static const struct gc9503v_config gc9503v_config_##n = { \
		.mipi_dsi = DEVICE_DT_GET(DT_INST_BUS(n)), \
		.reset = GPIO_DT_SPEC_INST_GET_OR(n, reset_gpios, {0}), \
		.backlight = GPIO_DT_SPEC_INST_GET_OR(n, bl_gpios, {0}), \
		.data_lanes = DT_INST_PROP_BY_IDX(n, data_lanes, 0), \
		.width = DT_INST_PROP(n, width), \
		.height = DT_INST_PROP(n, height), \
		.channel = DT_INST_REG_ADDR(n), \
		.pixel_format = DT_INST_PROP(n, pixel_format), \
	}; \
	DEVICE_DT_INST_DEFINE(n, &gc9503v_init, NULL, NULL, &gc9503v_config_##n, POST_KERNEL, \
		CONFIG_DISPLAY_GC9503V_DSI_INIT_PRIORITY, &gc9503v_api);

DT_INST_FOREACH_STATUS_OKAY(GC9503V_DEFINE)
