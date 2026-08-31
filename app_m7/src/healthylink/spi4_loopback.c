/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * SPI4 loopback self-test -- HealthyLink slot-A bus bring-up diagnostic.
 *
 * Jumper SPI4 MOSI (PE6) to MISO (PE5); one transceive must echo its TX bytes
 * back on RX. No chip-select is used (no slave), so the soft-NSS CS path is
 * out of the picture. PASS -> controller/clock/pins work (fault is in the
 * N657/CS/soft-NSS path). FAIL (rx != tx) -> clock-source/prescaler (SPI45
 * mux) or signal integrity. Reset / no result line -> the transceive itself
 * faults (SPI4 kernel clock, soft-NSS driver path, or the H7 errata class).
 *
 * Gated behind CONFIG_HPI_SPI4_LOOPBACK_TEST (default n) and run on a
 * dedicated work queue so a *hang* cannot starve the IWDG. A genuine CPU
 * exception still resets -- run with a debugger/RTT attached so the fault
 * dump survives.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(spi4_lb, CONFIG_HPI_APP_LOG_LEVEL);

#if IS_ENABLED(CONFIG_HPI_SPI4_LOOPBACK_TEST) && \
    DT_NODE_HAS_STATUS(DT_NODELABEL(spi4), okay)

static const struct device *const spi4 = DEVICE_DT_GET(DT_NODELABEL(spi4));

/* Conservative: 1 MHz, 8-bit, mode 0, MSB-first, master, no CS. */
static const struct spi_config lb_cfg = {
    .frequency = 1000000,
    .operation = SPI_WORD_SET(8) | SPI_TRANSFER_MSB | SPI_OP_MODE_MASTER,
    .slave = 0,
};

K_THREAD_STACK_DEFINE(spi4_lb_stack, 3072);
static struct k_work_q spi4_lb_wq;
static struct k_work spi4_lb_work;

static void spi4_lb_run(struct k_work *w)
{
    ARG_UNUSED(w);

    if (!device_is_ready(spi4)) {
        LOG_ERR("SPI4 loopback: controller not ready");
        return;
    }

    const uint8_t tx[4] = { 0xA5, 0x5A, 0xF0, 0x0F };
    uint8_t rx[4] = { 0 };
    struct spi_buf txb = { .buf = (void *)tx, .len = sizeof(tx) };
    struct spi_buf rxb = { .buf = rx, .len = sizeof(rx) };
    struct spi_buf_set txs = { .buffers = &txb, .count = 1 };
    struct spi_buf_set rxs = { .buffers = &rxb, .count = 1 };

    LOG_INF("SPI4 loopback: tie PE6(MOSI)->PE5(MISO); transceiving 4 bytes @1MHz");
    int rc = spi_transceive(spi4, &lb_cfg, &txs, &rxs);  /* <-- fault lands here if it does */

    LOG_INF("SPI4 loopback: rc=%d  tx=%02x %02x %02x %02x  rx=%02x %02x %02x %02x",
            rc, tx[0], tx[1], tx[2], tx[3], rx[0], rx[1], rx[2], rx[3]);

    if (rc != 0) {
        LOG_ERR("SPI4 loopback: FAIL (transceive rc=%d) -- bus error path", rc);
        return;
    }
    if (memcmp(tx, rx, sizeof(tx)) == 0) {
        LOG_INF("SPI4 loopback: PASS -- bus moves bytes; fault is in N657/CS path");
    } else {
        LOG_WRN("SPI4 loopback: FAIL -- rc=0 but rx!=tx (clock-source/prescaler or "
                "wiring); check SPI45 mux + jumper");
    }
}

static int spi4_lb_init(void)
{
    k_work_queue_init(&spi4_lb_wq);
    k_work_queue_start(&spi4_lb_wq, spi4_lb_stack,
                       K_THREAD_STACK_SIZEOF(spi4_lb_stack),
                       K_LOWEST_APPLICATION_THREAD_PRIO, NULL);
    k_thread_name_set(&spi4_lb_wq.thread, "spi4_lb");
    k_work_init(&spi4_lb_work, spi4_lb_run);
    k_work_submit_to_queue(&spi4_lb_wq, &spi4_lb_work);
    return 0;
}

/* Run late, after drivers + main bring-up so the watchdog loop is already up. */
SYS_INIT(spi4_lb_init, APPLICATION, 99);

#endif /* CONFIG_HPI_SPI4_LOOPBACK_TEST && spi4 okay */
