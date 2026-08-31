// ProtoCentral Electronics (info@protocentral.com)
// SPDX-License-Identifier: Apache-2.0

// AFE4400 driver header - async RTIO-capable
#pragma once

#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/rtio/rtio.h>
#include <zephyr/sys/atomic.h>

#define AFE4400_STACK_SIZE 1024

struct afe4400_config {
    struct spi_dt_spec spi;
    struct gpio_dt_spec drdy_pin;
    struct gpio_dt_spec reset_pin;
    struct gpio_dt_spec pdwn_pin;
};

struct afe4400_data {
    const struct device *dev;
    struct gpio_callback drdy_cb;
    
    /* Last decoded samples (one per result register 0x2A..0x2F) stored as signed 32-bit values */
    int32_t last_values[6];
    uint64_t last_sample_ts;
    
    /* Trigger support */
    const struct sensor_trigger *drdy_trigger;
    sensor_trigger_handler_t drdy_handler;
    struct k_work work;
};

/* AFE4400 initialization and configuration functions */
int afe4400_hardware_reset(const struct device *dev);
int afe4400_software_reset(const struct device *dev);
/* Controls AFE4400 power state using both PWDN GPIO pin and CONTROL2 register */
int afe4400_powerdown(const struct device *dev, bool powerdown);
int afe4400_configure_timing(const struct device *dev);
int afe4400_configure_led_current(const struct device *dev, uint8_t led1_current, uint8_t led2_current);
int afe4400_configure_tia_gain(const struct device *dev);
int afe4400_read_diagnostics(const struct device *dev, uint32_t *diag_status);
int afe4400_init_registers(const struct device *dev);


/* Blocking register accessors used during init or when RTIO not available */
uint32_t afe4400_reg_read_blocking(const struct device *dev, uint8_t reg);
int afe4400_reg_write_blocking(const struct device *dev, uint8_t reg, uint32_t val);

/* Wrapper register accessors - abstraction layer for future RTIO support
 * These functions provide a unified API that can support both blocking and non-blocking
 * register operations. Driver code should use these instead of the _blocking variants.
 */
uint32_t afe4400_reg_read(const struct device *dev, uint8_t reg);
int afe4400_reg_write(const struct device *dev, uint8_t reg, uint32_t val);

/* RTIO encoded data structures */
struct afe4400_decoder_header
{
	uint64_t timestamp;
} __attribute__((__packed__));

struct afe4400_encoded_data
{
    struct afe4400_decoder_header header;
    uint32_t led1_val;
    uint32_t amb1_val;
    uint32_t led2_val;
    uint32_t amb2_val;
    uint32_t diag_val;
};

/* RTIO async functions */
void afe4400_submit(const struct device *dev, struct rtio_iodev_sqe *iodev_sqe);
