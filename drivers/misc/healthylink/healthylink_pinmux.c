/*
 * HealthyLink Runtime Pin Multiplexer
 * Copyright (c) 2025 ProtoCentral
 * SPDX-License-Identifier: MIT
 *
 * Runtime pin configuration using direct STM32H757 GPIO register access
 * (MODER/OTYPER/OSPEEDR/PUPDR/AFR via CMSIS), allowing dynamic
 * reconfiguration based on the detected module type.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <soc.h>

#include "healthylink_pinmux.h"
#include <healthylink/healthylink.h>

LOG_MODULE_REGISTER(healthylink_pinmux, CONFIG_HEALTHYLINK_LOG_LEVEL);

/* GPIO Mode register values (MODER) */
#define HLPM_MODE_INPUT      0x00
#define HLPM_MODE_OUTPUT     0x01
#define HLPM_MODE_AF         0x02
#define HLPM_MODE_ANALOG     0x03

/* GPIO Speed register values (OSPEEDR) */
#define HLPM_SPEED_LOW       0x00
#define HLPM_SPEED_MEDIUM    0x01
#define HLPM_SPEED_HIGH      0x02
#define HLPM_SPEED_VERY_HIGH 0x03

/* GPIO Output type register values (OTYPER) */
#define HLPM_OTYPE_PP        0x00  /* Push-pull */
#define HLPM_OTYPE_OD        0x01  /* Open-drain */

/* GPIO Pull-up/Pull-down register values (PUPDR) */
#define HLPM_PUPD_NONE       0x00
#define HLPM_PUPD_UP         0x01
#define HLPM_PUPD_DOWN       0x02

/* Track current pin configuration state */
static enum {
	PINMUX_STATE_RESET,
	PINMUX_STATE_SPI6,
	PINMUX_STATE_SPI6_GPIO,
	PINMUX_STATE_FDCAN1,
	PINMUX_STATE_FDCAN1_GPIO,
} spi6_state = PINMUX_STATE_RESET, fdcan1_state = PINMUX_STATE_RESET;

/*
 * The SPI6 (PG12/13/14) and FDCAN1 (PH13/14) pins are board-wide singletons
 * wired to EVERY HealthyLink slot in parallel, so only one slot may drive them
 * at a time. `pin_owner` is the slot that currently has them, or NULL if they
 * are free.
 *
 * This exists because detection is per-slot: detecting a module in slot B must
 * not reprogram -- or hi-Z -- the pins a module already running in slot A is
 * using. A slot that wants them while another slot holds them is refused; the
 * pins are left exactly as the owner set them.
 *
 * Deliberately unlocked. Every caller reaches this through the HealthyLink
 * service, which serialises bring-up and tear-down of BOTH slots under its own
 * g_op_lock, so two slots never claim or release concurrently. (The only other
 * entry, CONFIG_HEALTHYLINK_AUTO_DETECT, runs in single-threaded device init.)
 * Anything that gives a slot its own detect thread must add a lock here.
 */
static const void *pin_owner;

/**
 * @brief Configure a single pin
 *
 * @param port GPIO port base address
 * @param pin Pin number (0-15)
 * @param mode GPIO mode (input, output, alternate, analog)
 * @param af Alternate function number (0-15), ignored if mode != AF
 * @param speed Output speed
 * @param otype Output type (push-pull or open-drain)
 * @param pupd Pull-up/pull-down configuration
 */
static void configure_pin(GPIO_TypeDef *port, uint32_t pin, uint32_t mode,
			  uint32_t af, uint32_t speed, uint32_t otype, uint32_t pupd)
{
	uint32_t pos = pin;
	uint32_t pos2 = pin * 2;

	/* Configure mode (2 bits per pin) */
	MODIFY_REG(port->MODER, (0x3UL << pos2), (mode << pos2));

	if (mode == HLPM_MODE_OUTPUT || mode == HLPM_MODE_AF) {
		/* Configure output type (1 bit per pin) */
		MODIFY_REG(port->OTYPER, (0x1UL << pos), (otype << pos));

		/* Configure output speed (2 bits per pin) */
		MODIFY_REG(port->OSPEEDR, (0x3UL << pos2), (speed << pos2));
	}

	/* Configure pull-up/pull-down (2 bits per pin) */
	MODIFY_REG(port->PUPDR, (0x3UL << pos2), (pupd << pos2));

	/* Configure alternate function if in AF mode */
	if (mode == HLPM_MODE_AF) {
		if (pin < 8) {
			/* AFR[0] for pins 0-7 */
			uint32_t afpos = pin * 4;
			MODIFY_REG(port->AFR[0], (0xFUL << afpos), (af << afpos));
		} else {
			/* AFR[1] for pins 8-15 */
			uint32_t afpos = (pin - 8) * 4;
			MODIFY_REG(port->AFR[1], (0xFUL << afpos), (af << afpos));
		}
	}
}

/**
 * @brief Enable GPIO port clock
 */
static void enable_gpio_clock(GPIO_TypeDef *port)
{
	if (port == GPIOG) {
		SET_BIT(RCC->AHB4ENR, RCC_AHB4ENR_GPIOGEN);
		/* Delay after clock enable for stabilization */
		__DSB();
	} else if (port == GPIOH) {
		SET_BIT(RCC->AHB4ENR, RCC_AHB4ENR_GPIOHEN);
		__DSB();
	}
}

/* SPI6 Functions */

int healthylink_pinmux_spi6_enable(void)
{
	LOG_INF("Configuring SPI6 pins (PG12/13/14) as SPI alternate function (AF5)");

	enable_gpio_clock(GPIOG);

	/* PG12 = MISO, PG13 = SCK, PG14 = MOSI - all AF5 */
	configure_pin(GPIOG, 12, HLPM_MODE_AF, 5, HLPM_SPEED_VERY_HIGH,
		      HLPM_OTYPE_PP, HLPM_PUPD_NONE);
	configure_pin(GPIOG, 13, HLPM_MODE_AF, 5, HLPM_SPEED_VERY_HIGH,
		      HLPM_OTYPE_PP, HLPM_PUPD_NONE);
	configure_pin(GPIOG, 14, HLPM_MODE_AF, 5, HLPM_SPEED_VERY_HIGH,
		      HLPM_OTYPE_PP, HLPM_PUPD_NONE);

	spi6_state = PINMUX_STATE_SPI6;
	return 0;
}

int healthylink_pinmux_spi6_to_gpio(enum healthylink_pin_mode mode)
{
	uint32_t gpio_mode;
	uint32_t pupd;

	LOG_DBG("Configuring SPI6 pins (PG12/13/14) as GPIO (mode=%d)", mode);

	enable_gpio_clock(GPIOG);

	switch (mode) {
	case HEALTHYLINK_PIN_MODE_HIZ:
		gpio_mode = HLPM_MODE_INPUT;
		pupd = HLPM_PUPD_NONE;
		break;
	case HEALTHYLINK_PIN_MODE_GPIO_OUTPUT:
		gpio_mode = HLPM_MODE_OUTPUT;
		pupd = HLPM_PUPD_NONE;
		break;
	case HEALTHYLINK_PIN_MODE_GPIO_INPUT:
		gpio_mode = HLPM_MODE_INPUT;
		pupd = HLPM_PUPD_NONE;
		break;
	case HEALTHYLINK_PIN_MODE_GPIO_INPUT_PULLUP:
		gpio_mode = HLPM_MODE_INPUT;
		pupd = HLPM_PUPD_UP;
		break;
	case HEALTHYLINK_PIN_MODE_GPIO_INPUT_PULLDOWN:
		gpio_mode = HLPM_MODE_INPUT;
		pupd = HLPM_PUPD_DOWN;
		break;
	default:
		return -EINVAL;
	}

	configure_pin(GPIOG, 12, gpio_mode, 0, HLPM_SPEED_LOW, HLPM_OTYPE_PP, pupd);
	configure_pin(GPIOG, 13, gpio_mode, 0, HLPM_SPEED_LOW, HLPM_OTYPE_PP, pupd);
	configure_pin(GPIOG, 14, gpio_mode, 0, HLPM_SPEED_LOW, HLPM_OTYPE_PP, pupd);

	spi6_state = PINMUX_STATE_SPI6_GPIO;
	return 0;
}

void healthylink_pinmux_spi6_gpio_set(uint8_t sck_val, uint8_t miso_val, uint8_t mosi_val)
{
	/* Use BSRR for atomic set/reset */
	if (sck_val) {
		GPIOG->BSRR = (1UL << 13);  /* Set PG13 */
	} else {
		GPIOG->BSRR = (1UL << (13 + 16));  /* Reset PG13 */
	}

	if (miso_val) {
		GPIOG->BSRR = (1UL << 12);  /* Set PG12 */
	} else {
		GPIOG->BSRR = (1UL << (12 + 16));  /* Reset PG12 */
	}

	if (mosi_val) {
		GPIOG->BSRR = (1UL << 14);  /* Set PG14 */
	} else {
		GPIOG->BSRR = (1UL << (14 + 16));  /* Reset PG14 */
	}
}

void healthylink_pinmux_spi6_gpio_get(uint8_t *sck_val, uint8_t *miso_val, uint8_t *mosi_val)
{
	uint32_t idr = GPIOG->IDR;

	if (sck_val != NULL) {
		*sck_val = (idr & (1UL << 13)) ? 1 : 0;
	}

	if (miso_val != NULL) {
		*miso_val = (idr & (1UL << 12)) ? 1 : 0;
	}

	if (mosi_val != NULL) {
		*mosi_val = (idr & (1UL << 14)) ? 1 : 0;
	}
}

/* FDCAN1 Functions */

int healthylink_pinmux_fdcan1_enable(void)
{
	LOG_INF("Configuring FDCAN1 pins (PH13=TX, PH14=RX) as CAN alternate function (AF9)");

	enable_gpio_clock(GPIOH);

	/* PH13 = TX (AF9) - push-pull, no pull */
	configure_pin(GPIOH, 13, HLPM_MODE_AF, 9, HLPM_SPEED_VERY_HIGH,
		      HLPM_OTYPE_PP, HLPM_PUPD_NONE);

	/* PH14 = RX (AF9) - input with pull-up for idle high */
	configure_pin(GPIOH, 14, HLPM_MODE_AF, 9, HLPM_SPEED_VERY_HIGH,
		      HLPM_OTYPE_PP, HLPM_PUPD_UP);

	fdcan1_state = PINMUX_STATE_FDCAN1;
	return 0;
}

int healthylink_pinmux_fdcan1_to_gpio(enum healthylink_pin_mode mode)
{
	uint32_t gpio_mode;
	uint32_t pupd;

	LOG_DBG("Configuring FDCAN1 pins (PH13/14) as GPIO (mode=%d)", mode);

	enable_gpio_clock(GPIOH);

	switch (mode) {
	case HEALTHYLINK_PIN_MODE_HIZ:
		gpio_mode = HLPM_MODE_INPUT;
		pupd = HLPM_PUPD_NONE;
		break;
	case HEALTHYLINK_PIN_MODE_GPIO_OUTPUT:
		gpio_mode = HLPM_MODE_OUTPUT;
		pupd = HLPM_PUPD_NONE;
		break;
	case HEALTHYLINK_PIN_MODE_GPIO_INPUT:
		gpio_mode = HLPM_MODE_INPUT;
		pupd = HLPM_PUPD_NONE;
		break;
	case HEALTHYLINK_PIN_MODE_GPIO_INPUT_PULLUP:
		gpio_mode = HLPM_MODE_INPUT;
		pupd = HLPM_PUPD_UP;
		break;
	case HEALTHYLINK_PIN_MODE_GPIO_INPUT_PULLDOWN:
		gpio_mode = HLPM_MODE_INPUT;
		pupd = HLPM_PUPD_DOWN;
		break;
	default:
		return -EINVAL;
	}

	configure_pin(GPIOH, 13, gpio_mode, 0, HLPM_SPEED_LOW, HLPM_OTYPE_PP, pupd);
	configure_pin(GPIOH, 14, gpio_mode, 0, HLPM_SPEED_LOW, HLPM_OTYPE_PP, pupd);

	fdcan1_state = PINMUX_STATE_FDCAN1_GPIO;
	return 0;
}

void healthylink_pinmux_fdcan1_gpio_set(uint8_t tx_val, uint8_t rx_val)
{
	if (tx_val) {
		GPIOH->BSRR = (1UL << 13);  /* Set PH13 */
	} else {
		GPIOH->BSRR = (1UL << (13 + 16));  /* Reset PH13 */
	}

	if (rx_val) {
		GPIOH->BSRR = (1UL << 14);  /* Set PH14 */
	} else {
		GPIOH->BSRR = (1UL << (14 + 16));  /* Reset PH14 */
	}
}

void healthylink_pinmux_fdcan1_gpio_get(uint8_t *tx_val, uint8_t *rx_val)
{
	uint32_t idr = GPIOH->IDR;

	if (tx_val != NULL) {
		*tx_val = (idr & (1UL << 13)) ? 1 : 0;
	}

	if (rx_val != NULL) {
		*rx_val = (idr & (1UL << 14)) ? 1 : 0;
	}
}

/* High-level Configuration Functions */

int healthylink_pinmux_configure_for_module(const void *owner, uint16_t module_id)
{
	int ret = 0;

	LOG_INF("Configuring pins for module ID 0x%04X", module_id);

	/*
	 * Modules that need none of the shared pins never claim them -- they
	 * just drop any claim this same slot was holding from a previous
	 * module, which leaves another slot's claim untouched. Compute and the
	 * GPIO breakout are the two such modules today; an unrecognised module
	 * is handled the same way in `default:`.
	 *
	 * GPIO is a passive breakout: it brings every interface out to headers
	 * and drives nothing itself. Programming the shared pins on its behalf
	 * would be guesswork, and it would take them away from the other slot,
	 * so it gets the devicetree's pin state and nothing more.
	 */
	if (module_id == HEALTHYLINK_MODULE_ID_COMPUTE ||
	    module_id == HEALTHYLINK_MODULE_ID_GPIO) {
		/*
		 * HealthyLink Compute (NPU): uses SPI4 only. SPI4 pins
		 * (PE2/4/5/6) are already configured by the &spi4 pinctrl-0 DT
		 * entry, so this case MUST stay a no-op: doing SPI6 RCC +
		 * GPIOG/GPIOH AF writes here wedges the very next SPI4
		 * transceive.
		 */
		LOG_INF("%s: no extra pinmux needed (the devicetree's pin state "
			"is what this module wants)",
			healthylink_module_name(module_id));
		healthylink_pinmux_release(owner);
		return 0;
	}

	if (pin_owner != NULL && pin_owner != owner) {
		LOG_ERR("Module 0x%04X needs the shared SPI6/FDCAN1 pins, but "
			"another slot's module already has them -- leaving them "
			"alone", module_id);
		return -EBUSY;
	}

	switch (module_id) {
	case HEALTHYLINK_MODULE_ID_EEG_8CH:
		/* EEG-8CH: Uses SPI6 for ADS1299, FDCAN pins as GPIO */
		LOG_INF("EEG-8CH: Enabling SPI6, FDCAN1 pins as hi-z");
		ret = healthylink_pinmux_spi6_enable();
		if (ret == 0) {
			ret = healthylink_pinmux_fdcan1_to_gpio(HEALTHYLINK_PIN_MODE_HIZ);
		}
		break;

	case HEALTHYLINK_MODULE_ID_CAN:
		/* CAN Interface: Uses FDCAN1, SPI6 pins as GPIO */
		LOG_INF("CAN: Enabling FDCAN1, SPI6 pins as hi-z");
		ret = healthylink_pinmux_fdcan1_enable();
		if (ret == 0) {
			ret = healthylink_pinmux_spi6_to_gpio(HEALTHYLINK_PIN_MODE_HIZ);
		}
		break;

	case HEALTHYLINK_MODULE_ID_TRIGGER:
		/* Trigger I/O: Uses all pins as GPIO */
		LOG_INF("TRIGGER: Configuring SPI6 and FDCAN1 pins as GPIO");
		ret = healthylink_pinmux_spi6_to_gpio(HEALTHYLINK_PIN_MODE_GPIO_OUTPUT);
		if (ret == 0) {
			ret = healthylink_pinmux_fdcan1_to_gpio(HEALTHYLINK_PIN_MODE_GPIO_OUTPUT);
		}
		break;

	case HEALTHYLINK_MODULE_ID_EMG_4CH:
		/* EMG-4CH: Uses SPI6 for ADS1294, FDCAN pins as GPIO */
		LOG_INF("EMG-4CH: Enabling SPI6, FDCAN1 pins as hi-z");
		ret = healthylink_pinmux_spi6_enable();
		if (ret == 0) {
			ret = healthylink_pinmux_fdcan1_to_gpio(HEALTHYLINK_PIN_MODE_HIZ);
		}
		break;

	case HEALTHYLINK_MODULE_ID_HIRES_ADC:
		/* High-res ADC: Uses SPI6, FDCAN pins as GPIO */
		LOG_INF("HIRES-ADC: Enabling SPI6, FDCAN1 pins as hi-z");
		ret = healthylink_pinmux_spi6_enable();
		if (ret == 0) {
			ret = healthylink_pinmux_fdcan1_to_gpio(HEALTHYLINK_PIN_MODE_HIZ);
		}
		break;

	case HEALTHYLINK_MODULE_ID_SYNC:
		/* Sync Master: Uses all pins as GPIO for timing signals */
		LOG_INF("SYNC: Configuring all pins as GPIO");
		ret = healthylink_pinmux_spi6_to_gpio(HEALTHYLINK_PIN_MODE_GPIO_OUTPUT);
		if (ret == 0) {
			ret = healthylink_pinmux_fdcan1_to_gpio(HEALTHYLINK_PIN_MODE_GPIO_OUTPUT);
		}
		break;

	case HEALTHYLINK_MODULE_ID_GSR_RESP:
		/* GSR/Respiration: May use SPI or GPIO depending on sensor */
		LOG_INF("GSR-RESP: Enabling SPI6, FDCAN1 pins as hi-z");
		ret = healthylink_pinmux_spi6_enable();
		if (ret == 0) {
			ret = healthylink_pinmux_fdcan1_to_gpio(HEALTHYLINK_PIN_MODE_HIZ);
		}
		break;

	default:
		/*
		 * Unknown module: we do not know what it wants, so give it
		 * nothing. Hand the pins back (a no-op unless this slot held
		 * them) rather than programming them blind.
		 */
		LOG_WRN("Unknown module 0x%04X, leaving the shared pins unclaimed",
			module_id);
		healthylink_pinmux_release(owner);
		return 0;
	}

	if (ret == 0) {
		pin_owner = owner;
	}

	return ret;
}

void healthylink_pinmux_release(const void *owner)
{
	if (pin_owner == NULL || pin_owner != owner) {
		/* Never ours to give back: either already free, or another
		 * slot's module is still driving these pins. */
		return;
	}

	pin_owner = NULL;
	(void)healthylink_pinmux_reset();
}

int healthylink_pinmux_reset(void)
{
	int ret;

	LOG_DBG("Resetting HealthyLink pins to safe hi-z state");

	/* Configure all pins as high-impedance inputs */
	ret = healthylink_pinmux_spi6_to_gpio(HEALTHYLINK_PIN_MODE_HIZ);
	if (ret != 0) {
		LOG_ERR("Failed to reset SPI6 pins: %d", ret);
		return ret;
	}

	ret = healthylink_pinmux_fdcan1_to_gpio(HEALTHYLINK_PIN_MODE_HIZ);
	if (ret != 0) {
		LOG_ERR("Failed to reset FDCAN1 pins: %d", ret);
		return ret;
	}

	spi6_state = PINMUX_STATE_RESET;
	fdcan1_state = PINMUX_STATE_RESET;

	return 0;
}
