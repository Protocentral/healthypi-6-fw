/*
 * HealthyLink Core Driver
 * Copyright (c) 2025 ProtoCentral
 * SPDX-License-Identifier: MIT
 *
 * Core driver for HealthyLink expansion port. Handles:
 * - Module detection via I2C EEPROM
 * - Header parsing and validation
 * - EEPROM auto-provisioning for blank modules
 * - Driver dispatch based on module ID
 * - Slot power (load switch + fault line)
 *
 * One device per "protocentral,healthylink-slot" node that has an `eeprom`.
 */

#define DT_DRV_COMPAT protocentral_healthylink_slot

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <soc.h>
#include <string.h>

#include <healthylink/healthylink.h>
#include <healthylink/healthylink_eeprom.h>
#include "healthylink_core.h"
#include "healthylink_pinmux.h"

LOG_MODULE_REGISTER(healthylink, CONFIG_HEALTHYLINK_LOG_LEVEL);

/*
 * Auto-provisioning module identity from device tree: the chosen node
 * "healthylink-module" points to the module definition node, and the module
 * ID is derived from its compatible string (a property would require a
 * binding definition).
 */
#define HEALTHYLINK_MODULE_NODE DT_CHOSEN(healthylink_module)

#if DT_NODE_EXISTS(HEALTHYLINK_MODULE_NODE)
#define HAS_DT_MODULE_INFO 1
/* Determine module ID from compatible string */
#if DT_NODE_HAS_COMPAT(HEALTHYLINK_MODULE_NODE, protocentral_healthylink_eeg_8ch)
#define DT_MODULE_ID HEALTHYLINK_MODULE_ID_EEG_8CH
#elif DT_NODE_HAS_COMPAT(HEALTHYLINK_MODULE_NODE, protocentral_healthylink_can)
#define DT_MODULE_ID HEALTHYLINK_MODULE_ID_CAN
#elif DT_NODE_HAS_COMPAT(HEALTHYLINK_MODULE_NODE, protocentral_healthylink_trigger)
#define DT_MODULE_ID HEALTHYLINK_MODULE_ID_TRIGGER
#elif DT_NODE_HAS_COMPAT(HEALTHYLINK_MODULE_NODE, protocentral_healthylink_compute)
#define DT_MODULE_ID HEALTHYLINK_MODULE_ID_COMPUTE
#else
#define DT_MODULE_ID 0
#endif
#else
#define HAS_DT_MODULE_INFO 0
#define DT_MODULE_ID       0
#endif

/*
 * Static driver registration table
 * Module drivers are registered here at compile time
 */
static const struct healthylink_module_driver registered_drivers[] = {
#if IS_ENABLED(CONFIG_HEALTHYLINK_EEG_8CH)
	{
		.module_id = HEALTHYLINK_MODULE_ID_EEG_8CH,
		.name = "EEG-8CH",
		.probe = healthylink_eeg_probe,
		.remove = healthylink_eeg_remove,
	},
#endif
#if IS_ENABLED(CONFIG_HEALTHYLINK_CAN_INTERFACE)
	{
		.module_id = HEALTHYLINK_MODULE_ID_CAN,
		.name = "CAN-INTERFACE",
		.probe = healthylink_can_probe,
		.remove = healthylink_can_remove,
	},
#endif
#if IS_ENABLED(CONFIG_HEALTHYLINK_TRIGGER_IO)
	{
		.module_id = HEALTHYLINK_MODULE_ID_TRIGGER,
		.name = "TRIGGER-IO",
		.probe = healthylink_trigger_probe,
		.remove = healthylink_trigger_remove,
	},
#endif
#if IS_ENABLED(CONFIG_HEALTHYLINK_COMPUTE)
	{
		.module_id = HEALTHYLINK_MODULE_ID_COMPUTE,
		.name = "HealthyLink Compute",
		.probe = healthylink_compute_probe,
		.remove = healthylink_compute_remove,
	},
#endif
};

/* Find driver for module ID */
const struct healthylink_module_driver *healthylink_find_driver(uint16_t module_id)
{
	for (size_t i = 0; i < ARRAY_SIZE(registered_drivers); i++) {
		if (registered_drivers[i].module_id == module_id) {
			return &registered_drivers[i];
		}
	}
	return NULL;
}

/* Get module name string */
const char *healthylink_module_name(uint16_t module_id)
{
	switch (module_id) {
	case HEALTHYLINK_MODULE_ID_EEG_8CH:
		return "EEG-8CH";
	case HEALTHYLINK_MODULE_ID_EMG_4CH:
		return "EMG-4CH";
	case HEALTHYLINK_MODULE_ID_TRIGGER:
		return "TRIGGER-IO";
	case HEALTHYLINK_MODULE_ID_CAN:
		return "CAN-INTERFACE";
	case HEALTHYLINK_MODULE_ID_COMPUTE:
		return "HealthyLink Compute";
	case HEALTHYLINK_MODULE_ID_HIRES_ADC:
		return "HIGH-RES-ADC";
	case HEALTHYLINK_MODULE_ID_STIM:
		return "STIM-OUTPUT";
	case HEALTHYLINK_MODULE_ID_SYNC:
		return "SYNC-MASTER";
	case HEALTHYLINK_MODULE_ID_GSR_RESP:
		return "GSR-RESPIRATION";
	case HEALTHYLINK_MODULE_ID_GPIO:
		return "GPIO";
	default:
		return "Unknown";
	}
}

/*
 * CRC-16/CCITT-FALSE over bytes 0x00-0xFD: polynomial 0x1021, init 0xFFFF,
 * NO input or output reflection. Check value for "123456789" is 0x29B1.
 *
 * This is the algorithm the EEPROM layout documents (see
 * include/healthylink/healthylink_eeprom.h) and the one the host tooling
 * (`healthypi hl eeprom generate`) writes -- both ends must agree.
 *
 * Do NOT substitute Zephyr's crc16_ccitt(): despite the name it is the
 * REFLECTED variant (CRC-16/KERMIT family, poly 0x8408) and disagrees on
 * every input. This driver used it before 2026-08-03; see the legacy
 * fallback below.
 */
uint16_t healthylink_calc_crc16(const uint8_t *data)
{
	return crc16(0x1021, 0xFFFF, data, 254);
}

/*
 * What healthylink_calc_crc16() computed before 2026-08-03.
 *
 * Retained ONLY so a module auto-provisioned by the older driver still
 * validates instead of being rejected on a firmware update. Accepted with a
 * warning; never written. Delete once no such module remains in circulation.
 */
static uint16_t healthylink_calc_crc16_legacy(const uint8_t *data)
{
	return crc16_ccitt(0xFFFF, data, 254);
}

#if IS_ENABLED(CONFIG_HEALTHYLINK_AUTO_PROVISION)

/**
 * @brief Generate a serial number from MCU unique ID
 *
 * Creates a deterministic serial number based on the STM32 unique ID
 * and module ID, ensuring each module gets a unique serial.
 */
static uint32_t healthylink_generate_serial(uint16_t module_id)
{
	/* STM32H7 unique ID is at 0x1FF1E800 (96 bits = 12 bytes) */
	const uint32_t *uid = (const uint32_t *)0x1FF1E800;

	/* Combine UID words with module_id to create unique serial */
	uint32_t serial = uid[0] ^ uid[1] ^ uid[2];
	serial = (serial & 0xFFFF0000) | ((serial ^ module_id) & 0x0000FFFF);

	return serial;
}

/**
 * @brief Check if EEPROM is blank (unprogrammed)
 *
 * A blank EEPROM will have all 0xFF (unprogrammed flash/EEPROM) or
 * all 0x00 (zeroed). We check the magic bytes location.
 *
 * @return true if EEPROM appears blank, false otherwise
 */
static bool healthylink_eeprom_is_blank(const struct device *dev)
{
	uint8_t magic[4];
	int ret;

	ret = healthylink_eeprom_read(dev, HEALTHYLINK_ADDR_MAGIC, magic, 4);
	if (ret < 0) {
		LOG_ERR("EEPROM I2C read failed (chip not responding @ 0x%02x?): %d",
			healthylink_get_config(dev)->eeprom.addr, ret);
		return false;
	}

	LOG_INF("EEPROM first 4 bytes: %02X %02X %02X %02X",
		magic[0], magic[1], magic[2], magic[3]);

	/* Check for all 0xFF (unprogrammed) */
	if (magic[0] == 0xFF && magic[1] == 0xFF &&
	    magic[2] == 0xFF && magic[3] == 0xFF) {
		LOG_INF("EEPROM is blank (factory 0xFF) - will auto-provision");
		return true;
	}

	/* Check for all 0x00 (zeroed) */
	if (magic[0] == 0x00 && magic[1] == 0x00 &&
	    magic[2] == 0x00 && magic[3] == 0x00) {
		LOG_INF("EEPROM is blank (all 0x00) - will auto-provision");
		return true;
	}

	/* Check for valid magic - if invalid, consider it "blank" for provisioning */
	if (memcmp(magic, HEALTHYLINK_EEPROM_MAGIC, 4) != 0) {
		LOG_WRN("EEPROM has unexpected magic - treating as blank for "
			"auto-provision");
		return true;
	}

	LOG_INF("EEPROM has valid HLNK magic");
	return false;
}

/**
 * @brief Probe hardware to detect module type
 *
 * Attempts to identify the connected module by probing specific hardware:
 * - EEG-8CH: Read ADS1299 ID register via SPI4
 * - CAN: Check FDCAN peripheral
 * - AI: Probe SPI6 device
 *
 * @return Module ID if detected, 0 if unknown
 */
static uint16_t healthylink_probe_hardware(void)
{
	uint16_t detected_id = 0;

#if IS_ENABLED(CONFIG_HEALTHYLINK_EEG_8CH)
	/* Try to detect ADS1299 on SPI4 */
	const struct device *spi4 = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(spi4));
	if (spi4 != NULL && device_is_ready(spi4)) {
		/* ADS1299 detection would require initializing GPIOs and doing
		 * a proper power-up sequence. For now, if SPI4 is configured
		 * with an ADS1299 child node, assume EEG module is intended.
		 */
#if DT_NODE_EXISTS(DT_NODELABEL(ads1299))
		LOG_DBG("ADS1299 node exists in DT, assuming EEG-8CH");
		detected_id = HEALTHYLINK_MODULE_ID_EEG_8CH;
#endif
	}
#endif

#if IS_ENABLED(CONFIG_HEALTHYLINK_CAN_INTERFACE)
	if (detected_id == 0) {
		/* Try to detect CAN transceiver */
		const struct device *can = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(fdcan1));
		if (can != NULL && device_is_ready(can)) {
			LOG_DBG("FDCAN1 ready, could be CAN-INTERFACE");
			/* Additional transceiver detection could go here */
		}
	}
#endif

	return detected_id;
}

/**
 * @brief Get default capabilities for a module ID
 */
static uint32_t healthylink_default_capabilities(uint16_t module_id)
{
	switch (module_id) {
	case HEALTHYLINK_MODULE_ID_EEG_8CH:
		/* SPI4 + GPIO + realtime streaming + low power */
		return HEALTHYLINK_CAP_REQUIRES_SPI4 | HEALTHYLINK_CAP_REQUIRES_GPIO |
		       HEALTHYLINK_CAP_REALTIME_STREAM | HEALTHYLINK_CAP_POWER_LOW;
	case HEALTHYLINK_MODULE_ID_CAN:
		return HEALTHYLINK_CAP_REQUIRES_FDCAN | HEALTHYLINK_CAP_POWER_MED;
	case HEALTHYLINK_MODULE_ID_TRIGGER:
		return HEALTHYLINK_CAP_REQUIRES_GPIO | HEALTHYLINK_CAP_POWER_LOW;
	case HEALTHYLINK_MODULE_ID_GPIO:
		/* Deliberately claims no interface bit: the breakout exposes
		 * every interface passively, and bits 0-7 are exclusive across
		 * slots -- claiming one would lock a real module out of the
		 * other slot for nothing. */
		return HEALTHYLINK_CAP_POWER_LOW;
	case HEALTHYLINK_MODULE_ID_COMPUTE:
		return HEALTHYLINK_CAP_REQUIRES_SPI6 | HEALTHYLINK_CAP_REQUIRES_GPIO |
		       HEALTHYLINK_CAP_DMA_CAPABLE | HEALTHYLINK_CAP_POWER_HIGH;
	default:
		return 0;
	}
}

/**
 * @brief Write module identity to blank EEPROM
 *
 * Provisions a blank EEPROM with module identity data. Sources:
 * 1. Device tree overlay (if module-id specified)
 * 2. Hardware probing (detect ADS1299, CAN transceiver, etc.)
 *
 * @param dev HealthyLink controller device
 * @return 0 on success, negative errno on failure
 */
static int healthylink_provision_eeprom(const struct device *dev)
{
	struct healthylink_eeprom_image image;
	uint16_t module_id = 0;
	const char *name = "Unknown";
	const char *manufacturer = "ProtoCentral";
	uint8_t hw_major = 1, hw_minor = 0;
	uint16_t fw_min = 0x0110;  /* v1.1.0 */
	uint32_t capabilities = 0;
	int ret;

	/* Try device tree first - module_id is set in the overlay */
#if HAS_DT_MODULE_INFO
	module_id = DT_MODULE_ID;
	if (module_id != 0) {
		/* Derive name from module ID (avoids need for DT string binding) */
		name = healthylink_module_name(module_id);
		capabilities = healthylink_default_capabilities(module_id);
		LOG_INF("Using DT module ID: %s (0x%04X)", name, module_id);
	}
#endif

	/* If no DT info, try hardware probing */
	if (module_id == 0) {
		module_id = healthylink_probe_hardware();
		if (module_id != 0) {
			name = healthylink_module_name(module_id);
			capabilities = healthylink_default_capabilities(module_id);
			LOG_INF("Hardware probe detected: %s (0x%04X)", name, module_id);
		}
	}

	/* If still no ID, we can't provision */
	if (module_id == 0) {
		LOG_WRN("Cannot provision: no module ID from DT or hardware probe");
		return -ENOENT;
	}

	/* Build EEPROM image */
	memset(&image, 0, sizeof(image));

	/* Header */
	memcpy(image.header.magic, HEALTHYLINK_EEPROM_MAGIC, 4);
	image.header.version = HEALTHYLINK_HEADER_VERSION;
	image.header.header_len = HEALTHYLINK_HEADER_SIZE;
	image.header.module_id = sys_cpu_to_le16(module_id);
	image.header.hw_rev_major = hw_major;
	image.header.hw_rev_minor = hw_minor;
	image.header.fw_compat_min = sys_cpu_to_le16(fw_min);
	image.header.capabilities = sys_cpu_to_le32(capabilities);

	/* Copy strings (null-terminated, max 31 chars) */
	strncpy(image.header.name, name, 31);
	image.header.name[31] = '\0';
	strncpy(image.header.manufacturer, manufacturer, 31);
	image.header.manufacturer[31] = '\0';

	/* Generate serial number */
	image.header.serial = sys_cpu_to_le32(healthylink_generate_serial(module_id));

	/* Calculate CRC over bytes 0x00-0xFD */
	uint16_t crc = healthylink_calc_crc16((uint8_t *)&image);
	image.crc.crc16 = sys_cpu_to_le16(crc);

	/* Write to EEPROM */
	LOG_INF("Provisioning EEPROM: %s (ID=0x%04X, Serial=%u)",
		name, module_id, sys_le32_to_cpu(image.header.serial));

	ret = healthylink_eeprom_write(dev, 0x00, (uint8_t *)&image, sizeof(image));
	if (ret < 0) {
		LOG_ERR("Failed to write EEPROM: %d", ret);
		return ret;
	}

	LOG_INF("EEPROM provisioned successfully");
	return 0;
}

#endif /* CONFIG_HEALTHYLINK_AUTO_PROVISION */

/* Read EEPROM bytes */
int healthylink_eeprom_read(const struct device *dev,
			    uint8_t addr, uint8_t *data, size_t len)
{
	const struct healthylink_config *cfg = healthylink_get_config(dev);

	return i2c_burst_read_dt(&cfg->eeprom, addr, data, len);
}

/* Write EEPROM bytes */
int healthylink_eeprom_write(const struct device *dev,
			     uint8_t addr, const uint8_t *data, size_t len)
{
	const struct healthylink_config *cfg = healthylink_get_config(dev);
	int ret;

	/* 24AA02 has 8-byte page size, need to write page by page */
	while (len > 0) {
		/* Calculate bytes to end of current page */
		size_t page_offset = addr % 8;
		size_t page_remaining = 8 - page_offset;
		size_t write_len = MIN(len, page_remaining);

		/* Write data with address byte prepended */
		uint8_t buf[9];  /* Max 8 data bytes + 1 address byte */
		buf[0] = addr;
		memcpy(&buf[1], data, write_len);

		ret = i2c_write_dt(&cfg->eeprom, buf, write_len + 1);
		if (ret < 0) {
			return ret;
		}

		/* Wait for write cycle (5ms max for 24AA02) */
		k_msleep(5);

		addr += write_len;
		data += write_len;
		len -= write_len;
	}

	return 0;
}

/* Probe every address on the bus this slot's ID EEPROM lives on.
 *
 * A bring-up instrument, not part of detection: on v5 the slot EEPROMs are the
 * only devices on I2C3, so a slot that answers nothing gives no way to tell a
 * module that is not responding from a bus that is not working. This says
 * which it is.
 *
 * Zero-length write, the same probe Zephyr's `i2c scan` shell command uses:
 * it ACKs or it does not, and nothing is read or written either way.
 */
int healthylink_bus_scan(const struct device *dev, uint8_t *addrs, size_t max)
{
	const struct healthylink_config *cfg = healthylink_get_config(dev);
	size_t n = 0;

	if (addrs == NULL || max == 0) {
		return -EINVAL;
	}
	if (!device_is_ready(cfg->eeprom.bus)) {
		return -ENODEV;
	}

	for (uint16_t a = 0x08; a <= 0x77 && n < max; a++) {
		struct i2c_msg msg = {
			.buf = NULL,
			.len = 0U,
			.flags = I2C_MSG_WRITE | I2C_MSG_STOP,
		};

		if (i2c_transfer(cfg->eeprom.bus, &msg, 1, (uint16_t)a) == 0) {
			addrs[n++] = (uint8_t)a;
		}
	}

	LOG_INF("slot %s: bus scan found %u device(s)", cfg->label,
		(unsigned int)n);
	return (int)n;
}

/* Check if a module is present.
 *
 * Detection is EEPROM-based only (no detect GPIO): probe the slot's ID EEPROM
 * over I2C - a successful 1-byte read (I2C ACK) means a module is seated. The
 * EEPROM contents are then read to identify the module type.
 */
static bool healthylink_module_present(const struct device *dev)
{
	const struct healthylink_config *cfg = healthylink_get_config(dev);
	uint8_t dummy;

	return i2c_read_dt(&cfg->eeprom, &dummy, 1) == 0;
}

/* Read and parse module header */
static int healthylink_read_header(const struct device *dev,
				   struct healthylink_header *header)
{
	struct healthylink_eeprom_header raw;
	int ret;

	/* Read raw header from EEPROM */
	ret = healthylink_eeprom_read(dev, 0x00, (uint8_t *)&raw, sizeof(raw));
	if (ret < 0) {
		LOG_DBG("EEPROM read failed: %d", ret);
		return ret;
	}

	/* Verify magic. Worth a warning rather than a debug line: with
	 * auto-provisioning off (the default), an unprogrammed module is the
	 * ordinary reason a slot comes up ERROR, and "-22" on its own sends
	 * people looking for a hardware fault that is not there. */
	if (memcmp(raw.magic, HEALTHYLINK_EEPROM_MAGIC, 4) != 0) {
		const uint8_t m0 = raw.magic[0];
		bool unwritten = (m0 == 0xFF || m0 == 0x00) &&
				 raw.magic[1] == m0 && raw.magic[2] == m0 &&
				 raw.magic[3] == m0;

		LOG_WRN("slot %s: no HLNK magic (%02X %02X %02X %02X) -- %s",
			healthylink_get_config(dev)->label,
			raw.magic[0], raw.magic[1], raw.magic[2], raw.magic[3],
			unwritten ? "the ID EEPROM has never been programmed. "
				    "Run: healthypi hl eeprom program --slot <a|b> "
				    "--module-id <id> --name <name>"
				  : "the ID EEPROM holds something else (a partial "
				    "write, or not a HealthyLink module)");
		return -EINVAL;
	}

	/* Verify header version */
	if (raw.version != HEALTHYLINK_HEADER_VERSION) {
		LOG_WRN("Unsupported header version: %d (expected %d)",
			raw.version, HEALTHYLINK_HEADER_VERSION);
		/* Continue anyway - may still be compatible */
	}

	/* Parse header fields */
	header->version = raw.version;
	header->header_len = raw.header_len;
	header->module_id = sys_le16_to_cpu(raw.module_id);
	header->hw_rev_major = raw.hw_rev_major;
	header->hw_rev_minor = raw.hw_rev_minor;
	header->fw_compat_min = sys_le16_to_cpu(raw.fw_compat_min);
	header->capabilities = sys_le32_to_cpu(raw.capabilities);
	memcpy(header->name, raw.name, sizeof(header->name));
	header->name[31] = '\0';  /* Ensure null termination */
	memcpy(header->manufacturer, raw.manufacturer, sizeof(header->manufacturer));
	header->manufacturer[31] = '\0';
	header->serial = sys_le32_to_cpu(raw.serial);

	/* Read module-specific config */
	ret = healthylink_eeprom_read(dev, HEALTHYLINK_ADDR_CONFIG,
				      header->config, sizeof(header->config));
	if (ret < 0) {
		LOG_WRN("Failed to read module config: %d", ret);
		/* Not fatal - continue with defaults */
		memset(header->config, 0, sizeof(header->config));
	}

	/* Verify the CRC over 0x00-0xFD against the stored value at 0xFE. It is
	 * the only integrity check the EEPROM has (no signature, no
	 * authentication), and the arbiter claims interfaces (SPI4/SPI6/USART2/
	 * ...) from the capability word, so a module we cannot identify must not
	 * be wired up: a mismatch fails detection, the caller marks the slot
	 * ERROR, and the module stays unpowered and unclaimed.
	 *
	 * Both values are logged because the usual cause is a programming
	 * mistake: `healthypi hl eeprom read <file>` computes the same
	 * CRC-16-CCITT (poly 0x1021, init 0xFFFF, first 254 bytes).
	 */
	struct healthylink_eeprom_image image;

	ret = healthylink_eeprom_read(dev, 0x00, (uint8_t *)&image, sizeof(image));
	if (ret < 0) {
		LOG_ERR("EEPROM CRC read failed: %d", ret);
		return ret;
	}

	uint16_t stored = sys_le16_to_cpu(image.crc.crc16);
	uint16_t computed = healthylink_calc_crc16((const uint8_t *)&image);

	if (stored != computed) {
		/* Transitional: accept an image written by the pre-2026-08-03
		 * driver, which used the reflected variant. Warn loudly -- it
		 * should be reprovisioned -- but do not reject a module that is
		 * intact and merely stamped with the old algorithm. */
		if (stored == healthylink_calc_crc16_legacy((const uint8_t *)&image)) {
			LOG_WRN("EEPROM carries a LEGACY (reflected) CRC 0x%04X; "
				"expected 0x%04X. The module is intact -- reprovision "
				"it to the documented CRC-16/CCITT-FALSE.",
				stored, computed);
			return 0;
		}
		LOG_ERR("EEPROM CRC mismatch: stored 0x%04X, computed 0x%04X -- "
			"module rejected (identity and capabilities cannot be trusted)",
			stored, computed);
		return -EBADMSG;
	}

	return 0;
}

#if IS_ENABLED(CONFIG_HEALTHYLINK_REPROVISION_NAME)
/* One-shot: rewrite ONLY the name field of an already-provisioned EEPROM to the
 * canonical name for its module ID, preserving every other field. CRC over
 * 0x00-0xFD is recomputed. No-op if the name already matches. */
static int healthylink_reprovision_name(const struct device *dev)
{
	struct healthylink_eeprom_image image;
	int ret = healthylink_eeprom_read(dev, 0x00, (uint8_t *)&image, sizeof(image));
	if (ret < 0) {
		LOG_ERR("Reprovision: EEPROM read failed: %d", ret);
		return ret;
	}

	uint16_t id = sys_le16_to_cpu(image.header.module_id);
	const char *want = healthylink_module_name(id);

	if (strncmp(image.header.name, want, sizeof(image.header.name)) == 0) {
		return 0;   /* already correct */
	}

	LOG_WRN("Reprovision: EEPROM name '%s' -> '%s' (id 0x%04X)",
		image.header.name, want, id);
	memset(image.header.name, 0, sizeof(image.header.name));
	strncpy(image.header.name, want, sizeof(image.header.name) - 1);

	uint16_t crc = healthylink_calc_crc16((uint8_t *)&image);
	image.crc.crc16 = sys_cpu_to_le16(crc);

	ret = healthylink_eeprom_write(dev, 0x00, (uint8_t *)&image, sizeof(image));
	if (ret < 0) {
		LOG_ERR("Reprovision: EEPROM write failed: %d", ret);
		return ret;
	}
	k_msleep(10);   /* EEPROM write-cycle settle */
	LOG_INF("Reprovision: EEPROM name updated to '%s'", want);
	return 0;
}
#endif /* CONFIG_HEALTHYLINK_REPROVISION_NAME */

/* Drive the load switch and record the level. Caller holds the lock. */
static void slot_power_set(const struct device *dev, bool on)
{
	const struct healthylink_config *cfg = healthylink_get_config(dev);
	struct healthylink_data *data = healthylink_get_data(dev);

	if (cfg->power_gpio.port == NULL) {
		return;
	}
	gpio_pin_set_dt(&cfg->power_gpio, on ? 1 : 0);
	data->powered = on;
}

/* Power the slot and switch it back off on a load-switch fault. Caller holds
 * the lock. */
static int slot_power_on_checked(const struct device *dev)
{
	const struct healthylink_config *cfg = healthylink_get_config(dev);

	if (cfg->power_gpio.port != NULL) {
		slot_power_set(dev, true);
		k_msleep(10);  /* power-up settle */
	}
	if (cfg->fault_gpio.port != NULL && gpio_pin_get_dt(&cfg->fault_gpio) > 0) {
		LOG_ERR("slot %s: load-switch fault after power-up -- slot switched off",
			cfg->label);
		slot_power_set(dev, false);
		return -EIO;
	}
	return 0;
}

int healthylink_slot_power(const struct device *dev, bool on)
{
	const struct healthylink_config *cfg = healthylink_get_config(dev);
	struct healthylink_data *data = healthylink_get_data(dev);
	int ret = 0;

	if (cfg->power_gpio.port == NULL) {
		return -ENOTSUP;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	if (on) {
		ret = slot_power_on_checked(dev);
	} else {
		slot_power_set(dev, false);
		/* An unpowered module is not driving the shared pins, so stop
		 * holding them on its behalf. */
		healthylink_pinmux_release(dev);
	}
	k_mutex_unlock(&data->lock);

	LOG_INF("slot %s: power %s%s", cfg->label, on ? "on" : "off",
		ret == 0 ? "" : " refused (load-switch fault)");
	return ret;
}

bool healthylink_slot_is_powered(const struct device *dev)
{
	return healthylink_get_data(dev)->powered;
}

const char *healthylink_slot_label(const struct device *dev)
{
	return healthylink_get_config(dev)->label;
}

/* Detect and initialize module */
int healthylink_detect(const struct device *dev)
{
	struct healthylink_data *data = healthylink_get_data(dev);
	const struct healthylink_config *cfg = healthylink_get_config(dev);
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);

	/* Start from nothing, so a re-run never reports the previous module. */
	if (data->active_driver != NULL && data->active_driver->remove != NULL) {
		data->active_driver->remove(dev);
	}
	/* Whatever this slot held is no longer in use; give it back so the other
	 * slot can take it. A no-op if this slot never claimed anything. */
	healthylink_pinmux_release(dev);
	data->active_driver = NULL;
	data->module_id = 0;
	memset(&data->module_header, 0, sizeof(data->module_header));
	data->status = HEALTHYLINK_STATUS_NOT_PRESENT;

	/* Identify-then-power: probe with the slot off, unless the EEPROM is on
	 * the switched rail and cannot ACK without power. */
	if (cfg->eeprom_switched) {
		slot_power_set(dev, true);
		k_msleep(10);  /* tPWR + EEPROM POR */
	} else {
		slot_power_set(dev, false);
	}

	LOG_INF("slot %s: probing ID EEPROM @ I2C 0x%02x", cfg->label,
		cfg->eeprom.addr);
	bool present = healthylink_module_present(dev);

	if (!present) {
		/* Nothing to undo here: the release above already gave back
		 * anything this slot held, and it is ownership -- not the
		 * absence of this call -- that keeps us off pins the other slot
		 * is using. */
		LOG_INF("slot %s: empty (no ACK at 0x%02x)", cfg->label,
			cfg->eeprom.addr);
		slot_power_set(dev, false);
		k_mutex_unlock(&data->lock);
		return -ENODEV;
	}
	LOG_INF("slot %s: ID EEPROM answered at 0x%02x", cfg->label,
		cfg->eeprom.addr);

#if IS_ENABLED(CONFIG_HEALTHYLINK_AUTO_PROVISION)
	/* Check if EEPROM is blank and needs provisioning */
	if (healthylink_eeprom_is_blank(dev)) {
		LOG_INF("Blank EEPROM detected, attempting auto-provision...");
		ret = healthylink_provision_eeprom(dev);
		if (ret < 0) {
			LOG_WRN("Auto-provision failed: %d", ret);
			/* Continue anyway - maybe manual provisioning later */
		}
		/* Small delay after write for EEPROM to settle */
		k_msleep(10);
	}
#endif

	/* Read module header */
	ret = healthylink_read_header(dev, &data->module_header);
	if (ret < 0) {
		LOG_ERR("slot %s: failed to read module header: %d", cfg->label, ret);
		data->status = HEALTHYLINK_STATUS_ERROR;
		slot_power_set(dev, false);   /* unidentified: never powered */
		k_mutex_unlock(&data->lock);
		return ret;
	}

#if IS_ENABLED(CONFIG_HEALTHYLINK_REPROVISION_NAME)
	/* One-shot name migration on a valid (non-blank) EEPROM, then re-read so
	 * the fields logged below reflect the update. */
	if (healthylink_reprovision_name(dev) == 0) {
		(void)healthylink_read_header(dev, &data->module_header);
	}
#endif

	data->module_id = data->module_header.module_id;
	data->status = HEALTHYLINK_STATUS_DETECTED;

	LOG_INF("HealthyLink module detected in slot %s:", cfg->label);
	LOG_INF("  Name: %s", data->module_header.name);
	LOG_INF("  ID: 0x%04X (%s)", data->module_id,
		healthylink_module_name(data->module_id));
	LOG_INF("  Manufacturer: %s", data->module_header.manufacturer);
	LOG_INF("  HW Rev: %d.%d", data->module_header.hw_rev_major,
		data->module_header.hw_rev_minor);
	LOG_INF("  Serial: %u", data->module_header.serial);
	LOG_INF("  Capabilities: 0x%08X", data->module_header.capabilities);

	/* Identified: power the slot. */
	ret = slot_power_on_checked(dev);
	if (ret < 0) {
		data->status = HEALTHYLINK_STATUS_ERROR;
		k_mutex_unlock(&data->lock);
		return ret;
	}

	/* Configure pins for this module type (runtime pinmux). The slot device
	 * is the owner token: the SPI6/FDCAN1 pins reach both slots, so this
	 * refuses with -EBUSY rather than reprogramming pins the other slot's
	 * module is using. */
	ret = healthylink_pinmux_configure_for_module(dev, data->module_id);
	if (ret < 0) {
		LOG_ERR("slot %s: failed to configure pins for module: %d",
			cfg->label, ret);
		data->status = HEALTHYLINK_STATUS_ERROR;
		k_mutex_unlock(&data->lock);
		return ret;
	}

	/* Find and probe HealthyLink-specific driver (if any) */
	const struct healthylink_module_driver *drv =
		healthylink_find_driver(data->module_id);

	if (drv != NULL) {
		LOG_INF("Loading HealthyLink driver: %s", drv->name);
		ret = drv->probe(dev, &data->module_header);
		if (ret == 0) {
			data->active_driver = drv;
			data->status = HEALTHYLINK_STATUS_READY;
			LOG_INF("Module initialized successfully");
		} else {
			LOG_ERR("Driver probe failed: %d", ret);
			data->status = HEALTHYLINK_STATUS_ERROR;
		}
		k_mutex_unlock(&data->lock);
		return ret;
	}

	/* No HealthyLink-specific driver found.
	 * For known module types (EEG, CAN, etc.), this is normal - the sensor
	 * driver handles functionality directly. Mark as READY since the module
	 * was successfully identified and EEPROM is valid.
	 *
	 * For truly unknown modules, try LLEXT fallback if enabled.
	 */
	const char *mod_name = healthylink_module_name(data->module_id);
	if (strcmp(mod_name, "Unknown") != 0) {
		/* Known module type - sensor driver handles this */
		LOG_INF("Module %s (0x%04X) ready - handled by sensor driver",
			mod_name, data->module_id);
		data->status = HEALTHYLINK_STATUS_READY;
		k_mutex_unlock(&data->lock);
		return 0;
	}

#if IS_ENABLED(CONFIG_HEALTHYLINK_LLEXT_FALLBACK)
	LOG_INF("Unknown module 0x%04X, trying LLEXT...", data->module_id);
	ret = healthylink_load_llext(dev, data->module_id);
	if (ret == 0) {
		data->status = HEALTHYLINK_STATUS_READY;
	} else {
		data->status = HEALTHYLINK_STATUS_NO_DRIVER;
	}
	k_mutex_unlock(&data->lock);
	return ret;
#else
	LOG_WRN("Unknown module ID 0x%04X - no driver available", data->module_id);
	data->status = HEALTHYLINK_STATUS_NO_DRIVER;
	k_mutex_unlock(&data->lock);
	return -ENOTSUP;
#endif
}

/* Get module info */
int healthylink_get_module_info(const struct device *dev,
				struct healthylink_header *header)
{
	struct healthylink_data *data = healthylink_get_data(dev);

	if (data->status == HEALTHYLINK_STATUS_NOT_PRESENT) {
		return -ENODEV;
	}

	if (header != NULL) {
		k_mutex_lock(&data->lock, K_FOREVER);
		*header = data->module_header;
		k_mutex_unlock(&data->lock);
	}

	return 0;
}

/* Get module status */
enum healthylink_status healthylink_get_status(const struct device *dev)
{
	struct healthylink_data *data = healthylink_get_data(dev);
	return data->status;
}

/* Get module ID */
uint16_t healthylink_get_module_id(const struct device *dev)
{
	struct healthylink_data *data = healthylink_get_data(dev);
	return data->module_id;
}

/* Check if specific module is present */
bool healthylink_is_module_present(const struct device *dev, uint16_t module_id)
{
	struct healthylink_data *data = healthylink_get_data(dev);
	return (data->status == HEALTHYLINK_STATUS_READY &&
		data->module_id == module_id);
}

/* Driver initialization */
static int healthylink_init(const struct device *dev)
{
	const struct healthylink_config *cfg = healthylink_get_config(dev);
	struct healthylink_data *data = healthylink_get_data(dev);
	int ret;

	memset(data, 0, sizeof(*data));
	k_mutex_init(&data->lock);

	data->status = HEALTHYLINK_STATUS_NOT_PRESENT;

	/* Verify I2C bus ready */
	if (!device_is_ready(cfg->eeprom.bus)) {
		LOG_ERR("I2C bus not ready: %s", cfg->eeprom.bus->name);
		return -ENODEV;
	}

	/* No detection GPIO: module presence is determined by probing the slot
	 * ID EEPROM over I2C (see healthylink_module_present()). */

	/* The slot starts off until detect() identifies a module -- except a
	 * switched-rail slot, whose EEPROM needs the rail and stays on. */
	if (cfg->power_gpio.port != NULL) {
		if (!gpio_is_ready_dt(&cfg->power_gpio)) {
			LOG_ERR("slot %s: power GPIO not ready", cfg->label);
			return -ENODEV;
		}
		ret = gpio_pin_configure_dt(&cfg->power_gpio,
					    cfg->eeprom_switched ? GPIO_OUTPUT_ACTIVE
								 : GPIO_OUTPUT_INACTIVE);
		if (ret < 0) {
			LOG_ERR("slot %s: failed to configure power GPIO: %d",
				cfg->label, ret);
			return ret;
		}
		data->powered = cfg->eeprom_switched;
	}

	/* Configure slot load-switch fault GPIO if available */
	if (cfg->fault_gpio.port != NULL) {
		if (!gpio_is_ready_dt(&cfg->fault_gpio)) {
			LOG_ERR("Fault GPIO not ready");
			return -ENODEV;
		}
		ret = gpio_pin_configure_dt(&cfg->fault_gpio, GPIO_INPUT);
		if (ret < 0) {
			LOG_ERR("Failed to configure fault GPIO: %d", ret);
			return ret;
		}
	}

	LOG_INF("HealthyLink slot %s initialized (ID EEPROM @0x%02x)", cfg->label,
		cfg->eeprom.addr);

	/* Auto-detect if configured */
#if IS_ENABLED(CONFIG_HEALTHYLINK_AUTO_DETECT)
	/* Give the EEPROM time to power up if module was just connected */
	k_msleep(10);
	healthylink_detect(dev);
#endif

	return 0;
}

/* One device per slot node; a slot without an `eeprom` gets none. */
#define HEALTHYLINK_SLOT_DEFINE(inst)                                        \
	static struct healthylink_data healthylink_data_##inst;              \
                                                                             \
	static const struct healthylink_config healthylink_config_##inst = { \
		.eeprom = I2C_DT_SPEC_GET(DT_INST_PHANDLE(inst, eeprom)),    \
		.power_gpio = GPIO_DT_SPEC_INST_GET_OR(inst, power_gpios, {0}), \
		.fault_gpio = GPIO_DT_SPEC_INST_GET_OR(inst, fault_gpios, {0}), \
		.eeprom_switched = DT_INST_PROP(inst, eeprom_switched_rail), \
		.label = DT_INST_PROP_OR(inst, slot_label, "?"),              \
	};                                                                   \
                                                                             \
	DEVICE_DT_INST_DEFINE(inst, healthylink_init, NULL,                  \
			      &healthylink_data_##inst,                      \
			      &healthylink_config_##inst,                    \
			      POST_KERNEL, CONFIG_HEALTHYLINK_INIT_PRIORITY, \
			      NULL);

#define HEALTHYLINK_SLOT_INIT(inst)                                          \
	COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, eeprom),                     \
		    (HEALTHYLINK_SLOT_DEFINE(inst)), ())

DT_INST_FOREACH_STATUS_OKAY(HEALTHYLINK_SLOT_INIT)
