/*
 * HealthyLink Core Driver Internal Header
 * Copyright (c) 2025 ProtoCentral
 * SPDX-License-Identifier: MIT
 *
 * Internal definitions for HealthyLink drivers. Module drivers
 * should include this header, applications should use
 * <healthylink/healthylink.h> instead.
 */

#ifndef DRIVERS_MISC_HEALTHYLINK_HEALTHYLINK_CORE_H_
#define DRIVERS_MISC_HEALTHYLINK_HEALTHYLINK_CORE_H_

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/gpio.h>
#include <healthylink/healthylink.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief HealthyLink module driver registration structure
 *
 * Module drivers register themselves using this structure.
 * The core driver uses this to dispatch to the appropriate
 * module driver when a module is detected.
 */
struct healthylink_module_driver {
	/** Module ID this driver handles */
	uint16_t module_id;

	/** Human-readable driver name */
	const char *name;

	/**
	 * @brief Probe function called when module is detected
	 *
	 * @param parent HealthyLink controller device
	 * @param header Parsed module header from EEPROM
	 * @return 0 on success, negative errno on failure
	 */
	int (*probe)(const struct device *parent,
		     const struct healthylink_header *header);

	/**
	 * @brief Remove function called when module is disconnected
	 *
	 * @param parent HealthyLink controller device
	 */
	void (*remove)(const struct device *parent);

	/**
	 * @brief Optional: Get driver-specific device
	 *
	 * @param parent HealthyLink controller device
	 * @return Pointer to driver-specific device, or NULL
	 */
	const struct device *(*get_device)(const struct device *parent);
};

/**
 * @brief HealthyLink controller configuration (from devicetree)
 */
struct healthylink_config {
	/** I2C device spec for the slot ID EEPROM.
	 *  Detection is EEPROM-based only: an I2C ACK means a module is present,
	 *  and the EEPROM contents identify the module type. No detect GPIO. */
	struct i2c_dt_spec eeprom;

	/** Slot load-switch enable (EN_MOD). Sourced from the slot node's
	 *  power-gpios; module power is applied only after EEPROM identify. */
	struct gpio_dt_spec power_gpio;

	/** Slot load-switch fault (MOD_FLT). Sourced from the slot node's
	 *  fault-gpios; checked after power-up. May be empty ({0}). */
	struct gpio_dt_spec fault_gpio;
};

/**
 * @brief HealthyLink controller runtime data
 */
struct healthylink_data {
	/** Parsed module header */
	struct healthylink_header module_header;

	/** Current module status */
	enum healthylink_status status;

	/** Connected module ID (0 if none) */
	uint16_t module_id;

	/** Active module driver (NULL if none) */
	const struct healthylink_module_driver *active_driver;

	/** Mutex for thread-safe access */
	struct k_mutex lock;
};

/**
 * @brief Register a module driver with the core
 *
 * Called at system initialization by module drivers.
 * Not intended for application use.
 *
 * @param drv Driver registration structure
 * @return 0 on success, negative errno on failure
 */
int healthylink_register_driver(const struct healthylink_module_driver *drv);

/**
 * @brief Find driver for a module ID
 *
 * @param module_id Module ID to find driver for
 * @return Pointer to driver, or NULL if not found
 */
const struct healthylink_module_driver *
healthylink_find_driver(uint16_t module_id);

/**
 * @brief Get controller data from device
 *
 * @param dev HealthyLink controller device
 * @return Pointer to runtime data
 */
static inline struct healthylink_data *
healthylink_get_data(const struct device *dev)
{
	return (struct healthylink_data *)dev->data;
}

/**
 * @brief Get controller config from device
 *
 * @param dev HealthyLink controller device
 * @return Pointer to configuration
 */
static inline const struct healthylink_config *
healthylink_get_config(const struct device *dev)
{
	return (const struct healthylink_config *)dev->config;
}

/*
 * Module driver probe/remove function declarations
 * These are weak symbols that module drivers implement
 */

#if IS_ENABLED(CONFIG_HEALTHYLINK_EEG_8CH)
extern int healthylink_eeg_probe(const struct device *parent,
				 const struct healthylink_header *header);
extern void healthylink_eeg_remove(const struct device *parent);
#endif

#if IS_ENABLED(CONFIG_HEALTHYLINK_CAN_INTERFACE)
extern int healthylink_can_probe(const struct device *parent,
				 const struct healthylink_header *header);
extern void healthylink_can_remove(const struct device *parent);
#endif

#if IS_ENABLED(CONFIG_HEALTHYLINK_TRIGGER_IO)
extern int healthylink_trigger_probe(const struct device *parent,
				     const struct healthylink_header *header);
extern void healthylink_trigger_remove(const struct device *parent);
#endif

#if IS_ENABLED(CONFIG_HEALTHYLINK_COMPUTE)
extern int healthylink_compute_probe(const struct device *parent,
				const struct healthylink_header *header);
extern void healthylink_compute_remove(const struct device *parent);
#endif

#if IS_ENABLED(CONFIG_HEALTHYLINK_LLEXT_FALLBACK)
extern int healthylink_load_llext(const struct device *dev, uint16_t module_id);
extern int healthylink_unload_llext(const struct device *dev);
#endif

#ifdef __cplusplus
}
#endif

#endif /* DRIVERS_MISC_HEALTHYLINK_HEALTHYLINK_CORE_H_ */
