/*
 * HealthyLink CAN Interface Module Driver
 * Copyright (c) 2025 ProtoCentral
 * SPDX-License-Identifier: MIT
 *
 * Driver for FDCAN lab equipment interface module.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/logging/log.h>

#include <healthylink/healthylink.h>
#include <healthylink/healthylink_eeprom.h>
#include "healthylink_core.h"

LOG_MODULE_REGISTER(healthylink_can, CONFIG_HEALTHYLINK_LOG_LEVEL);

struct healthylink_can_data {
	const struct device *can_dev;
	bool initialized;
	uint32_t bus_speed;
	uint32_t data_speed;
};

static struct healthylink_can_data can_data;

/* CAN RX callback */
static void can_rx_callback(const struct device *dev,
			    struct can_frame *frame, void *user_data)
{
	LOG_DBG("CAN RX: ID=0x%X DLC=%d", frame->id, frame->dlc);

	/* TODO: Process received CAN frame
	 * Could forward to IPC, log to recording, etc.
	 */
}

/* Module Probe Function */
int healthylink_can_probe(const struct device *parent,
			  const struct healthylink_header *header)
{
	int ret;

	LOG_INF("Probing CAN-INTERFACE module (serial: %u)", header->serial);

	/* Parse module configuration from EEPROM */
	const struct healthylink_config_can *cfg =
		(const struct healthylink_config_can *)header->config;

	can_data.bus_speed = cfg->default_bus_speed ?: 1000000;
	can_data.data_speed = cfg->default_data_speed ?: 0;

	LOG_INF("Config: bus_speed=%u, data_speed=%u",
		can_data.bus_speed, can_data.data_speed);

	/* Get CAN device (FDCAN1) */
	can_data.can_dev = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(fdcan1));
	if (can_data.can_dev == NULL || !device_is_ready(can_data.can_dev)) {
		LOG_ERR("FDCAN1 not ready");
		return -ENODEV;
	}

	/* Configure CAN bus speed */
	ret = can_set_bitrate(can_data.can_dev, can_data.bus_speed);
	if (ret < 0) {
		LOG_ERR("Failed to set CAN bitrate: %d", ret);
		return ret;
	}

#if IS_ENABLED(CONFIG_CAN_FD_MODE)
	if (can_data.data_speed > 0) {
		ret = can_set_bitrate_data(can_data.can_dev, can_data.data_speed);
		if (ret < 0) {
			LOG_WRN("Failed to set CAN-FD data bitrate: %d", ret);
			/* Continue without FD mode */
		}
	}
#endif

	/* Set CAN mode */
	ret = can_set_mode(can_data.can_dev, CAN_MODE_NORMAL);
	if (ret < 0) {
		LOG_ERR("Failed to set CAN mode: %d", ret);
		return ret;
	}

	/* Start CAN controller */
	ret = can_start(can_data.can_dev);
	if (ret < 0) {
		LOG_ERR("Failed to start CAN: %d", ret);
		return ret;
	}

	/* Add filter to receive all messages (for now) */
	struct can_filter filter = {
		.flags = 0,
		.id = 0,
		.mask = 0,  /* Accept all IDs */
	};

	ret = can_add_rx_filter(can_data.can_dev, can_rx_callback, NULL, &filter);
	if (ret < 0) {
		LOG_WRN("Failed to add CAN filter: %d", ret);
		/* Continue - may still be able to transmit */
	}

	can_data.initialized = true;

	LOG_INF("CAN-INTERFACE module initialized");
	return 0;
}

/* Module Remove Function */
void healthylink_can_remove(const struct device *parent)
{
	if (!can_data.initialized) {
		return;
	}

	LOG_INF("Removing CAN-INTERFACE module");

	/* Stop CAN controller */
	if (can_data.can_dev != NULL) {
		can_stop(can_data.can_dev);
	}

	can_data.initialized = false;

	LOG_INF("CAN-INTERFACE module removed");
}
