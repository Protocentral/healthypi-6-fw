/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Second MCUmgr SMP transport, bound to UART4 (the ESP32-C6 gateway line),
 * alongside the stock USB CDC1 transport (zephyr,uart-mcumgr). It lets a WiFi
 * SMP client reach the device's MCUmgr server -- including the stock `img`
 * group for OTA -- via the ESP32's TCP<->UART passthrough.
 *
 * Custom because Zephyr's uart_mcumgr driver and smp_uart transport are
 * singletons bound to the single `zephyr,uart-mcumgr` chosen node (= CDC1
 * here). This file re-implements both halves against UART4: interrupt-driven
 * newline-framed RX -> mcumgr_serial_process_frag() -> smp_rx_req(), and TX
 * via mcumgr_serial_tx_pkt() over uart_poll_out. The mcumgr_serial_* /
 * smp_transport_* helpers are generic, so both transports coexist. Logic
 * adapted from upstream Zephyr (Apache-2.0).
 *
 * Gated by CONFIG_HPI_WIRELESS_OTA (default n). When off this file is empty
 * and neither UART4 nor the CDC1 SMP transport is affected.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>

#if IS_ENABLED(CONFIG_HPI_WIRELESS_OTA) && DT_NODE_HAS_STATUS(DT_NODELABEL(uart4), okay)

/* Belt-and-braces for a forced =y that bypasses the Kconfig dependency: UART4
 * carries the HealthyBridge host link, and a second consumer would displace its
 * callback and receive path without either side noticing. */
BUILD_ASSERT(!DT_NODE_HAS_STATUS(DT_NODELABEL(healthybridge_esp32_uart), okay),
	     "CONFIG_HPI_WIRELESS_OTA binds MCUmgr SMP to UART4, which the "
	     "HealthyBridge host link already owns. Carry SMP as a HealthyBridge "
	     "frame type instead (migration plan P6), or disable one of them.");

#include <string.h>
#include <zephyr/init.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/net_buf.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zephyr/mgmt/mcumgr/transport/smp.h>
#include <zephyr/mgmt/mcumgr/transport/serial.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(hpi_ota_uart, CONFIG_MCUMGR_TRANSPORT_LOG_LEVEL);

#define OTA_UART_NODE DT_NODELABEL(uart4)
static const struct device *const ota_uart = DEVICE_DT_GET(OTA_UART_NODE);

/* One received newline-framed fragment. First word reserved for k_fifo. */
struct ota_rx_buf {
	void *fifo_reserved;
	uint16_t length;
	uint8_t data[CONFIG_UART_MCUMGR_RX_BUF_SIZE];
};

K_MEM_SLAB_DEFINE(ota_rx_slab, sizeof(struct ota_rx_buf),
		  CONFIG_UART_MCUMGR_RX_BUF_COUNT, sizeof(void *));

static struct ota_rx_buf *ota_cur_buf;   /* fragment being filled (ISR only) */
static bool ota_ignoring;                /* drop the rest of an oversized line */

static struct mcumgr_serial_rx_ctxt ota_rx_ctxt;
static struct smp_transport ota_transport;

static void ota_process_rx_queue(struct k_work *work);
K_FIFO_DEFINE(ota_rx_fifo);
K_WORK_DEFINE(ota_rx_work, ota_process_rx_queue);

#if IS_ENABLED(CONFIG_HPI_CONN_DEBUG)
/* Wire-level RX byte counter: ANY bytes arriving from the ESP32 on UART4 bump
 * this, even malformed (non-SMP) ones -- so it proves the ESP32->M7 UART wire
 * independently of SMP/MCUmgr correctness. Logged every 2 s. */
static volatile uint32_t ota_rx_total;
static uint32_t ota_rx_last;
static void ota_dbg_work_fn(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(ota_dbg_work, ota_dbg_work_fn);
static void ota_dbg_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	uint32_t now = ota_rx_total;

	LOG_INF("UART4 RX: %u bytes total (+%u in 2s)", now, now - ota_rx_last);
	ota_rx_last = now;
	k_work_reschedule(&ota_dbg_work, K_SECONDS(2));
}
#endif

static struct ota_rx_buf *ota_alloc_rx_buf(void)
{
	void *block;

	if (k_mem_slab_alloc(&ota_rx_slab, &block, K_NO_WAIT) != 0) {
		return NULL;
	}
	struct ota_rx_buf *b = block;

	b->length = 0;
	return b;
}

static void ota_free_rx_buf(struct ota_rx_buf *b)
{
	k_mem_slab_free(&ota_rx_slab, (void *)b);
}

/* ISR context: accumulate one '\n'-terminated fragment; return it when done. */
static struct ota_rx_buf *ota_rx_byte(uint8_t byte)
{
	if (!ota_ignoring && ota_cur_buf == NULL) {
		ota_cur_buf = ota_alloc_rx_buf();
		if (ota_cur_buf == NULL) {
			LOG_WRN("no rx buffer, fragment dropped");
			ota_ignoring = true;
		}
	}

	struct ota_rx_buf *b = ota_cur_buf;

	if (!ota_ignoring) {
		if (b->length >= sizeof(b->data)) {
			LOG_WRN("line too long, fragment dropped");
			ota_free_rx_buf(ota_cur_buf);
			ota_cur_buf = NULL;
			ota_ignoring = true;
		} else {
			b->data[b->length++] = byte;
		}
	}

	if (byte == '\n') {
		if (ota_ignoring) {
			ota_ignoring = false;
		} else {
			ota_cur_buf = NULL;
			return b;
		}
	}
	return NULL;
}

static void ota_uart_isr(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);
	uint8_t buf[32];

	while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
		if (!uart_irq_rx_ready(dev)) {
			continue;
		}
		int n = uart_fifo_read(dev, buf, sizeof(buf));

#if IS_ENABLED(CONFIG_HPI_CONN_DEBUG)
		if (n > 0) {
			ota_rx_total += n;   /* wire-level RX proof (any bytes) */
		}
#endif
		for (int i = 0; i < n; i++) {
			struct ota_rx_buf *frag = ota_rx_byte(buf[i]);

			if (frag != NULL) {
				k_fifo_put(&ota_rx_fifo, frag);
				k_work_submit(&ota_rx_work);
			}
		}
	}
}

/* Work context: de-frame fragments into SMP packets and hand them to SMP. */
static void ota_process_rx_queue(struct k_work *work)
{
	ARG_UNUSED(work);
	struct ota_rx_buf *frag;

	while ((frag = k_fifo_get(&ota_rx_fifo, K_NO_WAIT)) != NULL) {
		struct net_buf *nb = mcumgr_serial_process_frag(&ota_rx_ctxt,
							frag->data, frag->length);
		ota_free_rx_buf(frag);
		if (nb != NULL) {
			smp_rx_req(&ota_transport, nb);
		}
	}
}

/* TX: raw byte writer + framed-packet output callback. */
static int ota_send_raw(const void *data, int len)
{
	const uint8_t *p = data;

	while (len--) {
		uart_poll_out(ota_uart, *p++);
	}
	return 0;
}

static int ota_tx_pkt(struct net_buf *nb)
{
	int rc = mcumgr_serial_tx_pkt(nb->data, nb->len, ota_send_raw);

	smp_packet_free(nb);
	return rc;
}

static uint16_t ota_get_mtu(const struct net_buf *nb)
{
	ARG_UNUSED(nb);
	return CONFIG_MCUMGR_TRANSPORT_UART_MTU;
}

static int ota_uart_init(void)
{
	if (!device_is_ready(ota_uart)) {
		LOG_ERR("UART4 not ready -- wireless OTA transport offline");
		return -ENODEV;
	}

	ota_transport.functions.output = ota_tx_pkt;
	ota_transport.functions.get_mtu = ota_get_mtu;

	int rc = smp_transport_init(&ota_transport);

	if (rc != 0) {
		LOG_ERR("smp_transport_init(UART4) failed: %d", rc);
		return rc;
	}

	uart_irq_rx_disable(ota_uart);
	uart_irq_tx_disable(ota_uart);
	uart_irq_callback_set(ota_uart, ota_uart_isr);
	uart_irq_rx_enable(ota_uart);

#if IS_ENABLED(CONFIG_HPI_CONN_DEBUG)
	k_work_schedule(&ota_dbg_work, K_SECONDS(2));
#endif
	LOG_INF("wireless OTA: MCUmgr SMP on UART4 (ESP32 gateway) ready");
	return 0;
}

SYS_INIT(ota_uart_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#endif /* CONFIG_HPI_WIRELESS_OTA && uart4 okay */
