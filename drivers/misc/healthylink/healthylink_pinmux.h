/*
 * HealthyLink Runtime Pin Multiplexer
 * Copyright (c) 2025 ProtoCentral
 * SPDX-License-Identifier: MIT
 *
 * Runtime pin configuration for HealthyLink expansion port.
 * Allows switching between SPI6/FDCAN and GPIO modes based on
 * the connected module type detected via EEPROM.
 *
 * STM32H757 Pin Assignments:
 *   SPI6:   PG13 (SCK/AF5), PG12 (MISO/AF5), PG14 (MOSI/AF5)
 *   FDCAN1: PH13 (TX/AF9), PH14 (RX/AF9)
 */

#ifndef DRIVERS_MISC_HEALTHYLINK_HEALTHYLINK_PINMUX_H_
#define DRIVERS_MISC_HEALTHYLINK_HEALTHYLINK_PINMUX_H_

#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Pin mode configuration options
 */
enum healthylink_pin_mode {
	/** Pins configured as high-impedance input (safe default) */
	HEALTHYLINK_PIN_MODE_HIZ,
	/** Pins configured as GPIO output */
	HEALTHYLINK_PIN_MODE_GPIO_OUTPUT,
	/** Pins configured as GPIO input */
	HEALTHYLINK_PIN_MODE_GPIO_INPUT,
	/** Pins configured as GPIO input with pull-up */
	HEALTHYLINK_PIN_MODE_GPIO_INPUT_PULLUP,
	/** Pins configured as GPIO input with pull-down */
	HEALTHYLINK_PIN_MODE_GPIO_INPUT_PULLDOWN,
};

/**
 * @brief Configure SPI6 pins for SPI alternate function
 *
 * Configures PG12, PG13, PG14 as SPI6 (AF5) with:
 *   - Very high speed
 *   - Push-pull output
 *   - No pull-up/pull-down
 *
 * @return 0 on success, negative errno on failure
 */
int healthylink_pinmux_spi6_enable(void);

/**
 * @brief Configure SPI6 pins as GPIO
 *
 * Reconfigures PG12, PG13, PG14 from SPI6 alternate function
 * to general-purpose I/O mode.
 *
 * @param mode GPIO mode configuration
 * @return 0 on success, negative errno on failure
 */
int healthylink_pinmux_spi6_to_gpio(enum healthylink_pin_mode mode);

/**
 * @brief Set SPI6 GPIO pin values (when in GPIO output mode)
 *
 * @param sck_val  Value for PG13 (0 or 1)
 * @param miso_val Value for PG12 (0 or 1)
 * @param mosi_val Value for PG14 (0 or 1)
 */
void healthylink_pinmux_spi6_gpio_set(uint8_t sck_val, uint8_t miso_val, uint8_t mosi_val);

/**
 * @brief Read SPI6 GPIO pin values (when in GPIO input mode)
 *
 * @param sck_val  Pointer to store PG13 value (can be NULL)
 * @param miso_val Pointer to store PG12 value (can be NULL)
 * @param mosi_val Pointer to store PG14 value (can be NULL)
 */
void healthylink_pinmux_spi6_gpio_get(uint8_t *sck_val, uint8_t *miso_val, uint8_t *mosi_val);

/**
 * @brief Configure FDCAN1 pins for CAN alternate function
 *
 * Configures PH13, PH14 as FDCAN1 (AF9) with:
 *   - Very high speed
 *   - Push-pull output (TX), input (RX)
 *   - No pull-up/pull-down
 *
 * @return 0 on success, negative errno on failure
 */
int healthylink_pinmux_fdcan1_enable(void);

/**
 * @brief Configure FDCAN1 pins as GPIO
 *
 * Reconfigures PH13, PH14 from FDCAN1 alternate function
 * to general-purpose I/O mode.
 *
 * @param mode GPIO mode configuration
 * @return 0 on success, negative errno on failure
 */
int healthylink_pinmux_fdcan1_to_gpio(enum healthylink_pin_mode mode);

/**
 * @brief Set FDCAN1 GPIO pin values (when in GPIO output mode)
 *
 * @param tx_val Value for PH13 (0 or 1)
 * @param rx_val Value for PH14 (0 or 1)
 */
void healthylink_pinmux_fdcan1_gpio_set(uint8_t tx_val, uint8_t rx_val);

/**
 * @brief Read FDCAN1 GPIO pin values (when in GPIO input mode)
 *
 * @param tx_val Pointer to store PH13 value (can be NULL)
 * @param rx_val Pointer to store PH14 value (can be NULL)
 */
void healthylink_pinmux_fdcan1_gpio_get(uint8_t *tx_val, uint8_t *rx_val);

/**
 * @brief Configure pins for a specific module type
 *
 * High-level function that configures all HealthyLink pins
 * appropriate for the detected module type.
 *
 * @param module_id Module ID from EEPROM
 * @return 0 on success, negative errno on failure
 */
int healthylink_pinmux_configure_for_module(uint16_t module_id);

/**
 * @brief Reset all HealthyLink pins to safe default state
 *
 * Configures all expansion pins as high-impedance inputs
 * to prevent contention when no module is connected.
 *
 * @return 0 on success, negative errno on failure
 */
int healthylink_pinmux_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* DRIVERS_MISC_HEALTHYLINK_HEALTHYLINK_PINMUX_H_ */
