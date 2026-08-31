// ProtoCentral Electronics (info@protocentral.com)
// SPDX-License-Identifier: Apache-2.0

#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(afe4400_async, LOG_LEVEL_WRN);  /* Reduced to WARNING level */

#include "afe4400.h"
#include "afe4400_regs.h"

#define DT_DRV_COMPAT ti_afe4400

static int afe4400_async_read(const struct device *dev, uint32_t *led1_val, uint32_t *amb1_val,
                              uint32_t *led2_val, uint32_t *amb2_val, uint32_t *diag_val)
{
    /* CRITICAL DEBUG: Log CONTROL0 write to verify it's happening */
    static uint32_t read_count = 0;
    read_count++;
    if (read_count % 100 == 0) {
        LOG_INF("AFE4400 CONTROL0 write #%u: triggering data transfer", read_count);
    }

    /* AFE4400 requires writing 0x000001 to CONTROL0 before reading data registers
     * This triggers the data transfer from internal registers to SPI-readable registers
     * Per AFE4400 datasheet and HealthyPi 5 implementation */
    afe4400_reg_write_blocking(dev, AFE4400_REG_CONTROL0, 0x000001);
    
    /* Read AFE4400 data registers (blocking, returns 24-bit values) */
    *led1_val = afe4400_reg_read_blocking(dev, AFE4400_REG_LED1VAL);
    *amb1_val = afe4400_reg_read_blocking(dev, AFE4400_REG_ALED1VAL);
    *led2_val = afe4400_reg_read_blocking(dev, AFE4400_REG_LED2VAL);
    *amb2_val = afe4400_reg_read_blocking(dev, AFE4400_REG_ALED2VAL);
    *diag_val = afe4400_reg_read_blocking(dev, AFE4400_REG_DIAG);

    /* CRITICAL DEBUG: Log raw AFE4400 register values to verify data is changing */
    static uint32_t last_led1 = 0, last_led2 = 0;
    static int sample_count = 0;
    sample_count++;
    if (sample_count % 100 == 0) {  // Log every 100th sample to avoid spam
        LOG_INF("AFE4400 raw[#%d]: LED1(Red)=%u (Δ%d), LED2(IR)=%u (Δ%d)", 
                sample_count, *led1_val, (int32_t)(*led1_val - last_led1),
                *led2_val, (int32_t)(*led2_val - last_led2));
        last_led1 = *led1_val;
        last_led2 = *led2_val;
    }

    return 0;
}

void afe4400_submit(const struct device *dev, struct rtio_iodev_sqe *iodev_sqe)
{
    uint32_t min_buf_len = sizeof(struct afe4400_encoded_data);
    uint8_t *buf;
    uint32_t buf_len;
    struct afe4400_encoded_data *edata;
    int ret = 0;

    ret = rtio_sqe_rx_buf(iodev_sqe, min_buf_len, min_buf_len, &buf, &buf_len);
    if (ret < 0) {
        LOG_ERR("Failed to get RX buffer: %d of size: %d", ret, min_buf_len);
        rtio_iodev_sqe_err(iodev_sqe, ret);
        return;
    }

    edata = (struct afe4400_encoded_data *)buf;
    edata->header.timestamp = k_ticks_to_ns_floor64(k_uptime_ticks());
    
    ret = afe4400_async_read(dev, &edata->led1_val, &edata->amb1_val,
                            &edata->led2_val, &edata->amb2_val, &edata->diag_val);

    if (ret != 0) {
        LOG_ERR("afe4400_async_read failed: %d", ret);
        rtio_iodev_sqe_err(iodev_sqe, ret);
        return;
    }

    rtio_iodev_sqe_ok(iodev_sqe, 0);
}
