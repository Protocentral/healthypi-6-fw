// ProtoCentral Electronics (info@protocentral.com)
// SPDX-License-Identifier: Apache-2.0

#include <zephyr/drivers/sensor.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(ads129xx_async, LOG_LEVEL_DBG);

#include "ads129xx.h"

#define DT_DRV_COMPAT ti_ads129xx

static int ads129xx_async_read(const struct device *dev, uint32_t *data_stat, int32_t *data_ch0,
                               int32_t *data_ch1, int32_t *data_ch2, int32_t *data_ch3)
{
    struct ads129xx_data *data = dev->data;
    const struct ads129xx_config *config = dev->config;

    uint8_t buf_ecg[(data->number_channels + 1) * 3];

    const struct spi_buf rx_buf[] = {{
        .buf = buf_ecg,
        .len = ARRAY_SIZE(buf_ecg),
    }};

    const struct spi_buf_set rx = {
        .buffers = rx_buf,
        .count = ARRAY_SIZE(rx_buf),
    };

    int ret = spi_transceive_dt(&config->spi, NULL, &rx);
    if (ret < 0)
    {
        LOG_ERR("SPI transceive failed: %d", ret);
        return ret;
    }

    // Decode buf_ecg into int32 values with proper sign extension
    // ADS1298 frame: [STATUS(3 bytes), CH0(3 bytes), CH1(3 bytes), CH2(3 bytes), CH3(3 bytes)]
    // Each 24-bit value should be sign-extended to 32-bit
    *data_stat = (uint32_t)((buf_ecg[0] << 16) | (buf_ecg[1] << 8) | buf_ecg[2]);
    
    // Extract 24-bit channels - big-endian format (MSB first)
    uint32_t temp;
    
    temp = ((uint32_t)buf_ecg[3] << 16) | ((uint32_t)buf_ecg[4] << 8) | (uint32_t)buf_ecg[5];
    *data_ch0 = (temp & 0x00800000) ? (int32_t)(temp | 0xFF000000) : (int32_t)temp;
    
    temp = ((uint32_t)buf_ecg[6] << 16) | ((uint32_t)buf_ecg[7] << 8) | (uint32_t)buf_ecg[8];
    *data_ch1 = (temp & 0x00800000) ? (int32_t)(temp | 0xFF000000) : (int32_t)temp;
    
    temp = ((uint32_t)buf_ecg[9] << 16) | ((uint32_t)buf_ecg[10] << 8) | (uint32_t)buf_ecg[11];
    *data_ch2 = (temp & 0x00800000) ? (int32_t)(temp | 0xFF000000) : (int32_t)temp;
    
    temp = ((uint32_t)buf_ecg[12] << 16) | ((uint32_t)buf_ecg[13] << 8) | (uint32_t)buf_ecg[14];
    *data_ch3 = (temp & 0x00800000) ? (int32_t)(temp | 0xFF000000) : (int32_t)temp;

    return 0;
}

void ads129xx_submit(const struct device *dev, struct rtio_iodev_sqe *iodev_sqe)
{
    uint32_t m_min_buf_len = sizeof(struct ads129xx_encoded_data);

    uint8_t *buf;
    uint32_t buf_len;

    struct ads129xx_encoded_data *m_edata;

    int ret = 0;

    ret = rtio_sqe_rx_buf(iodev_sqe, m_min_buf_len, m_min_buf_len, &buf, &buf_len);
    if (ret < 0)
    {
        LOG_ERR("Failed to get RX buffer: %d of size: %d", ret, m_min_buf_len);
        rtio_iodev_sqe_err(iodev_sqe, ret);
        return;
    }

    m_edata = (struct ads129xx_encoded_data *)buf;
    m_edata->header.timestamp = k_ticks_to_ns_floor64(k_uptime_ticks());
    ret = ads129xx_async_read(dev, &m_edata->data_stat, &m_edata->data_ch0,
                              &m_edata->data_ch1, &m_edata->data_ch2, &m_edata->data_ch3);

    if (ret != 0)
    {
        LOG_ERR("ads129xx_async_read failed: %d", ret);
        rtio_iodev_sqe_err(iodev_sqe, ret);
        return;
    }

    rtio_iodev_sqe_ok(iodev_sqe, 0);
}


int ads129xx_get_decoder(const struct device *dev, const struct sensor_decoder_api **decoder)
{
	ARG_UNUSED(dev);
	*decoder = &SENSOR_DECODER_NAME();

	return 0;
}