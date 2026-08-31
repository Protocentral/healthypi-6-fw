/*
 * Copyright (c) 2026 ProtoCentral Electronics
 * SPDX-License-Identifier: MIT
 */

// ______          _        _____            _             _
// | ___ \        | |      /  __ \          | |           | |
// | |_/ / __ ___ | |_ ___ | /  \/ ___ _ __ | |_ _ __ __ _| |
// |  __/ '__/ _ \| __/ _ \| |    / _ \ '_ \| __| '__/ _` | |
// | |  | | | (_) | || (_) | \__/\  __/ | | | |_| | | (_| | |
// \_|  |_|  \___/ \__\___/ \____/\___|_| |_|\__|_|  \__,_|_|

//////////////////////////////////////////////////////////////////////////////////////////
//
//  Copyright (c) 2025 ProtoCentral
//
//  This software is licensed under the MIT License(http://opensource.org/licenses/MIT).
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
//  NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
//  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
//  WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
//  SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//
//  For information on how to use, visit https://github.com/Protocentral/protocentral-max30001-arduino
//
/////////////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>

#define ADS129XX_STACK_SIZE  2048

struct ads129xx_config
{
    struct spi_dt_spec spi;

    struct gpio_dt_spec reset_pin;
    struct gpio_dt_spec start_pin;
    struct gpio_dt_spec cs_pin;
    struct gpio_dt_spec drdy_pin;
    struct gpio_dt_spec pwdn_pin;
    
    uint8_t gain;  // PGA gain for all channels (1, 2, 3, 4, 6, 8, or 12)
};

struct ads129xx_data
{
    int32_t adc_data_ch0;
    int32_t adc_data_ch1;
    int32_t adc_data_ch2;
    int32_t adc_data_ch3;
    int32_t adc_data_ch4;
    int32_t adc_data_ch5;
    int32_t adc_data_ch6;
    int32_t adc_data_ch7;

    // ADS129XX config
    int number_channels;

    //Trigger vars
    const struct device *dev;
    struct gpio_callback drdy_gpio_cb;

    const struct sensor_trigger *drdy_trigger;
    sensor_trigger_handler_t drdy_handler;

    K_KERNEL_STACK_MEMBER(stack, ADS129XX_STACK_SIZE);
    struct k_thread thread;
    struct k_sem drdy_sem;

    struct k_work work;
};

enum ads129xx_attribute
{
    ADS129XX_CHAN_ENABLE = SENSOR_ATTR_PRIV_START + 1,
    ADS129XX_CHAN_DISABLE = SENSOR_ATTR_PRIV_START + 2,
    ADS129XX_CHAN_SET_GAIN = SENSOR_ATTR_PRIV_START + 3,
    ADS129XX_CHAN_SET_MUX = SENSOR_ATTR_PRIV_START + 4,
    ADS129XX_ATTR_START = SENSOR_ATTR_PRIV_START + 5,
    ADS129XX_ATTR_STOP = SENSOR_ATTR_PRIV_START + 6,
};

enum ads129xx_channel
{
    SENSOR_CHAN_ADS129XX_CH0 = SENSOR_CHAN_PRIV_START,
    SENSOR_CHAN_ADS129XX_CH1 = SENSOR_CHAN_PRIV_START + 1,
    SENSOR_CHAN_ADS129XX_CH2 = SENSOR_CHAN_PRIV_START + 2,
    SENSOR_CHAN_ADS129XX_CH3 = SENSOR_CHAN_PRIV_START + 3,
    SENSOR_CHAN_ADS129XX_CH4 = SENSOR_CHAN_PRIV_START + 4,
    SENSOR_CHAN_ADS129XX_CH5 = SENSOR_CHAN_PRIV_START + 5,
    SENSOR_CHAN_ADS129XX_CH6 = SENSOR_CHAN_PRIV_START + 6,
    SENSOR_CHAN_ADS129XX_CH7 = SENSOR_CHAN_PRIV_START + 7
};

enum ads129xx_command
{
    ADS129XX_CMD_WAKEUP = 0x02,
    ADS129XX_CMD_STANDBY = 0x04,
    ADS129XX_CMD_RESET = 0x06,
    ADS129XX_CMD_START = 0x08,
    ADS129XX_CMD_STOP = 0x0a,

    ADS129XX_CMD_RDATAC = 0x10,
    ADS129XX_CMD_SDATAC = 0x11,
    ADS129XX_CMD_RDATA = 0x12,

    ADS129XX_CMD_RREG = 0x20,
    ADS129XX_CMD_WREG = 0x40
};

enum ads129xx_registers
{
    ADS129XX_REG_ID = 0x00,

    ADS129XX_REG_CONFIG1 = 0x01,
    ADS129XX_REG_CONFIG2 = 0x02,
    ADS129XX_REG_CONFIG3 = 0x03,
    ADS129XX_REG_LOFF = 0x04,

    ADS129XX_REG_CHnSET = 0x05,
    ADS129XX_REG_CH1SET = ADS129XX_REG_CHnSET + 1,
    ADS129XX_REG_CH2SET = ADS129XX_REG_CHnSET + 2,
    ADS129XX_REG_CH3SET = ADS129XX_REG_CHnSET + 3,
    ADS129XX_REG_CH4SET = ADS129XX_REG_CHnSET + 4,
    ADS129XX_REG_CH5SET = ADS129XX_REG_CHnSET + 5,
    ADS129XX_REG_CH6SET = ADS129XX_REG_CHnSET + 6,
    ADS129XX_REG_CH7SET = ADS129XX_REG_CHnSET + 7,
    ADS129XX_REG_CH8SET = ADS129XX_REG_CHnSET + 8,

	ADS129XX_REG_RLD_SENSP = 0x0d,
	ADS129XX_REG_RLD_SENSN = 0x0e,

	ADS129XX_REG_LOFF_SENSP = 0x0f,
	ADS129XX_REG_LOFF_SENSN = 0x10,
	ADS129XX_REG_LOFF_FLIP = 0x11,

	ADS129XX_REG_LOFF_STATP = 0x12,
	ADS129XX_REG_LOFF_STATN = 0x13,

	ADS129XX_REG_GPIO = 0x14,
	ADS129XX_REG_PACE = 0x15,
	ADS129XX_REG_RESP = 0x16,
	ADS129XX_REG_CONFIG4 = 0x17,
	ADS129XX_REG_WCT1 = 0x18,
	ADS129XX_REG_WCT2 = 0x19
};

/* LOFF register bits (0x04) - Lead-Off Detection Control */
enum LOFF_bits
{
    /* COMP_TH[2:0] - Lead-off comparator threshold (bits 7:5) */
    LOFF_COMP_TH_95  = 0x00,  /* 95% / 5% */
    LOFF_COMP_TH_92  = 0x20,  /* 92.5% / 7.5% */
    LOFF_COMP_TH_90  = 0x40,  /* 90% / 10% */
    LOFF_COMP_TH_87  = 0x60,  /* 87.5% / 12.5% */
    LOFF_COMP_TH_85  = 0x80,  /* 85% / 15% */
    LOFF_COMP_TH_80  = 0xA0,  /* 80% / 20% */
    LOFF_COMP_TH_75  = 0xC0,  /* 75% / 25% */
    LOFF_COMP_TH_70  = 0xE0,  /* 70% / 30% */

    /* VLEAD_OFF_EN - Lead-off detection mode (bit 4) */
    LOFF_VLEAD_OFF_EN = 0x10,  /* 1 = pull-up/pull-down resistor mode */

    /* ILEAD_OFF[1:0] - Lead-off current magnitude (bits 3:2) */
    LOFF_ILEAD_OFF_6NA  = 0x00,  /* 6 nA */
    LOFF_ILEAD_OFF_12NA = 0x04,  /* 12 nA (was 22nA in older datasheets) */
    LOFF_ILEAD_OFF_6UA  = 0x08,  /* 6 µA */
    LOFF_ILEAD_OFF_24NA = 0x0C,  /* 24 nA */

    /* FLEAD_OFF[1:0] - Lead-off frequency (bits 1:0) */
    LOFF_FLEAD_OFF_DC     = 0x00,  /* DC lead-off detection */
    LOFF_FLEAD_OFF_AC_7_8 = 0x01,  /* AC @ fDR/4 (7.8125 Hz @ 500 SPS) */
    LOFF_FLEAD_OFF_AC_31  = 0x02,  /* AC @ fDR (31.25 Hz @ 500 SPS) */
    LOFF_FLEAD_OFF_FDR_4  = 0x03,  /* fDR/4 for DC lead-off */
};

enum CONFIG3_bits
{
    PD_REFBUF = 0x80,
    VREF_4V = 0x20,
    RLD_MEAS = 0x10,
    RLDREF_INT = 0x08,
    PD_RLD = 0x04,
    RLD_LOFF_SENS = 0x02,
    RLD_STAT = 0x01,

    CONFIG3_const = 0x60
};

enum CONFIG1_bits
{
    DR_250 = 0x06,
    DR_500 = 0x05,
    DR_1K = 0x04,
    DR_2K = 0x03,
    DR_4K = 0x02,
    DR_8K = 0x01,
    DR_16K = 0x00,

    CONFIG1_const = 0x90
};

/* RESP register bits (0x16) - Respiration Control */
enum RESP_bits
{
    RESP_DEMOD_EN1 = 0x80,  /* Enable respiration demodulation on channel 1 */
    RESP_MOD_EN1   = 0x40,  /* Enable respiration modulation (excitation current) */
    RESP_PH2       = 0x10,  /* Respiration phase bit 2 */
    RESP_PH1       = 0x08,  /* Respiration phase bit 1 */
    RESP_PH0       = 0x04,  /* Respiration phase bit 0 */
    RESP_CTRL1     = 0x02,  /* Respiration control bit 1 */
    RESP_CTRL0     = 0x01,  /* Respiration control bit 0 */

    /* RESP_CTRL[1:0] settings */
    RESP_CTRL_OFF         = 0x00,  /* Respiration off */
    RESP_CTRL_EXT         = 0x01,  /* External respiration */
    RESP_CTRL_INT_SIG_INT = 0x02,  /* Internal resp with internal signals */
    RESP_CTRL_INT_SIG_EXT = 0x03,  /* Internal resp with external signals */

    /* RESP_PH[2:0] phase settings (22.5° steps) */
    RESP_PH_22_5  = 0x00,
    RESP_PH_45    = 0x04,
    RESP_PH_67_5  = 0x08,
    RESP_PH_90    = 0x0C,
    RESP_PH_112_5 = 0x10,
    RESP_PH_135   = 0x14,
    RESP_PH_157_5 = 0x18,
    RESP_PH_180   = 0x1C
};

/* CONFIG4 register bits (0x17) */
enum CONFIG4_bits
{
    RESP_FREQ        = 0x01,  /* 0 = 32kHz, 1 = 64kHz modulation clock */
    SINGLE_SHOT      = 0x08,  /* Single-shot conversion */
    WCT_TO_RLD       = 0x04,  /* Connect WCT to RLD */
    PD_LOFF_COMP     = 0x02,  /* Lead-off comparator power-down */
};

enum CHnSET_bits
{
    PDn = 0x80,
    PD_n = 0x80,
    GAINn2 = 0x40,
    GAINn1 = 0x20,
    GAINn0 = 0x10,
    MUXn2 = 0x04,
    MUXn1 = 0x02,
    MUXn0 = 0x01,

    CHnSET_const = 0x00,

    // ADS129XX
    GAIN_1X = 0x00,
    GAIN_2X = GAINn0,
    GAIN_4X = GAINn1,
    GAIN_6X = (GAINn1 | GAINn0),
    GAIN_8X = GAINn2,
    GAIN_12X = (GAINn2 | GAINn0),
    GAIN_24X = (GAINn2 | GAINn1),

    ELECTRODE_INPUT = 0x00,
    SHORTED = MUXn0,
    RLD_INPUT = MUXn1,
    MVDD = (MUXn1 | MUXn0),
    TEMP = MUXn2,
    TEST_SIGNAL = (MUXn2 | MUXn0),
    RLD_DRP = (MUXn2 | MUXn1),
    RLD_DRN = (MUXn2 | MUXn1 | MUXn0)
};

struct ads129xx_decoder_header
{
	uint64_t timestamp;
} __attribute__((__packed__));

struct ads129xx_encoded_data
{
    struct ads129xx_decoder_header header;
    uint32_t data_stat;

    int32_t data_ch0;
    int32_t data_ch1;
    int32_t data_ch2;
    int32_t data_ch3;
};

void ads129xx_submit(const struct device *dev, struct rtio_iodev_sqe *iodev_sqe);