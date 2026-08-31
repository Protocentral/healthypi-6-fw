/*
 * Texas Instruments ADS1299 - 8-Channel 24-Bit EEG Analog Front-End
 * Copyright (c) 2025 ProtoCentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Driver header for ADS1299 EEG AFE used on HealthyLink EEG-8CH module.
 * This is a separate driver from ADS129xx (ECG) due to EEG-specific features:
 * - 8 fixed channels (vs 4/6/8 variable on ADS129xR)
 * - Different gain mapping (includes 24x for EEG)
 * - BIAS drive circuit for patient reference
 * - SRB1/SRB2 reference routing for EEG montages
 * - No respiration demodulation (EEG-specific)
 */

#ifndef ZEPHYR_DRIVERS_SENSOR_ADS1299_H_
#define ZEPHYR_DRIVERS_SENSOR_ADS1299_H_

#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ADS1299 Register Addresses
 */
#define ADS1299_REG_ID          0x00    /* Device ID (read-only) */
#define ADS1299_REG_CONFIG1     0x01    /* Configuration 1 */
#define ADS1299_REG_CONFIG2     0x02    /* Configuration 2 */
#define ADS1299_REG_CONFIG3     0x03    /* Configuration 3 */
#define ADS1299_REG_LOFF        0x04    /* Lead-Off Control */
#define ADS1299_REG_CH1SET      0x05    /* Channel 1 Settings */
#define ADS1299_REG_CH2SET      0x06    /* Channel 2 Settings */
#define ADS1299_REG_CH3SET      0x07    /* Channel 3 Settings */
#define ADS1299_REG_CH4SET      0x08    /* Channel 4 Settings */
#define ADS1299_REG_CH5SET      0x09    /* Channel 5 Settings */
#define ADS1299_REG_CH6SET      0x0A    /* Channel 6 Settings */
#define ADS1299_REG_CH7SET      0x0B    /* Channel 7 Settings */
#define ADS1299_REG_CH8SET      0x0C    /* Channel 8 Settings */
#define ADS1299_REG_BIAS_SENSP  0x0D    /* BIAS Positive Signal Derivation */
#define ADS1299_REG_BIAS_SENSN  0x0E    /* BIAS Negative Signal Derivation */
#define ADS1299_REG_LOFF_SENSP  0x0F    /* Lead-Off Positive */
#define ADS1299_REG_LOFF_SENSN  0x10    /* Lead-Off Negative */
#define ADS1299_REG_LOFF_FLIP   0x11    /* Lead-Off Flip */
#define ADS1299_REG_LOFF_STATP  0x12    /* Lead-Off Status Positive (read-only) */
#define ADS1299_REG_LOFF_STATN  0x13    /* Lead-Off Status Negative (read-only) */
#define ADS1299_REG_GPIO        0x14    /* GPIO Control */
#define ADS1299_REG_MISC1       0x15    /* Miscellaneous 1 (SRB1 control) */
#define ADS1299_REG_MISC2       0x16    /* Miscellaneous 2 */
#define ADS1299_REG_CONFIG4     0x17    /* Configuration 4 */

/*
 * ADS1299 SPI Commands
 */
#define ADS1299_CMD_WAKEUP      0x02    /* Wake-up from standby */
#define ADS1299_CMD_STANDBY     0x04    /* Enter standby mode */
#define ADS1299_CMD_RESET       0x06    /* Reset all registers */
#define ADS1299_CMD_START       0x08    /* Start conversion */
#define ADS1299_CMD_STOP        0x0A    /* Stop conversion */
#define ADS1299_CMD_RDATAC      0x10    /* Enable continuous data read mode */
#define ADS1299_CMD_SDATAC      0x11    /* Stop continuous data read mode */
#define ADS1299_CMD_RDATA       0x12    /* Read single data sample */
#define ADS1299_CMD_RREG        0x20    /* Read register (OR with reg addr) */
#define ADS1299_CMD_WREG        0x40    /* Write register (OR with reg addr) */

/*
 * ADS1299 Device ID
 * ID register: [7:5]=110 (ADS1299), [4:2]=REV, [1:0]=NU (number of channels - 1 = 8)
 */
#define ADS1299_DEVICE_ID_MASK  0xE0    /* Bits [7:5] */
#define ADS1299_DEVICE_ID       0xC0    /* 110xxxxx = ADS1299 */
#define ADS1299_NUM_CHANNELS    8

/*
 * CONFIG1 Register Bits
 */
#define ADS1299_CONFIG1_HR      0x80    /* High-resolution mode (always 1) */
#define ADS1299_CONFIG1_DAISY_EN 0x40   /* Daisy-chain enable */
#define ADS1299_CONFIG1_CLK_EN  0x20    /* CLK output enable */

/* Data Rate Selection [2:0] */
#define ADS1299_DR_16KSPS       0x00    /* fMOD/64  = 16 kSPS */
#define ADS1299_DR_8KSPS        0x01    /* fMOD/128 = 8 kSPS */
#define ADS1299_DR_4KSPS        0x02    /* fMOD/256 = 4 kSPS */
#define ADS1299_DR_2KSPS        0x03    /* fMOD/512 = 2 kSPS */
#define ADS1299_DR_1KSPS        0x04    /* fMOD/1024 = 1 kSPS */
#define ADS1299_DR_500SPS       0x05    /* fMOD/2048 = 500 SPS */
#define ADS1299_DR_250SPS       0x06    /* fMOD/4096 = 250 SPS (default for EEG) */

/*
 * CONFIG2 Register Bits
 */
#define ADS1299_CONFIG2_INT_CAL 0x10    /* Internal calibration signal */
#define ADS1299_CONFIG2_CAL_AMP 0x04    /* Calibration amplitude: 0=1x, 1=2x */
#define ADS1299_CONFIG2_CAL_FREQ_MASK 0x03  /* Calibration frequency */

/*
 * CONFIG3 Register Bits
 */
#define ADS1299_CONFIG3_PD_REFBUF   0x80    /* Internal reference buffer power-down */
#define ADS1299_CONFIG3_BIAS_MEAS   0x10    /* BIAS measurement */
#define ADS1299_CONFIG3_BIASREF_INT 0x08    /* BIAS reference: 0=external, 1=internal */
#define ADS1299_CONFIG3_PD_BIAS     0x04    /* BIAS buffer power-down */
#define ADS1299_CONFIG3_BIAS_LOFF_SENS 0x02 /* BIAS lead-off sense function */
#define ADS1299_CONFIG3_BIAS_STAT   0x01    /* BIAS lead-off status (read-only) */
#define ADS1299_CONFIG3_CONST       0x60    /* Must be set */

/*
 * CHnSET Register Bits (Channel Settings)
 */
#define ADS1299_CHNSET_PD       0x80    /* Channel power-down */

/* PGA Gain Selection [6:4] - ADS1299 specific (different from ADS129xR!) */
#define ADS1299_GAIN_1          0x00    /* Gain = 1 */
#define ADS1299_GAIN_2          0x10    /* Gain = 2 */
#define ADS1299_GAIN_4          0x20    /* Gain = 4 */
#define ADS1299_GAIN_6          0x30    /* Gain = 6 */
#define ADS1299_GAIN_8          0x40    /* Gain = 8 */
#define ADS1299_GAIN_12         0x50    /* Gain = 12 */
#define ADS1299_GAIN_24         0x60    /* Gain = 24 (recommended for EEG) */

/* SRB2 Connection [3] */
#define ADS1299_CHNSET_SRB2     0x08    /* Connect SRB2 to negative input */

/* Input Multiplexer Selection [2:0] */
#define ADS1299_MUX_NORMAL      0x00    /* Normal electrode input */
#define ADS1299_MUX_SHORTED     0x01    /* Input shorted (offset measurement) */
#define ADS1299_MUX_BIAS_MEAS   0x02    /* BIAS measurement */
#define ADS1299_MUX_MVDD        0x03    /* MVDD for supply measurement */
#define ADS1299_MUX_TEMP        0x04    /* Temperature sensor */
#define ADS1299_MUX_TEST        0x05    /* Test signal */
#define ADS1299_MUX_BIAS_DRP    0x06    /* BIAS_DRP (positive BIAS drive) */
#define ADS1299_MUX_BIAS_DRN    0x07    /* BIAS_DRN (negative BIAS drive) */

/*
 * MISC1 Register Bits (SRB1 Control)
 */
#define ADS1299_MISC1_SRB1      0x20    /* Connect SRB1 to all channel negative inputs */

/*
 * CONFIG4 Register Bits
 */
#define ADS1299_CONFIG4_SINGLE_SHOT 0x08    /* Single-shot conversion mode */
#define ADS1299_CONFIG4_PD_LOFF_COMP 0x02   /* Lead-off comparator power-down */

/*
 * Zephyr Sensor API Extensions
 */

/* Custom sensor channels for ADS1299 */
enum ads1299_channel {
	SENSOR_CHAN_ADS1299_CH1 = SENSOR_CHAN_PRIV_START,
	SENSOR_CHAN_ADS1299_CH2,
	SENSOR_CHAN_ADS1299_CH3,
	SENSOR_CHAN_ADS1299_CH4,
	SENSOR_CHAN_ADS1299_CH5,
	SENSOR_CHAN_ADS1299_CH6,
	SENSOR_CHAN_ADS1299_CH7,
	SENSOR_CHAN_ADS1299_CH8,
	SENSOR_CHAN_ADS1299_ALL,    /* Fetch all 8 channels */
};

/* Custom sensor attributes for ADS1299 */
enum ads1299_attribute {
	/* Start/stop data acquisition (val1: 1=start, 0=stop) */
	ADS1299_ATTR_START = SENSOR_ATTR_PRIV_START,
	/* Set gain for a channel (val1: gain value 1,2,4,6,8,12,24) */
	ADS1299_ATTR_GAIN,
	/* Set sample rate (val1: rate in SPS: 250,500,1000,2000,4000,8000,16000) */
	ADS1299_ATTR_SAMPLE_RATE,
	/* Enable/disable SRB1 (val1: 1=enable, 0=disable) */
	ADS1299_ATTR_SRB1,
	/* Enable/disable BIAS for a channel (val1: 1=enable, 0=disable) */
	ADS1299_ATTR_BIAS,
	/* Set input mux for a channel (val1: mux selection) */
	ADS1299_ATTR_INPUT_MUX,
	/* Read device ID (val1: output) */
	ADS1299_ATTR_DEVICE_ID,
	/* Enter impedance check mode */
	ADS1299_ATTR_IMPEDANCE_CHECK,
};

/*
 * Driver Configuration Structure
 */
struct ads1299_config {
	struct spi_dt_spec spi;
	struct gpio_dt_spec drdy_gpio;
	struct gpio_dt_spec reset_gpio;
	struct gpio_dt_spec pwdn_gpio;
	struct gpio_dt_spec start_gpio;
	uint8_t gain;           /* Initial gain setting */
	uint8_t sample_rate;    /* Initial sample rate code */
	bool bias_enabled;      /* Enable BIAS circuit */
	bool srb1_enabled;      /* Enable SRB1 reference */
};

/*
 * Driver Data Structure
 */
struct ads1299_data {
	const struct device *dev;
	int32_t channels[8];    /* Latest sample values */
	uint32_t status;        /* Status word from last read */

	/* Trigger handling */
	struct gpio_callback drdy_cb;
	const struct sensor_trigger *trigger;
	sensor_trigger_handler_t handler;
	struct k_work work;

	/* State */
	bool running;
	bool continuous_mode;
	bool hw_present;        /* True if hardware detected and initialized */
};

/*
 * Public API Functions
 */

/**
 * @brief Initialize ADS1299 hardware (deferred initialization)
 *
 * Must be called from the application: init is deferred so boot does not hang
 * when the HealthyLink module is absent.
 *
 * @param dev ADS1299 device instance
 * @return 0 on success, -ENODEV if SPI/GPIO not ready, -ENOENT if hardware
 *         not detected, other negative errno on failure
 */
int ads1299_hw_init(const struct device *dev);

/*
 * Helper function prototypes (for external use if needed)
 */

/**
 * @brief Convert gain value to register bits
 * @param gain Gain value (1, 2, 4, 6, 8, 12, 24)
 * @return Register bits for CHnSET[6:4]
 */
static inline uint8_t ads1299_gain_to_bits(uint8_t gain)
{
	switch (gain) {
	case 1:  return ADS1299_GAIN_1;
	case 2:  return ADS1299_GAIN_2;
	case 4:  return ADS1299_GAIN_4;
	case 6:  return ADS1299_GAIN_6;
	case 8:  return ADS1299_GAIN_8;
	case 12: return ADS1299_GAIN_12;
	case 24: return ADS1299_GAIN_24;
	default: return ADS1299_GAIN_24;  /* Default for EEG */
	}
}

/**
 * @brief Convert sample rate to register bits
 * @param sps Sample rate in SPS
 * @return Register bits for CONFIG1[2:0]
 */
static inline uint8_t ads1299_sps_to_bits(uint16_t sps)
{
	switch (sps) {
	case 16000: return ADS1299_DR_16KSPS;
	case 8000:  return ADS1299_DR_8KSPS;
	case 4000:  return ADS1299_DR_4KSPS;
	case 2000:  return ADS1299_DR_2KSPS;
	case 1000:  return ADS1299_DR_1KSPS;
	case 500:   return ADS1299_DR_500SPS;
	case 250:
	default:    return ADS1299_DR_250SPS;
	}
}

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_DRIVERS_SENSOR_ADS1299_H_ */
