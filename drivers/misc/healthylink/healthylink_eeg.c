/*
 * HealthyLink EEG-8CH Module Driver
 * Copyright (c) 2025 ProtoCentral
 * SPDX-License-Identifier: MIT
 *
 * Driver for 8-channel 24-bit EEG expansion module using
 * Texas Instruments ADS1299 analog front-end.
 *
 * This driver uses the Zephyr sensor API to interface with the ADS1299.
 * The actual hardware communication is handled by the ads1299 sensor driver
 * (drivers/sensor/ads1299/).
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>

#include <healthylink/healthylink.h>
#include <healthylink/healthylink_eeprom.h>
#include "healthylink_core.h"

/* Include ADS1299 header for channel and attribute definitions */
#include <ads1299.h>

LOG_MODULE_REGISTER(healthylink_eeg, CONFIG_HEALTHYLINK_LOG_LEVEL);

/*
 * Driver Data Structures
 */
struct healthylink_eeg_data {
	const struct device *adc_dev;  /* ADS1299 sensor device */
	struct sensor_trigger drdy_trigger;
	int32_t samples[8];  /* 8 channels */
	bool running;
	bool initialized;

	/* Configuration from EEPROM */
	uint16_t sample_rate;
	uint8_t gain;
	uint8_t channel_mask;
};

static struct healthylink_eeg_data eeg_data;

/*
 * DRDY Trigger Handler - called when new data is available
 */
static void eeg_drdy_handler(const struct device *dev,
			     const struct sensor_trigger *trigger)
{
	struct sensor_value val;
	int ret;

	if (!eeg_data.running) {
		return;
	}

	/* Fetch new samples */
	ret = sensor_sample_fetch(dev);
	if (ret < 0) {
		LOG_ERR("Failed to fetch EEG samples: %d", ret);
		return;
	}

	/* Read all 8 channels using ADS1299-specific channel definitions */
	for (int ch = 0; ch < 8; ch++) {
		ret = sensor_channel_get(dev, SENSOR_CHAN_ADS1299_CH1 + ch, &val);
		if (ret == 0) {
			eeg_data.samples[ch] = val.val1;
		}
	}

	/* TODO: forward samples to the M4 over IPC using the EEG batch message
	 * type (0x30, HPI_IPC_MSG_TYPE_EEG_RAW). */

	LOG_DBG("EEG: %d %d %d %d %d %d %d %d",
		eeg_data.samples[0], eeg_data.samples[1],
		eeg_data.samples[2], eeg_data.samples[3],
		eeg_data.samples[4], eeg_data.samples[5],
		eeg_data.samples[6], eeg_data.samples[7]);
}

/*
 * Module Probe Function
 */
int healthylink_eeg_probe(const struct device *parent,
			  const struct healthylink_header *header)
{
	int ret;

	LOG_INF("Probing EEG-8CH module (serial: %u)", header->serial);

	/* Parse module configuration from EEPROM */
	const struct healthylink_config_eeg_8ch *cfg =
		(const struct healthylink_config_eeg_8ch *)header->config;

	eeg_data.sample_rate = cfg->default_sample_rate ?: 250;
	eeg_data.gain = cfg->default_gain ?: 24;  /* Default gain = 24 for EEG */
	eeg_data.channel_mask = cfg->channel_mask ?: 0xFF;  /* All channels */

	LOG_INF("Config: sample_rate=%u, gain=%u, channels=0x%02X",
		eeg_data.sample_rate, eeg_data.gain, eeg_data.channel_mask);

	/* Get the ADS1299 sensor device (defined on &spi4 in the EEG overlay,
	 * compatible "ti,ads1299"). */
	eeg_data.adc_dev = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(ads1299));
	if (eeg_data.adc_dev == NULL) {
		LOG_ERR("ADS1299 device not found in devicetree");
		return -ENODEV;
	}

	if (!device_is_ready(eeg_data.adc_dev)) {
		LOG_ERR("ADS1299 device not ready");
		return -ENODEV;
	}

	LOG_INF("ADS1299 sensor device: %s", eeg_data.adc_dev->name);

	/* Configure DRDY trigger */
	eeg_data.drdy_trigger.type = SENSOR_TRIG_DATA_READY;
	eeg_data.drdy_trigger.chan = SENSOR_CHAN_ALL;

	ret = sensor_trigger_set(eeg_data.adc_dev, &eeg_data.drdy_trigger,
				 eeg_drdy_handler);
	if (ret < 0) {
		LOG_ERR("Failed to set EEG trigger: %d", ret);
		return ret;
	}

	/* Start data acquisition using ADS1299-specific attribute */
	struct sensor_value start_val = {.val1 = 1, .val2 = 0};
	ret = sensor_attr_set(eeg_data.adc_dev, SENSOR_CHAN_ALL,
			      ADS1299_ATTR_START, &start_val);
	if (ret < 0) {
		LOG_WRN("Failed to start EEG acquisition: %d", ret);
		/* Continue anyway - trigger_set may have already started it */
	}

	eeg_data.running = true;
	eeg_data.initialized = true;

	LOG_INF("EEG-8CH module initialized (using ADS1299 sensor driver)");
	return 0;
}

/*
 * Module Remove Function
 */
void healthylink_eeg_remove(const struct device *parent)
{
	if (!eeg_data.initialized) {
		return;
	}

	LOG_INF("Removing EEG-8CH module");

	/* Stop sampling */
	eeg_data.running = false;

	/* Stop data acquisition */
	if (eeg_data.adc_dev != NULL) {
		struct sensor_value stop_val = {.val1 = 0, .val2 = 0};
		sensor_attr_set(eeg_data.adc_dev, SENSOR_CHAN_ALL,
				ADS1299_ATTR_START, &stop_val);

		/* Remove trigger */
		sensor_trigger_set(eeg_data.adc_dev, &eeg_data.drdy_trigger, NULL);
	}

	eeg_data.initialized = false;
	eeg_data.adc_dev = NULL;

	LOG_INF("EEG-8CH module removed");
}

/*
 * Get current EEG samples (for external access)
 */
int healthylink_eeg_get_samples(int32_t *samples, size_t count)
{
	if (!eeg_data.initialized || !eeg_data.running) {
		return -ENODEV;
	}

	if (samples == NULL || count == 0) {
		return -EINVAL;
	}

	size_t copy_count = MIN(count, 8);
	memcpy(samples, eeg_data.samples, copy_count * sizeof(int32_t));

	return copy_count;
}

/*
 * Check if EEG module is active
 */
bool healthylink_eeg_is_active(void)
{
	return eeg_data.initialized && eeg_data.running;
}
