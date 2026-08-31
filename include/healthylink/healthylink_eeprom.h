/*
 * HealthyLink EEPROM Header Format
 * Copyright (c) 2025 ProtoCentral
 * SPDX-License-Identifier: MIT
 *
 * This header defines the binary format of the HealthyLink module
 * identification EEPROM. Use this for creating EEPROM images for
 * new modules.
 */

#ifndef INCLUDE_HEALTHYLINK_HEALTHYLINK_EEPROM_H_
#define INCLUDE_HEALTHYLINK_HEALTHYLINK_EEPROM_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup healthylink_eeprom HealthyLink EEPROM Format
 * @{
 */

/**
 * @brief Raw EEPROM header structure (packed for direct I2C read)
 *
 * This structure can be read directly from the EEPROM starting at address 0x00.
 * All multi-byte fields are little-endian.
 */
struct __attribute__((packed)) healthylink_eeprom_header {
	/* 0x00-0x03: Magic number "HLNK" */
	uint8_t magic[4];

	/* 0x04: Header version */
	uint8_t version;

	/* 0x05: Header length */
	uint8_t header_len;

	/* 0x06-0x07: Module ID (little-endian) */
	uint16_t module_id;

	/* 0x08: Hardware revision major */
	uint8_t hw_rev_major;

	/* 0x09: Hardware revision minor */
	uint8_t hw_rev_minor;

	/* 0x0A-0x0B: Minimum firmware version (little-endian) */
	uint16_t fw_compat_min;

	/* 0x0C-0x0F: Capabilities bitmap (little-endian) */
	uint32_t capabilities;

	/* 0x10-0x2F: Module name (32 bytes, null-terminated) */
	char name[32];

	/* 0x30-0x4F: Manufacturer (32 bytes, null-terminated) */
	char manufacturer[32];

	/* 0x50-0x53: Serial number (little-endian) */
	uint32_t serial;

	/* 0x54-0x5F: Reserved (12 bytes) */
	uint8_t reserved[12];
};

/**
 * @brief Module-specific configuration block
 *
 * Located at EEPROM address 0x60-0x7F (32 bytes). Contents are
 * module-specific; see the healthylink_config_* structs below.
 */
struct __attribute__((packed)) healthylink_eeprom_config {
	uint8_t data[32];
};

/**
 * @brief Extended data block
 *
 * Located at EEPROM address 0x80-0xFD (126 bytes).
 * Can be used for:
 *   - Calibration data
 *   - Factory test results
 *   - Additional configuration
 */
struct __attribute__((packed)) healthylink_eeprom_extended {
	uint8_t data[126];
};

/**
 * @brief CRC block at end of EEPROM
 *
 * Located at EEPROM address 0xFE-0xFF.
 * CRC-16-CCITT calculated over bytes 0x00-0xFD.
 */
struct __attribute__((packed)) healthylink_eeprom_crc {
	uint16_t crc16;
};

/**
 * @brief Complete EEPROM image structure
 *
 * This structure represents the complete 256-byte EEPROM contents.
 * Can be used for generating or validating EEPROM images.
 */
struct __attribute__((packed)) healthylink_eeprom_image {
	struct healthylink_eeprom_header header;  /* 0x00-0x5F (96 bytes) */
	struct healthylink_eeprom_config config;  /* 0x60-0x7F (32 bytes) */
	struct healthylink_eeprom_extended ext;   /* 0x80-0xFD (126 bytes) */
	struct healthylink_eeprom_crc crc;        /* 0xFE-0xFF (2 bytes) */
};

/* Compile-time size verification */
_Static_assert(sizeof(struct healthylink_eeprom_image) == 256,
	       "EEPROM image must be exactly 256 bytes");

_Static_assert(sizeof(struct healthylink_eeprom_header) == 96,
	       "EEPROM header must be exactly 96 bytes");

/**
 * @brief EEG-8CH module configuration structure
 */
struct __attribute__((packed)) healthylink_config_eeg_8ch {
	/** Default sample rate in SPS (250, 500, 1000, 2000) */
	uint16_t default_sample_rate;

	/** Default gain index (0-6 maps to ADS1299 gains) */
	uint8_t default_gain;

	/** Channel enable mask (bit N = channel N enabled) */
	uint8_t channel_mask;

	/** Lead-off detection enable */
	uint8_t leadoff_enable;

	/** Reserved for future use */
	uint8_t reserved[27];
};

_Static_assert(sizeof(struct healthylink_config_eeg_8ch) == 32,
	       "EEG config must be 32 bytes");

/**
 * @brief CAN interface module configuration structure
 */
struct __attribute__((packed)) healthylink_config_can {
	/** Default bus speed in bps (125000, 250000, 500000, 1000000) */
	uint32_t default_bus_speed;

	/** CAN-FD data phase speed in bps (0 = classic CAN only) */
	uint32_t default_data_speed;

	/** Termination resistor enable (1 = enabled) */
	uint8_t termination_enable;

	/** Silent mode (listen-only) enable */
	uint8_t silent_mode;

	/** Reserved for future use */
	uint8_t reserved[22];
};

_Static_assert(sizeof(struct healthylink_config_can) == 32,
	       "CAN config must be 32 bytes");

/**
 * @brief Trigger I/O module configuration structure
 */
struct __attribute__((packed)) healthylink_config_trigger {
	/** Input debounce time in microseconds */
	uint16_t debounce_us;

	/** Output polarity (bit N = output N active high) */
	uint8_t output_polarity;

	/** Input polarity (bit N = input N active high) */
	uint8_t input_polarity;

	/** Edge detection (0=rising, 1=falling, 2=both) for each input */
	uint8_t edge_config[4];

	/** Reserved for future use */
	uint8_t reserved[24];
};

_Static_assert(sizeof(struct healthylink_config_trigger) == 32,
	       "Trigger config must be 32 bytes");

/**
 * @brief HealthyLink Compute module configuration structure
 */
struct __attribute__((packed)) healthylink_config_ai {
	/** Accelerator type (0=generic, 1=Coral TPU, 2=custom) */
	uint8_t accelerator_type;

	/** SPI clock speed in MHz */
	uint8_t spi_speed_mhz;

	/** Maximum inference time in ms (for timeout) */
	uint16_t inference_timeout_ms;

	/** Input tensor size in bytes */
	uint16_t input_size;

	/** Output tensor size in bytes */
	uint16_t output_size;

	/** Reserved for future use */
	uint8_t reserved[24];
};

_Static_assert(sizeof(struct healthylink_config_ai) == 32,
	       "AI config must be 32 bytes");

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* INCLUDE_HEALTHYLINK_HEALTHYLINK_EEPROM_H_ */
