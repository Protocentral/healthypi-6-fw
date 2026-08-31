/*
 * HealthyLink Expansion Port Public API
 * Copyright (c) 2025 ProtoCentral
 * SPDX-License-Identifier: MIT
 *
 * Public API for HealthyLink expansion modules: detection, module
 * information, and module-specific functionality.
 */

#ifndef INCLUDE_HEALTHYLINK_HEALTHYLINK_H_
#define INCLUDE_HEALTHYLINK_HEALTHYLINK_H_

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup healthylink HealthyLink Expansion Port
 * @{
 */

/*
 * EEPROM Magic and Constants
 */
#define HEALTHYLINK_EEPROM_MAGIC        "HLNK"
#define HEALTHYLINK_EEPROM_MAGIC_U32    0x484C4E4B  /* "HLNK" little-endian */
#define HEALTHYLINK_HEADER_VERSION      0x01
#define HEALTHYLINK_HEADER_SIZE         0x60
#define HEALTHYLINK_EEPROM_SIZE         256

/*
 * EEPROM Address Map
 */
#define HEALTHYLINK_ADDR_MAGIC          0x00  /* 4 bytes */
#define HEALTHYLINK_ADDR_VERSION        0x04  /* 1 byte */
#define HEALTHYLINK_ADDR_HEADER_LEN     0x05  /* 1 byte */
#define HEALTHYLINK_ADDR_MODULE_ID      0x06  /* 2 bytes */
#define HEALTHYLINK_ADDR_HW_REV         0x08  /* 2 bytes */
#define HEALTHYLINK_ADDR_FW_COMPAT      0x0A  /* 2 bytes */
#define HEALTHYLINK_ADDR_CAPABILITIES   0x0C  /* 4 bytes */
#define HEALTHYLINK_ADDR_NAME           0x10  /* 32 bytes */
#define HEALTHYLINK_ADDR_MANUFACTURER   0x30  /* 32 bytes */
#define HEALTHYLINK_ADDR_SERIAL         0x50  /* 4 bytes */
#define HEALTHYLINK_ADDR_RESERVED       0x54  /* 12 bytes */
#define HEALTHYLINK_ADDR_CONFIG         0x60  /* 32 bytes */
#define HEALTHYLINK_ADDR_EXTENDED       0x80  /* 126 bytes */
#define HEALTHYLINK_ADDR_CRC            0xFE  /* 2 bytes */

/*
 * Registered Module IDs
 */
#define HEALTHYLINK_MODULE_ID_INVALID   0x0000
#define HEALTHYLINK_MODULE_ID_EEG_8CH   0x0001
#define HEALTHYLINK_MODULE_ID_EMG_4CH   0x0002
#define HEALTHYLINK_MODULE_ID_TRIGGER   0x0003
#define HEALTHYLINK_MODULE_ID_CAN       0x0004
#define HEALTHYLINK_MODULE_ID_COMPUTE  0x0005
#define HEALTHYLINK_MODULE_ID_HIRES_ADC 0x0006
#define HEALTHYLINK_MODULE_ID_STIM      0x0007
#define HEALTHYLINK_MODULE_ID_SYNC      0x0008
#define HEALTHYLINK_MODULE_ID_GSR_RESP  0x0009
/* 0x000A - 0x00FF: Reserved for ProtoCentral */
/* 0x0100 - 0xFFFE: Community/third-party modules */
#define HEALTHYLINK_MODULE_ID_RESERVED  0xFFFF

/*
 * Capability Bits
 */

/* Bits 0-7: Required interfaces */
#define HEALTHYLINK_CAP_REQUIRES_SPI4   BIT(0)
#define HEALTHYLINK_CAP_REQUIRES_SPI6   BIT(1)
#define HEALTHYLINK_CAP_REQUIRES_USART2 BIT(2)
#define HEALTHYLINK_CAP_REQUIRES_FDCAN  BIT(3)
#define HEALTHYLINK_CAP_REQUIRES_I2C1   BIT(4)
#define HEALTHYLINK_CAP_REQUIRES_ADC    BIT(5)
#define HEALTHYLINK_CAP_REQUIRES_GPIO   BIT(6)
#define HEALTHYLINK_CAP_REQUIRES_PWM    BIT(7)

/* Bits 8-12: Module features */
#define HEALTHYLINK_CAP_HAS_EXT_CLK     BIT(8)
#define HEALTHYLINK_CAP_HAS_PWR_SWITCH  BIT(9)
#define HEALTHYLINK_CAP_HOT_PLUGGABLE   BIT(10)
#define HEALTHYLINK_CAP_DMA_CAPABLE     BIT(11)
#define HEALTHYLINK_CAP_REALTIME_STREAM BIT(12)

/* Bits 13-15: StackLink inter-module bus capabilities */
#define HEALTHYLINK_CAP_STACKLINK_MASTER BIT(13)  /* Can be StackLink clock master */
#define HEALTHYLINK_CAP_STACKLINK_SLAVE  BIT(14)  /* Can be StackLink slave */
#define HEALTHYLINK_CAP_STACKLINK_BRIDGE BIT(15)  /* Can bridge StackLink to host */

/* Bits 16-17: Power requirements */
#define HEALTHYLINK_CAP_POWER_MASK      (0x3 << 16)
#define HEALTHYLINK_CAP_POWER_LOW       (0 << 16)   /* <50mA */
#define HEALTHYLINK_CAP_POWER_MED       (1 << 16)   /* 50-200mA */
#define HEALTHYLINK_CAP_POWER_HIGH      (2 << 16)   /* >200mA */

/*
 * StackLink Inter-Module Bus Definitions
 *
 * StackLink uses reserved pins 38-61 in the 62-pin connector for direct
 * module-to-module communication, bypassing the main MCU for high-speed
 * data transfer between stacked modules.
 *
 * Pin Assignment:
 *   38: SL_CLK   - Bus clock (driven by master)
 *   39: SL_MOSI  - Data downstream (pos N → pos N+1)
 *   40: SL_MISO  - Data upstream (pos N+1 → pos N)
 *   41: SL_SYNC  - Synchronization pulse (broadcast)
 *   42-43: SL_IRQ[0:1] - Interrupt request lines (open-drain)
 *   44-47: SL_CS[0:3]  - Chip select per stack position
 *   48-53: SL_DATA[0:5] - 6-bit parallel data bus (optional)
 *   54: SL_ACK   - Acknowledgment (open-drain)
 *   55: SL_RST   - Stack-wide reset
 *   60-61: GND   - Signal ground
 */

/* StackLink protocol types */
#define STACKLINK_PROTOCOL_SPI      0x00  /* SPI-based (default, up to 20 MHz) */
#define STACKLINK_PROTOCOL_I2S      0x01  /* I2S streaming (for audio/biosignal) */
#define STACKLINK_PROTOCOL_PARALLEL 0x02  /* 6-bit parallel (up to 120 Mbps) */

/* Maximum modules in a stack */
#define HEALTHYLINK_MAX_STACK_DEPTH 4

/* StackLink I2C EEPROM addresses for stacked modules */
#define HEALTHYLINK_EEPROM_ADDR_BASE    0x50
#define HEALTHYLINK_EEPROM_ADDR(pos)    (HEALTHYLINK_EEPROM_ADDR_BASE + (pos))

/**
 * @brief HealthyLink module header structure
 *
 * This structure represents the parsed contents of a HealthyLink
 * module's identification EEPROM.
 */
struct healthylink_header {
	/** Header version (currently 0x01) */
	uint8_t version;

	/** Header length in bytes */
	uint8_t header_len;

	/** Module ID (see HEALTHYLINK_MODULE_ID_* defines) */
	uint16_t module_id;

	/** Hardware revision major version */
	uint8_t hw_rev_major;

	/** Hardware revision minor version */
	uint8_t hw_rev_minor;

	/** Minimum firmware version required */
	uint16_t fw_compat_min;

	/** Capabilities bitmap (see HEALTHYLINK_CAP_* defines) */
	uint32_t capabilities;

	/** Module name (null-terminated, max 31 chars) */
	char name[32];

	/** Manufacturer name (null-terminated, max 31 chars) */
	char manufacturer[32];

	/** Serial number */
	uint32_t serial;

	/** Module-specific configuration (32 bytes from EEPROM 0x60-0x7F) */
	uint8_t config[32];
};

/**
 * @brief HealthyLink module status
 */
enum healthylink_status {
	/** No module detected */
	HEALTHYLINK_STATUS_NOT_PRESENT = 0,

	/** Module detected but not initialized */
	HEALTHYLINK_STATUS_DETECTED,

	/** Module initialized and ready */
	HEALTHYLINK_STATUS_READY,

	/** Module detected but driver not available */
	HEALTHYLINK_STATUS_NO_DRIVER,

	/** Module initialization failed */
	HEALTHYLINK_STATUS_ERROR,
};

/**
 * @brief Detect connected HealthyLink module
 *
 * Scans I2C3 for the module identification EEPROM and reads the
 * module header. If a valid module is found, attempts to load
 * the appropriate driver.
 *
 * @param dev HealthyLink controller device
 * @return 0 on success (module found and initialized),
 *         -ENODEV if no module present,
 *         -EINVAL if EEPROM invalid,
 *         -ENOTSUP if no driver for module,
 *         other negative errno on failure
 */
int healthylink_detect(const struct device *dev);

/**
 * @brief Get connected module information
 *
 * @param dev HealthyLink controller device
 * @param header Pointer to structure to fill with module info
 * @return 0 on success, -ENODEV if no module present
 */
int healthylink_get_module_info(const struct device *dev,
				struct healthylink_header *header);

/**
 * @brief Get module status
 *
 * @param dev HealthyLink controller device
 * @return Current module status
 */
enum healthylink_status healthylink_get_status(const struct device *dev);

/**
 * @brief Get module ID of connected module
 *
 * @param dev HealthyLink controller device
 * @return Module ID, or 0 if no module present
 */
uint16_t healthylink_get_module_id(const struct device *dev);

/**
 * @brief Check if a module with specific ID is connected
 *
 * @param dev HealthyLink controller device
 * @param module_id Module ID to check for
 * @return true if module with given ID is connected and ready
 */
bool healthylink_is_module_present(const struct device *dev, uint16_t module_id);

/**
 * @brief Read raw bytes from module EEPROM
 *
 * @param dev HealthyLink controller device
 * @param addr EEPROM address (0-255)
 * @param data Buffer to read into
 * @param len Number of bytes to read
 * @return 0 on success, negative errno on failure
 */
int healthylink_eeprom_read(const struct device *dev,
			    uint8_t addr, uint8_t *data, size_t len);

/**
 * @brief Write raw bytes to module EEPROM
 *
 * @warning This can corrupt the module identification if used incorrectly.
 *          Only use for module configuration data in the extended region.
 *
 * @param dev HealthyLink controller device
 * @param addr EEPROM address (0-255)
 * @param data Data to write
 * @param len Number of bytes to write
 * @return 0 on success, negative errno on failure
 */
int healthylink_eeprom_write(const struct device *dev,
			     uint8_t addr, const uint8_t *data, size_t len);

/**
 * @brief Calculate CRC-16 for EEPROM validation
 *
 * CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, non-reflected) over bytes
 * 0x00-0xFD.
 *
 * @param data EEPROM data buffer (at least 254 bytes)
 * @return Calculated CRC-16 value
 */
uint16_t healthylink_calc_crc16(const uint8_t *data);

/**
 * @brief Get module name string
 *
 * @param module_id Module ID
 * @return Human-readable module name, or "Unknown" if not recognized
 */
const char *healthylink_module_name(uint16_t module_id);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* INCLUDE_HEALTHYLINK_HEALTHYLINK_H_ */
