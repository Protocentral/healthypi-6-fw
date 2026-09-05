// ProtoCentral Electronics (info@protocentral.com)
// SPDX-License-Identifier: Apache-2.0

#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/sensor.h>

#include "ads129xx.h"

LOG_MODULE_REGISTER(sensor_ads129xx, CONFIG_SENSOR_LOG_LEVEL);

#define DT_DRV_COMPAT ti_ads129xx

#if DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 0
#warning "ADS129XX driver enabled without any devices"
#endif

/**
 * @brief Convert PGA gain to GAINn[2:0] register bits (ADS1294R/6R/8R).
 *
 * Note gain=6 is code 000, not gain=1.
 *
 * @param gain PGA gain (1, 2, 3, 4, 6, 8, or 12)
 * @return GAINn[2:0] bits shifted into position (bits 6:4); 0x30 (gain=3) on
 *         an invalid input
 */
static inline uint8_t ads129xx_gain_to_bits(uint8_t gain)
{
    switch (gain) {
        case 1:  return 0x10;  // 001 << 4
        case 2:  return 0x20;  // 010 << 4
        case 3:  return 0x30;  // 011 << 4
        case 4:  return 0x40;  // 100 << 4
        case 6:  return 0x00;  // 000 << 4
        case 8:  return 0x50;  // 101 << 4
        case 12: return 0x60;  // 110 << 4
        default:
            LOG_ERR("Invalid gain %d, defaulting to 3", gain);
            return 0x30;  // Default to gain=3
    }
}

static int _ads129xx_reg_write(const struct device *dev, uint8_t reg, uint8_t val)
{
    const struct ads129xx_config *config = dev->config;
    uint8_t cmd[] = {(ADS129XX_CMD_WREG | reg), 0x00, (uint8_t)val};

    const struct spi_buf tx_buf = {.buf = cmd, .len = sizeof(cmd)};
    const struct spi_buf_set tx = {.buffers = &tx_buf, .count = 1};
    int ret;

    ret = spi_write_dt(&config->spi, &tx);
    if (ret)
    {
        LOG_DBG("spi_write FAIL %d\n", ret);
        return ret;
    }
    return 0;
}

static int _ads129xx_send_command(const struct device *dev, uint8_t cmd)
{
    const struct ads129xx_config *config = dev->config;
    uint8_t spiTxCommand[1] = {cmd};

    const struct spi_buf tx_buf[] = {{.buf = spiTxCommand,
                                      .len = ARRAY_SIZE(spiTxCommand)}};
    const struct spi_buf_set tx = {.buffers = tx_buf,
                                   .count = ARRAY_SIZE(tx_buf)};

    return spi_write_dt(&config->spi, &tx);
}

static uint8_t _ads129xx_read_reg(const struct device *dev, uint8_t reg)
{
    const struct ads129xx_config *config = dev->config;

    uint8_t reg_val = 0;
    uint8_t buffer_tx[3] = {0};
    uint8_t buffer_rx[ARRAY_SIZE(buffer_tx)] = {0};

    const struct spi_buf tx_buf[] = {{
        .buf = buffer_tx,
        .len = ARRAY_SIZE(buffer_tx),
    }};

    const struct spi_buf rx_buf[] = {{
        .buf = buffer_rx,
        .len = ARRAY_SIZE(buffer_rx),
    }};

    const struct spi_buf_set tx = {
        .buffers = tx_buf,
        .count = ARRAY_SIZE(tx_buf),
    };

    const struct spi_buf_set rx = {
        .buffers = rx_buf,
        .count = ARRAY_SIZE(rx_buf),
    };

    buffer_tx[0] = (ADS129XX_CMD_RREG | (uint8_t)reg);
    buffer_tx[1] = 0x00;

    int result = spi_transceive_dt(&config->spi, &tx, &rx);

    if (result != 0)
    {
        LOG_ERR("SPI transceive failed: %d", result);
        return 0;
    }

    LOG_DBG("Read reg 0x%02x: TX[%02x %02x %02x] RX[%02x %02x %02x]",
            reg, buffer_tx[0], buffer_tx[1], buffer_tx[2],
            buffer_rx[0], buffer_rx[1], buffer_rx[2]);

    reg_val = buffer_rx[2];

    return reg_val;
}

static int ads129xx_read_id(const struct device *dev)
{
    struct ads129xx_data *data = dev->data;
    uint8_t id_reg = _ads129xx_read_reg(dev, ADS129XX_REG_ID);
    uint8_t id_device_types[3] = {4, 6, 8};
    uint8_t id_channels = id_device_types[(id_reg & 0x03)];

    if (((id_reg & 0xC0) >> 5) == 0b110)
    {
        LOG_INF("Found ADS129XR | ID: 0x%X | %d channels", id_reg, id_channels);
        data->number_channels = id_channels;
    }
    else
    {
        LOG_ERR("ADS129XX Device not found. ID Read:%x", id_reg);
        return -ENODEV;
    }

    return 0;
}
int ads129xx_trigger_set(const struct device *dev, const struct sensor_trigger *trig, sensor_trigger_handler_t handler)
{
    const struct ads129xx_config *config = dev->config;
    struct ads129xx_data *drv_data = dev->data;

    if (trig->type != SENSOR_TRIG_DATA_READY)
    {
        return -ENOTSUP;
    }

    drv_data->drdy_handler = handler;
    drv_data->drdy_trigger = trig;

    gpio_pin_set(config->start_pin.port, config->start_pin.pin, 1);
    k_sleep(K_MSEC(1));

    _ads129xx_send_command(dev, ADS129XX_CMD_RDATAC);
    k_sleep(K_MSEC(1));

    return 0;
}

static void ads129xx_drdy_gpio_callback(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    struct ads129xx_data *drv_data = CONTAINER_OF(cb, struct ads129xx_data, drdy_gpio_cb);
    const struct ads129xx_config *config = drv_data->dev->config;

    /* Return value is deliberately unchecked here. */
    (void)gpio_pin_interrupt_configure_dt(&config->drdy_pin, GPIO_INT_DISABLE);

    k_work_submit(&drv_data->work);
}

static void ads129xx_thread_cb(const struct device *dev)
{
    struct ads129xx_data *drv_data = dev->data;
    const struct ads129xx_config *config = dev->config;
    int ret;

    if (drv_data->drdy_handler != NULL)
    {
        drv_data->drdy_handler(dev, drv_data->drdy_trigger);
    }
    else
    {
        LOG_ERR("DRDY Handler not set");
    }

    // Re-enable interrupt
    ret = gpio_pin_interrupt_configure_dt(&config->drdy_pin, GPIO_INT_EDGE_TO_ACTIVE);
    if (ret)
    {
        LOG_ERR("Failed to configure interrupt");
    }
}

static void ads129xx_work_cb(struct k_work *work)
{
    struct ads129xx_data *drv_data = CONTAINER_OF(work, struct ads129xx_data, work);
    ads129xx_thread_cb(drv_data->dev);
}

static int ads129xx_init_interrupt(const struct device *dev)
{
    const struct ads129xx_config *config = dev->config;
    struct ads129xx_data *drv_data = dev->data;
    int ret;

    drv_data->dev = dev;

    k_work_init(&drv_data->work, ads129xx_work_cb);

    if (!gpio_is_ready_dt(&config->drdy_pin))
    {
        LOG_ERR("DRDY GPIO device %s is not ready", config->drdy_pin.port->name);
        return -EIO;
    }

    ret = gpio_pin_configure_dt(&config->drdy_pin, GPIO_INPUT);
    if (ret < 0)
    {
        LOG_ERR("Failed to configure DRDY pin %d", ret);
        return ret;
    }

    gpio_init_callback(&drv_data->drdy_gpio_cb, ads129xx_drdy_gpio_callback, BIT(config->drdy_pin.pin));
    ret = gpio_add_callback(config->drdy_pin.port, &drv_data->drdy_gpio_cb);
    if (ret < 0)
    {
        LOG_ERR("Failed to add callback %d", ret);
        return ret;
    }

    ret = gpio_pin_interrupt_configure_dt(&config->drdy_pin, GPIO_INT_EDGE_TO_ACTIVE);

    return ret;
}

static int ads129xx_attr_set(const struct device *dev, enum sensor_channel chan, enum sensor_attribute attr, const struct sensor_value *val)
{
    struct ads129xx_data *data = dev->data;
    const struct ads129xx_config *config = dev->config;

    switch (attr)
    {
    case ADS129XX_CHAN_ENABLE:
        _ads129xx_reg_write(dev, ADS129XX_REG_CHnSET + chan, 0b10000001);
        break;
    case ADS129XX_CHAN_DISABLE:
        _ads129xx_reg_write(dev, ADS129XX_REG_CHnSET + chan, 0b00000001);
        break;
    case ADS129XX_CHAN_SET_GAIN:
        // Convert gain value (1, 2, 3, 4, 6, 8, 12) to register bits
        _ads129xx_reg_write(dev, ADS129XX_REG_CHnSET + chan, ads129xx_gain_to_bits(val->val1));
        break;
    case ADS129XX_CHAN_SET_MUX:
        _ads129xx_reg_write(dev, ADS129XX_REG_CHnSET + chan, 0b00000111 | (val->val1 << 4));
        break;
    case ADS129XX_ATTR_START:
        if (val->val1 == 1)
        {
            // Start sampling
            gpio_pin_set(config->start_pin.port, config->start_pin.pin, 1);
        }
        else
        {
            // Stop sampling
            gpio_pin_set(config->start_pin.port, config->start_pin.pin, 0);
        }

        k_sleep(K_MSEC(1));
        return 0;
    default:
        return -ENOTSUP;
    }
}

static int ads129xx_channel_get(const struct device *dev, enum sensor_channel chan, struct sensor_value *val)
{
    struct ads129xx_data *drv_data = dev->data;

    switch (chan)
    {
    case SENSOR_CHAN_ADS129XX_CH0:
        val->val1 = drv_data->adc_data_ch0;
        val->val2 = 0;
        break;
    case SENSOR_CHAN_ADS129XX_CH1:
        val->val1 = drv_data->adc_data_ch1;
        val->val2 = 0;
        break;
    case SENSOR_CHAN_ADS129XX_CH2:
        val->val1 = drv_data->adc_data_ch2;
        val->val2 = 0;
        break;
    case SENSOR_CHAN_ADS129XX_CH3:
        val->val1 = drv_data->adc_data_ch3;
        val->val2 = 0;
        break;
    case SENSOR_CHAN_ADS129XX_CH4:
        val->val1 = drv_data->adc_data_ch4;
        val->val2 = 0;
        break;
    case SENSOR_CHAN_ADS129XX_CH5:
        val->val1 = drv_data->adc_data_ch5;
        val->val2 = 0;
        break;
    case SENSOR_CHAN_ADS129XX_CH6:
        val->val1 = drv_data->adc_data_ch6;
        val->val2 = 0;
        break;
    case SENSOR_CHAN_ADS129XX_CH7:
        val->val1 = drv_data->adc_data_ch7;
        val->val2 = 0;
        break;
    default:
        return -ENOTSUP;
    }
    return 0;
}

static int ads129xx_sample_fetch(const struct device *dev, enum sensor_channel chan)
{
    const struct ads129xx_config *config = dev->config;
    struct ads129xx_data *data = dev->data;

    uint8_t buffer_rx[((data->number_channels + 1) * 3)];

    const struct spi_buf rx_buf[] = {{
        .buf = buffer_rx,
        .len = ARRAY_SIZE(buffer_rx),
    }};

    const struct spi_buf_set rx = {
        .buffers = rx_buf,
        .count = ARRAY_SIZE(rx_buf),
    };

    int result = spi_transceive_dt(&config->spi, NULL, &rx);

    if (result != 0)
    {
        printk("SPI transceive failed\n");
        return 0;
    }

    // Extract 24-bit values and sign-extend to 32-bit
    // ADS1298 outputs 3 bytes of 24-bit signed data in buffer[3:5], [6:8], [9:11], [12:14]
    uint32_t temp;
    
    temp = ((uint32_t)buffer_rx[3] << 16) | ((uint32_t)buffer_rx[4] << 8) | (uint32_t)buffer_rx[5];
    data->adc_data_ch0 = (temp & 0x00800000) ? (int32_t)(temp | 0xFF000000) : (int32_t)temp;
    
    temp = ((uint32_t)buffer_rx[6] << 16) | ((uint32_t)buffer_rx[7] << 8) | (uint32_t)buffer_rx[8];
    data->adc_data_ch1 = (temp & 0x00800000) ? (int32_t)(temp | 0xFF000000) : (int32_t)temp;
    
    temp = ((uint32_t)buffer_rx[9] << 16) | ((uint32_t)buffer_rx[10] << 8) | (uint32_t)buffer_rx[11];
    data->adc_data_ch2 = (temp & 0x00800000) ? (int32_t)(temp | 0xFF000000) : (int32_t)temp;
    
    temp = ((uint32_t)buffer_rx[12] << 16) | ((uint32_t)buffer_rx[13] << 8) | (uint32_t)buffer_rx[14];
    data->adc_data_ch3 = (temp & 0x00800000) ? (int32_t)(temp | 0xFF000000) : (int32_t)temp;

    return 0;
}

static const struct sensor_driver_api ads129xx_api_funcs = {
    .attr_set = ads129xx_attr_set,

    .sample_fetch = ads129xx_sample_fetch,
    .channel_get = ads129xx_channel_get,
    .trigger_set = ads129xx_trigger_set,
    .submit = ads129xx_submit,
};

static int ads129xx_chip_init(const struct device *dev)
{
    const struct ads129xx_config *config = dev->config;
    struct ads129xx_data *data = dev->data;

    int err;

    err = spi_is_ready_dt(&config->spi);
    if (err < 0)
    {
        LOG_ERR("SPI bus not ready: %d", err);
        return err;
    }

    /* Dump SPI1 register state to verify clock and configuration */
    volatile uint32_t *spi1_cr1 = (volatile uint32_t *)0x40013000;
    volatile uint32_t *spi1_cfg1 = (volatile uint32_t *)0x40013008;
    volatile uint32_t *spi1_cfg2 = (volatile uint32_t *)0x4001300C;
    volatile uint32_t *spi1_sr = (volatile uint32_t *)0x40013014;
    LOG_DBG("SPI1 regs: CR1=0x%08X CFG1=0x%08X CFG2=0x%08X SR=0x%08X",
            *spi1_cr1, *spi1_cfg1, *spi1_cfg2, *spi1_sr);

    /* Dump GPIOA MODER to verify pin alternate function config */
    volatile uint32_t *gpioa_moder = (volatile uint32_t *)0x58020000;
    volatile uint32_t *gpioa_afrl = (volatile uint32_t *)0x58020020;
    LOG_DBG("GPIOA MODER=0x%08X AFRL=0x%08X (PA4-7 should be AF5)",
            *gpioa_moder, *gpioa_afrl);

    if (!device_is_ready(config->pwdn_pin.port))
    {
        LOG_ERR("PWDN GPIO device %s is not ready", config->pwdn_pin.port->name);
        return -ENODEV;
    }

    /* Configure GPIOs with explicit safe initial states:
     * PWDN: HIGH = powered up (active LOW pin, HIGH = normal operation)
     * RESET: HIGH = not in reset (active LOW pin, HIGH = normal)
     * START: LOW = conversions stopped
     * This prevents undefined pin states during configuration that
     * could glitch the ADS1294R into power-down or reset.
     */
    err = gpio_pin_configure_dt(&config->pwdn_pin, GPIO_OUTPUT_HIGH);
    if (err) { LOG_ERR("PWDN config failed: %d", err); return err; }

    err = gpio_pin_configure_dt(&config->reset_pin, GPIO_OUTPUT_HIGH);
    if (err) { LOG_ERR("RESET config failed: %d", err); return err; }

    err = gpio_pin_configure_dt(&config->start_pin, GPIO_OUTPUT_LOW);
    if (err) { LOG_ERR("START config failed: %d", err); return err; }

    err = gpio_pin_configure_dt(&config->drdy_pin, GPIO_INPUT);
    if (err) { LOG_ERR("DRDY config failed: %d", err); return err; }

    LOG_DBG("ADS129XX GPIO: PWDN=%s pin %d, RESET=%s pin %d, "
            "DRDY=%s pin %d, START=%s pin %d",
            config->pwdn_pin.port->name, config->pwdn_pin.pin,
            config->reset_pin.port->name, config->reset_pin.pin,
            config->drdy_pin.port->name, config->drdy_pin.pin,
            config->start_pin.port->name, config->start_pin.pin);

    /* Power-up sequence: ensure PWDN is HIGH, then wait for power stabilization */
    gpio_pin_set(config->pwdn_pin.port, config->pwdn_pin.pin, 1);
    k_sleep(K_MSEC(100));

    /* Check PWDN and RESET pin readback to verify they're driving correctly */
    int pwdn_val = gpio_pin_get(config->pwdn_pin.port, config->pwdn_pin.pin);
    int reset_val = gpio_pin_get(config->reset_pin.port, config->reset_pin.pin);
    int start_val = gpio_pin_get(config->start_pin.port, config->start_pin.pin);
    int drdy_val = gpio_pin_get(config->drdy_pin.port, config->drdy_pin.pin);
    LOG_DBG("GPIO state before reset: PWDN=%d RESET=%d START=%d DRDY=%d",
            pwdn_val, reset_val, start_val, drdy_val);

    /* Reset sequence: pulse RESET low then release */
    gpio_pin_set(config->reset_pin.port, config->reset_pin.pin, 0);
    k_sleep(K_MSEC(1));
    gpio_pin_set(config->reset_pin.port, config->reset_pin.pin, 1);
    k_sleep(K_MSEC(500));

    drdy_val = gpio_pin_get(config->drdy_pin.port, config->drdy_pin.pin);
    LOG_DBG("After reset: DRDY=%d", drdy_val);

    /* ADS1294R needs START=HIGH to begin conversions.
     * In RDATAC mode (default after reset), DOUT only outputs data
     * when conversions are active. Pulse START to trigger a conversion,
     * then check if DOUT comes alive. */
    gpio_pin_set(config->start_pin.port, config->start_pin.pin, 1);
    k_sleep(K_MSEC(100));

    drdy_val = gpio_pin_get(config->drdy_pin.port, config->drdy_pin.pin);
    LOG_DBG("After START=HIGH (100ms): DRDY=%d", drdy_val);

    /* Probe RDATAC output - with START=HIGH and conversions running,
     * the chip should be outputting data on DOUT */
    {
        uint8_t probe_tx[16] = {0};
        uint8_t probe_rx[16] = {0};
        const struct spi_buf tx_b = {.buf = probe_tx, .len = 16};
        const struct spi_buf rx_b = {.buf = probe_rx, .len = 16};
        const struct spi_buf_set tx_s = {.buffers = &tx_b, .count = 1};
        const struct spi_buf_set rx_s = {.buffers = &rx_b, .count = 1};
        spi_transceive_dt(&config->spi, &tx_s, &rx_s);
        LOG_DBG("RDATAC probe (START=1): %02X %02X %02X %02X %02X %02X %02X %02X "
                "%02X %02X %02X %02X %02X %02X %02X %02X",
                probe_rx[0], probe_rx[1], probe_rx[2], probe_rx[3],
                probe_rx[4], probe_rx[5], probe_rx[6], probe_rx[7],
                probe_rx[8], probe_rx[9], probe_rx[10], probe_rx[11],
                probe_rx[12], probe_rx[13], probe_rx[14], probe_rx[15]);
    }

    /* Stop conversions and exit RDATAC for register access */
    gpio_pin_set(config->start_pin.port, config->start_pin.pin, 0);
    k_sleep(K_MSEC(10));

    _ads129xx_send_command(dev, ADS129XX_CMD_SDATAC);
    k_sleep(K_MSEC(50));

    // Use internal ref
    _ads129xx_reg_write(dev, ADS129XX_REG_CONFIG3, 0b11100000);
    k_sleep(K_MSEC(100));

    /* Read registers after SDATAC */
    LOG_DBG("Register dump: ID=0x%02X CFG1=0x%02X CFG2=0x%02X CFG3=0x%02X",
            _ads129xx_read_reg(dev, 0x00),
            _ads129xx_read_reg(dev, 0x01),
            _ads129xx_read_reg(dev, 0x02),
            _ads129xx_read_reg(dev, 0x03));

    data->number_channels = 4;

    err = ads129xx_read_id(dev);
    if (err) {
        return err;
    }

    // Set the device to normal mode
    _ads129xx_reg_write(dev, ADS129XX_REG_CONFIG1, 0b01010110); // HR mode 500 SPS
    k_sleep(K_MSEC(100));
    _ads129xx_reg_write(dev, ADS129XX_REG_CONFIG2, 0b00000000); // Test signals disabled, normal ECG operation
    k_sleep(K_MSEC(100));
    _ads129xx_reg_write(dev, ADS129XX_REG_CONFIG3, 0b11101100); // Ref buf enabled, ref 2.4V
    k_sleep(K_MSEC(100));

    /* LOFF register (0x04) = 0xC4:
     * Bits 7-5: COMP_TH   = 110 (75%/25% comparator threshold)
     * Bit 4:    VLEAD_OFF_EN = 0 (current source mode, not pull-up/pull-down)
     * Bits 3-2: ILEAD_OFF = 01 (12 nA)
     * Bits 1-0: FLEAD_OFF = 00 (DC lead-off). AC lead-off (01) remains the
     *           fallback if DC bias proves troublesome, at the cost of a
     *           carrier inside the ECG passband.
     *
     * WAS 0x00 (95%/5%, 6 nA), and at that setting the POSITIVE side never
     * detected anything. Measured on v5 hardware 2026-08-31 with all electrodes
     * disconnected -- the easiest possible case, where all four should report
     * off -- LOFF_STATP read 0x00 while LOFF_STATN read 0x06. Only RA was ever
     * seen; LA, LL and V1 lead-off were dead.
     *
     * It is not a digital fault. Register read-back on the bench confirmed
     * LOFF_SENSP=0x0E and CONFIG4=0x02 (comparators enabled), and the STATUS
     * word decodes with the correct 0b1100 header, so selection, comparator
     * power and the decode were all already right. The fault is analog: at 6 nA
     * a floating input cannot be dragged past 95% of the supply against
     * whatever DC path the board presents, so the comparator never trips. The
     * numbers are unforgiving -- 6 nA into 1 Mohm is 6 mV.
     *
     * Threshold sweep with all electrodes open (100 % of samples unless noted):
     *   95%/5%,  6 nA  ->  STATP 0x00   nothing
     *   95%/5%, 24 nA  ->  STATP 0x00   nothing
     *   85%/15%,12 nA  ->  STATP 0x07   LA+LL but only 49 % stable (chatters)
     *   80%/20%,12 nA  ->  STATP 0x03   LA only, 50 % stable (chatters)
     *   75%/25%,12 nA  ->  STATP 0x07   LA+LL, 100 % stable   <-- chosen
     *   70%/30%,12 nA  ->  STATP 0x07   LA+LL, 99 % stable
     * 75%/25% is the most conservative threshold that is still rock stable:
     * more headroom against a false lead-off on a connected electrode (which
     * sits near mid-supply, 25 points from either rail) than 70%/30%, and
     * unlike 80%/20% it does not sit on the edge and chatter. 12 nA is the
     * lowest current that works, keeping injected DC to a minimum.
     *
     * STILL BROKEN: V1 (CH4P) never asserts at ANY threshold, current, or mode
     * -- including pull-up/pull-down and with the WCT buffers disabled. RA, LA
     * and LL now work; V1 does not. That is a board-level question and needs a
     * scope, not another register. See
     * fw-internal-docs/algorithms/LEAD_OFF_P_SIDE_DEAD.md.
     *
     * NOT YET VALIDATED ON A BODY. Lowering the threshold trades margin against
     * false lead-off during motion or a drying gel pad. That trade has only
     * been measured with open inputs; it must be checked on a subject before
     * this ships.
     */
    _ads129xx_reg_write(dev, ADS129XX_REG_LOFF, 0xC4);
  //  _ads129xx_reg_write(dev, ADS129XX_REG_LOFF, 0x00);
    k_sleep(K_MSEC(10));

    /* Select WHICH electrodes the lead-off comparators watch (LOFF_SENSP 0x0F /
     * LOFF_SENSN 0x10). Per the canonical map: LA=IN2P, LL=IN3P, V1=IN4P,
     * RA=IN2N(+IN3N).
     *   LOFF_SENSP = 0b0000_1110 = 0x0E  (CH2P=LA, CH3P=LL, CH4P=V1)
     *   LOFF_SENSN = 0b0000_0010 = 0x02  (CH2N=RA only)
     *
     * CH1 is deliberately excluded from BOTH: it is the respiration channel and
     * drives its own excitation current through IN1P/IN1N, so a lead-off current
     * source there would fight the resp modulator. RA is selected ONCE even
     * though it lands on IN2N and IN3N; selecting both would put two current
     * sources on one electrode.
     */
    _ads129xx_reg_write(dev, ADS129XX_REG_LOFF_SENSP, 0x0E);
    _ads129xx_reg_write(dev, ADS129XX_REG_LOFF_SENSN, 0x02);
    // _ads129xx_reg_write(dev, ADS129XX_REG_LOFF_SENSP, 0x00);
    // _ads129xx_reg_write(dev, ADS129XX_REG_LOFF_SENSN, 0x00);
    k_sleep(K_MSEC(10));

    /* ===================================================================
     * CANONICAL ELECTRODE MAP - HealthyPi 6, 5-electrode cable
     * (LA, RA, LL, RL, V1). Single source of truth: the CHnSET, RLD and
     * WCT blocks below all derive from THIS table. ADS1294R = 4 channels;
     * CH1 is respiration, so CH2/CH3/CH4 acquire Lead I, Lead II and V1
     * directly and III/aVR/aVL/aVF are derived in software (app_m7
     * lead-derivation helper).
     *
     *   Ch (1-idx) | Reg     | IN+        | IN-        | Acquires
     *   -----------+---------+------------+------------+-----------------
     *   CH1        | CH1SET  | IN1P resp  | IN1N resp  | Respiration (Z)
     *   CH2        | CH2SET  | IN2P = LA  | IN2N = RA  | Lead I  = LA-RA
     *   CH3        | CH3SET  | IN3P = LL  | IN3N = RA  | Lead II = LL-RA
     *   CH4        | CH4SET  | IN4P = V1  | IN4N = WCT | V1 (precordial)
     *
     *   Shared nodes: RA -> IN2N AND IN3N (common limb reference)
     *                 RL -> RLD drive output (bias, not digitized)
     *                 WCT = (RA+LA+LL)/3, generated on-chip (see WCT blk)
     *
     *   Downstream (data_module): data_ch0=Resp, data_ch1=Lead I,
     *   data_ch2=Lead II, data_ch3=V1 (NOT Lead III - Lead III is
     *   computed = II - I).
     * ===================================================================
     *
     * CHnSET bits: PDn[7]=0 (powered on), GAINn[6:4], reserved[3]=0,
     * MUXn[2:0]=000 (normal electrode input).
     */
    uint8_t gain_bits = ads129xx_gain_to_bits(config->gain);

    /* Channel 1 (Respiration): Use Gain=4 (0x40) for better respiration signal per TI SBAA181
     * Higher gain can cause clipping, lower gain reduces signal amplitude
     */
    _ads129xx_reg_write(dev, ADS129XX_REG_CHnSET + 0, 0x40);  // CH1: Respiration, Gain=4, normal input
    _ads129xx_reg_write(dev, ADS129XX_REG_CHnSET + 1, gain_bits | 0b00000000);  // CH2: Lead I  (IN2P=LA, IN2N=RA)
    _ads129xx_reg_write(dev, ADS129XX_REG_CHnSET + 2, gain_bits | 0b00000000);  // CH3: Lead II (IN3P=LL, IN3N=RA)
    _ads129xx_reg_write(dev, ADS129XX_REG_CHnSET + 3, gain_bits | 0b00000000);  // CH4: V1     (IN4P=V1, IN4N=WCT)

    /* RLD (Right Leg Drive) - common-mode feedback to the RL electrode,
     * averaged over the limb electrodes only (RA, LA, LL). Exclude CH1
     * (respiration) and CH4 (including V1 would inject chest noise into
     * the bias loop). Bit b = RLDnP/N for datasheet channel n+1.
     *
     * RLD_SENSP (0x0D) = 0x06: RLD2P (LA, IN2P) + RLD3P (LL, IN3P)
     * RLD_SENSN (0x0E) = 0x02: RLD2N (RA, IN2N) only - RA is also on IN3N
     *   but must be counted once
     */
    // _ads129xx_reg_write(dev, ADS129XX_REG_RLD_SENSP, 0x06); // RLD+ from LA(IN2P), LL(IN3P)
    // _ads129xx_reg_write(dev, ADS129XX_REG_RLD_SENSN, 0x02); // RLD- from RA(IN2N)
    _ads129xx_reg_write(dev, ADS129XX_REG_RLD_SENSP, 0x0E); // RLD+ from LA(IN2P), LL(IN3P)
    _ads129xx_reg_write(dev, ADS129XX_REG_RLD_SENSN, 0x0E); // RLD- from RA(IN2N)

    /* WCT (Wilson Central Terminal) - ADS1294R
     *
     * WCT = (WCTA + WCTB + WCTC)/3, used as IN4N reference so CH4 reads V1.
     * Datasheet convention (SBAS459K Tables 34/35): WCTA->RA, WCTB->LA,
     * WCTC->LL. Per the canonical map RA=IN2N, LA=IN2P, LL=IN3P, so:
     *   WCTA = RA = Ch2 negative input (IN2N) -> code 011
     *   WCTB = LA = Ch2 positive input (IN2P) -> code 010
     *   WCTC = LL = Ch3 positive input (IN3P) -> code 100
     * WCTx[2:0] mux (verified, SBAS459K): 000=Ch1+ 001=Ch1- 010=Ch2+
     *   011=Ch2- 100=Ch3+ 101=Ch3- 110=Ch4+ 111=Ch4-.
     *
     * PD_WCTx polarity is INVERTED (verified, SBAS459K): 0 = powered DOWN
     * (reset), 1 = powered ON. All three must be 1 or no WCT is generated
     * and CH4(V1) floats.
     *
     * WCT1 (0x18) = aVx[7:4]=0 | PD_WCTA(b3)=1 | WCTA[2:0]=011(IN2N=RA)
     *             = 0b0000_1011 = 0x0B
     * WCT2 (0x19) = PD_WCTC(b7)=1 | PD_WCTB(b6)=1 | WCTB[5:3]=010(IN2P=LA)
     *             | WCTC[2:0]=100(IN3P=LL) = 0b1101_0100 = 0xD4
     * NOTE WCT2 layout is PD_WCTC[7] PD_WCTB[6] WCTB[5:3] WCTC[2:0] - NOT
     * WCTC[6:4]/WCTB[2:0].
     */
    // _ads129xx_reg_write(dev, ADS129XX_REG_WCT1, 0x0B); // PD_WCTA on; WCTA=RA(IN2N)
    // _ads129xx_reg_write(dev, ADS129XX_REG_WCT2, 0xD4); // PD_WCTC+PD_WCTB on; WCTB=LA(IN2P), WCTC=LL(IN3P)

    _ads129xx_reg_write(dev, ADS129XX_REG_WCT1, 0x01); // PD_WCTA on; WCTA=RA(IN2N)
    _ads129xx_reg_write(dev, ADS129XX_REG_WCT2, 0x32); // PD_WCTC+PD_WCTB on; WCTB=LA(IN2P), WCTC=LL(IN3P)

    /* Respiration detection (ADS1294R): circuit on Channel 1 (IN1P/IN1N),
     * RESPMODP/RESPMODN via RC network per ADS1292R Figure 97; internal
     * modulation and demodulation.
     *
     * RESP register (0x16) = 0xF2:
     *   Bit 7: RESP_DEMOD_EN1 = 1 (demodulation on Channel 1)
     *   Bit 6: RESP_MOD_EN1 = 1 (modulation - drives AC excitation current)
     *   Bit 5: Reserved = 1 (MUST be 1 for ADS1294R/6R/8R per datasheet)
     *   Bits 4-2: RESP_PH[2:0] = 100 (112.5 deg phase at 32 kHz per TI
     *             SBAA181 - reduces gain error from demodulation glitches)
     *   Bits 1-0: RESP_CTRL = 10 (internal respiration, internal signals)
     */
    _ads129xx_reg_write(dev, ADS129XX_REG_RESP, 0xF2);
    k_sleep(K_MSEC(10));

    /* CONFIG4 register (0x17) = 0x02:
     *   Bit 0: RESP_FREQ = 0 (32 kHz modulation clock - lower noise)
     *   Bit 1: PD_LOFF_COMP = 1 -> lead-off comparators POWERED ON.
     *          The name reads like a power-DOWN bit but the polarity is
     *          inverted, exactly like PD_WCTx: 0 = powered down (reset),
     *          1 = enabled.
     *   Other bits: default 0
     */
   // _ads129xx_reg_write(dev, ADS129XX_REG_CONFIG4, PD_LOFF_COMP);
    _ads129xx_reg_write(dev, ADS129XX_REG_CONFIG4, 0x00);
    k_sleep(K_MSEC(10));

    /* Read back and LOG the respiration + lead-off config: the lead-off path
     * is invisible in normal operation, so this line is the bench evidence
     * that the comparators are actually enabled. */
    uint8_t resp_reg = _ads129xx_read_reg(dev, ADS129XX_REG_RESP);
    uint8_t config4_reg = _ads129xx_read_reg(dev, ADS129XX_REG_CONFIG4);
    uint8_t loff_reg = _ads129xx_read_reg(dev, ADS129XX_REG_LOFF);
    uint8_t loff_p = _ads129xx_read_reg(dev, ADS129XX_REG_LOFF_SENSP);
    uint8_t loff_n = _ads129xx_read_reg(dev, ADS129XX_REG_LOFF_SENSN);

    LOG_INF("ads129xx: RESP=0x%02x CONFIG4=0x%02x (loff comp %s) "
            "LOFF=0x%02x SENSP=0x%02x SENSN=0x%02x",
            resp_reg, config4_reg,
            (config4_reg & PD_LOFF_COMP) ? "on" : "OFF",
            loff_reg, loff_p, loff_n);

    k_sleep(K_MSEC(1));

    // Read data continuously
    _ads129xx_send_command(dev, ADS129XX_CMD_RDATAC);
    k_sleep(K_MSEC(1));

    ads129xx_init_interrupt(dev);

    LOG_DBG("\"%s\" OK", dev->name);
    return 0;
}

#define ADS129XX_SPI_OPERATION (SPI_WORD_SET(8) | SPI_TRANSFER_MSB | SPI_MODE_CPHA)

/*
 * Main instantiation macro, which selects the correct bus-specific
 * instantiation macros for the instance.
 */

#define ADS129XX_DEFINE(inst)                                              \
    static struct ads129xx_data ads129xx_data_##inst;                      \
    static const struct ads129xx_config ads129xx_config_##inst =           \
        {                                                                  \
            .spi = SPI_DT_SPEC_INST_GET(                                   \
                inst, ADS129XX_SPI_OPERATION, 2),                          \
            .pwdn_pin = GPIO_DT_SPEC_INST_GET_OR(inst, pwdn_gpios, {0}),   \
            .reset_pin = GPIO_DT_SPEC_INST_GET_OR(inst, reset_gpios, {0}), \
            .start_pin = GPIO_DT_SPEC_INST_GET_OR(inst, start_gpios, {0}), \
            .drdy_pin = GPIO_DT_SPEC_INST_GET_OR(inst, drdy_gpios, {0}),   \
            .gain = DT_INST_PROP(inst, gain),                              \
                                                                           \
    };                                                                     \
    PM_DEVICE_DT_INST_DEFINE(inst, ads129xx_pm_action);                    \                                                                           
    SENSOR_DEVICE_DT_INST_DEFINE(inst,                                     \
                                 ads129xx_chip_init,                       \
                                 PM_DEVICE_DT_INST_GET(inst),              \
                                 &ads129xx_data_##inst,                    \
                                 &ads129xx_config_##inst,                  \
                                 POST_KERNEL,                              \
                                 CONFIG_SENSOR_INIT_PRIORITY,              \
                                 &ads129xx_api_funcs);

DT_INST_FOREACH_STATUS_OKAY(ADS129XX_DEFINE)