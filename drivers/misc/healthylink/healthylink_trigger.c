/*
 * HealthyLink Trigger I/O Module Driver
 * Copyright (c) 2025 ProtoCentral
 * SPDX-License-Identifier: MIT
 *
 * Driver for lab trigger input/output module with precise timestamping.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include <healthylink/healthylink.h>
#include <healthylink/healthylink_eeprom.h>
#include "healthylink_core.h"

LOG_MODULE_REGISTER(healthylink_trigger, CONFIG_HEALTHYLINK_LOG_LEVEL);

/* GPIO definitions for trigger pins */
#define TRIG_OUT_0_NODE DT_NODELABEL(gpioi)
#define TRIG_OUT_0_PIN  2
#define TRIG_OUT_1_NODE DT_NODELABEL(gpioi)
#define TRIG_OUT_1_PIN  3

#define TRIG_IN_0_NODE DT_NODELABEL(gpioi)
#define TRIG_IN_0_PIN  6
#define TRIG_IN_1_NODE DT_NODELABEL(gpioa)
#define TRIG_IN_1_PIN  9
#define TRIG_IN_2_NODE DT_NODELABEL(gpioa)
#define TRIG_IN_2_PIN  10

#define NUM_TRIGGER_OUTPUTS 2
#define NUM_TRIGGER_INPUTS  3

struct trigger_event {
	uint32_t timestamp_us;
	uint8_t input_num;
	uint8_t edge;  /* 0=falling, 1=rising */
};

struct healthylink_trigger_data {
	const struct device *out_ports[NUM_TRIGGER_OUTPUTS];
	uint8_t out_pins[NUM_TRIGGER_OUTPUTS];
	const struct device *in_ports[NUM_TRIGGER_INPUTS];
	uint8_t in_pins[NUM_TRIGGER_INPUTS];
	struct gpio_callback in_callbacks[NUM_TRIGGER_INPUTS];

	/* Event ring buffer */
	struct trigger_event events[32];
	uint8_t event_head;
	uint8_t event_tail;
	struct k_sem event_sem;

	uint16_t debounce_us;
	bool initialized;
};

static struct healthylink_trigger_data trigger_data;

/* Get current timestamp in microseconds */
static uint32_t get_timestamp_us(void)
{
	return (uint32_t)(k_uptime_get() * 1000ULL +
			  (k_cycle_get_32() / (CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC / 1000000)));
}

/* Input interrupt handler */
static void trigger_input_handler(const struct device *dev,
				  struct gpio_callback *cb, uint32_t pins)
{
	struct healthylink_trigger_data *data = &trigger_data;
	uint32_t timestamp = get_timestamp_us();

	/* Determine which input triggered */
	for (int i = 0; i < NUM_TRIGGER_INPUTS; i++) {
		if (dev == data->in_ports[i] && (pins & BIT(data->in_pins[i]))) {
			/* Read current state to determine edge */
			int val = gpio_pin_get(dev, data->in_pins[i]);

			/* Store event in ring buffer */
			uint8_t next_head = (data->event_head + 1) % ARRAY_SIZE(data->events);
			if (next_head != data->event_tail) {
				data->events[data->event_head].timestamp_us = timestamp;
				data->events[data->event_head].input_num = i;
				data->events[data->event_head].edge = (val > 0) ? 1 : 0;
				data->event_head = next_head;
				k_sem_give(&data->event_sem);
			}

			LOG_DBG("Trigger IN_%d %s @ %u us",
				i, (val > 0) ? "RISE" : "FALL", timestamp);
		}
	}
}

/*
 * Public API: Set trigger output
 */
int healthylink_trigger_set_output(uint8_t output_num, bool state)
{
	if (output_num >= NUM_TRIGGER_OUTPUTS || !trigger_data.initialized) {
		return -EINVAL;
	}

	return gpio_pin_set(trigger_data.out_ports[output_num],
			    trigger_data.out_pins[output_num], state ? 1 : 0);
}

/*
 * Public API: Pulse trigger output
 */
int healthylink_trigger_pulse(uint8_t output_num, uint32_t duration_us)
{
	int ret;

	ret = healthylink_trigger_set_output(output_num, true);
	if (ret < 0) {
		return ret;
	}

	k_busy_wait(duration_us);

	return healthylink_trigger_set_output(output_num, false);
}

/*
 * Public API: Get pending trigger event
 */
int healthylink_trigger_get_event(struct trigger_event *event, k_timeout_t timeout)
{
	struct healthylink_trigger_data *data = &trigger_data;

	if (k_sem_take(&data->event_sem, timeout) != 0) {
		return -EAGAIN;
	}

	if (data->event_tail == data->event_head) {
		return -EAGAIN;
	}

	*event = data->events[data->event_tail];
	data->event_tail = (data->event_tail + 1) % ARRAY_SIZE(data->events);

	return 0;
}

/* Module Probe Function */
int healthylink_trigger_probe(const struct device *parent,
			      const struct healthylink_header *header)
{
	int ret;

	LOG_INF("Probing TRIGGER-IO module (serial: %u)", header->serial);

	/* Parse module configuration from EEPROM */
	const struct healthylink_config_trigger *cfg =
		(const struct healthylink_config_trigger *)header->config;

	trigger_data.debounce_us = cfg->debounce_us;

	LOG_INF("Config: debounce=%u us", trigger_data.debounce_us);

	/* Initialize event semaphore */
	k_sem_init(&trigger_data.event_sem, 0, K_SEM_MAX_LIMIT);

	/* Configure output GPIOs */
	trigger_data.out_ports[0] = DEVICE_DT_GET(TRIG_OUT_0_NODE);
	trigger_data.out_pins[0] = TRIG_OUT_0_PIN;
	trigger_data.out_ports[1] = DEVICE_DT_GET(TRIG_OUT_1_NODE);
	trigger_data.out_pins[1] = TRIG_OUT_1_PIN;

	for (int i = 0; i < NUM_TRIGGER_OUTPUTS; i++) {
		if (!device_is_ready(trigger_data.out_ports[i])) {
			LOG_ERR("Output GPIO %d not ready", i);
			return -ENODEV;
		}
		ret = gpio_pin_configure(trigger_data.out_ports[i],
					 trigger_data.out_pins[i],
					 GPIO_OUTPUT_INACTIVE);
		if (ret < 0) {
			LOG_ERR("Failed to configure output %d: %d", i, ret);
			return ret;
		}
	}

	/* Configure input GPIOs */
	trigger_data.in_ports[0] = DEVICE_DT_GET(TRIG_IN_0_NODE);
	trigger_data.in_pins[0] = TRIG_IN_0_PIN;
	trigger_data.in_ports[1] = DEVICE_DT_GET(TRIG_IN_1_NODE);
	trigger_data.in_pins[1] = TRIG_IN_1_PIN;
	trigger_data.in_ports[2] = DEVICE_DT_GET(TRIG_IN_2_NODE);
	trigger_data.in_pins[2] = TRIG_IN_2_PIN;

	for (int i = 0; i < NUM_TRIGGER_INPUTS; i++) {
		if (!device_is_ready(trigger_data.in_ports[i])) {
			LOG_ERR("Input GPIO %d not ready", i);
			return -ENODEV;
		}

		ret = gpio_pin_configure(trigger_data.in_ports[i],
					 trigger_data.in_pins[i],
					 GPIO_INPUT | GPIO_PULL_DOWN);
		if (ret < 0) {
			LOG_ERR("Failed to configure input %d: %d", i, ret);
			return ret;
		}

		/* Setup interrupt */
		gpio_init_callback(&trigger_data.in_callbacks[i],
				   trigger_input_handler,
				   BIT(trigger_data.in_pins[i]));

		ret = gpio_add_callback(trigger_data.in_ports[i],
					&trigger_data.in_callbacks[i]);
		if (ret < 0) {
			LOG_WRN("Failed to add callback for input %d: %d", i, ret);
		}

		ret = gpio_pin_interrupt_configure(trigger_data.in_ports[i],
						   trigger_data.in_pins[i],
						   GPIO_INT_EDGE_BOTH);
		if (ret < 0) {
			LOG_WRN("Failed to configure interrupt for input %d: %d", i, ret);
		}
	}

	trigger_data.initialized = true;

	LOG_INF("TRIGGER-IO module initialized");
	LOG_INF("  Outputs: PI2, PI3");
	LOG_INF("  Inputs: PI6, PA9, PA10");

	return 0;
}

/* Module Remove Function */
void healthylink_trigger_remove(const struct device *parent)
{
	if (!trigger_data.initialized) {
		return;
	}

	LOG_INF("Removing TRIGGER-IO module");

	/* Disable interrupts */
	for (int i = 0; i < NUM_TRIGGER_INPUTS; i++) {
		gpio_pin_interrupt_configure(trigger_data.in_ports[i],
					     trigger_data.in_pins[i],
					     GPIO_INT_DISABLE);
		gpio_remove_callback(trigger_data.in_ports[i],
				     &trigger_data.in_callbacks[i]);
	}

	/* Set outputs low */
	for (int i = 0; i < NUM_TRIGGER_OUTPUTS; i++) {
		gpio_pin_set(trigger_data.out_ports[i],
			     trigger_data.out_pins[i], 0);
	}

	trigger_data.initialized = false;

	LOG_INF("TRIGGER-IO module removed");
}
