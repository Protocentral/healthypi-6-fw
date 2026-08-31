/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Host-side UART transport to the HealthyLink Compute NPU (STM32N657) over
 * USART2 @ 1 Mbaud, 8N1, HW RTS/CTS. Replaces the parked SPI4 link.
 *
 * RX is interrupt-driven into a ring buffer; a frame parser in the requesting
 * thread reassembles one response with a deadline. TX uses uart_poll_out (frames
 * are tiny for control; the 876-byte input for inference is still well under a
 * few ms at 1 Mbaud). Runs off the boot path on a dedicated low-priority work
 * queue so it cannot starve the watchdog.
 */

#include "npu_uart_host.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/crc.h>
#include <zephyr/logging/log.h>
#include <healthylink/hl_npu_uart_proto.h>
#include <string.h>

LOG_MODULE_REGISTER(npu_uart, CONFIG_HPI_APP_LOG_LEVEL);

#define NPU_UART_NODE DT_NODELABEL(usart2)

#if IS_ENABLED(CONFIG_HPI_NPU_UART) && DT_NODE_HAS_STATUS(NPU_UART_NODE, okay)
#define NPU_UART_AVAILABLE 1

static const struct device *const npu_uart = DEVICE_DT_GET(NPU_UART_NODE);

RING_BUF_DECLARE(npu_rx_rb, 2 * HL_NPU_UART_MAX_FRAME);
static struct k_sem npu_rx_sem;
static bool npu_uart_inited;
static volatile int npu_uart_last = -2;   /* -2 not run, -1 in flight, 0 ok, <0 err */

/* ---- RX ISR: drain FIFO into the ring buffer, wake the parser ---- */
static void npu_uart_isr(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);
	while (uart_irq_update(dev) && uart_irq_rx_ready(dev)) {
		uint8_t buf[64];
		int n = uart_fifo_read(dev, buf, sizeof(buf));
		if (n <= 0) {
			break;
		}
		(void)ring_buf_put(&npu_rx_rb, buf, n);
	}
	k_sem_give(&npu_rx_sem);
}

/* ---- TX: send one framed request (flags = 0) ---- */
static void send_frame(uint8_t cmd, const uint8_t *payload, uint16_t len)
{
	uint8_t hdr[HL_NPU_UART_HDR_LEN] = {
		HL_NPU_UART_SOF0, HL_NPU_UART_SOF1, cmd, 0u,
		(uint8_t)(len & 0xff), (uint8_t)(len >> 8),
	};
	uint16_t crc = crc16_ccitt(0xffff, &hdr[HL_NPU_UART_CRC_OFFSET],
				   HL_NPU_UART_HDR_LEN - HL_NPU_UART_CRC_OFFSET);
	if (len) {
		crc = crc16_ccitt(crc, payload, len);
	}

	for (size_t i = 0; i < sizeof(hdr); i++) {
		uart_poll_out(npu_uart, hdr[i]);
	}
	for (uint16_t i = 0; i < len; i++) {
		uart_poll_out(npu_uart, payload[i]);
	}
	uart_poll_out(npu_uart, (uint8_t)(crc & 0xff));
	uart_poll_out(npu_uart, (uint8_t)(crc >> 8));
}

/* ---- RX: reassemble one response frame with a deadline ---- */
static int recv_frame(uint8_t *out_cmd, uint8_t *out_flags,
		      uint8_t *payload, uint16_t cap, uint16_t *out_len,
		      int timeout_ms)
{
	enum { S_SOF0, S_SOF1, S_CMD, S_FLAGS, S_LEN0, S_LEN1, S_PAY, S_CRC0, S_CRC1 } st = S_SOF0;
	/* acc holds cmd,flags,len0,len1,payload... for the CRC check */
	static uint8_t acc[HL_NPU_UART_HDR_LEN - 2 + HL_NPU_UART_MAX_PAYLOAD];
	uint16_t len = 0, idx = 0;
	uint16_t rxcrc = 0;
	int64_t deadline = k_uptime_get() + timeout_ms;

	for (;;) {
		uint8_t b;
		if (ring_buf_get(&npu_rx_rb, &b, 1) != 1) {
			int64_t rem = deadline - k_uptime_get();
			if (rem <= 0) {
				return -ETIMEDOUT;
			}
			k_sem_take(&npu_rx_sem, K_MSEC(rem));
			continue;
		}

		switch (st) {
		case S_SOF0:
			st = (b == HL_NPU_UART_SOF0) ? S_SOF1 : S_SOF0;
			break;
		case S_SOF1:
			st = (b == HL_NPU_UART_SOF1) ? S_CMD :
			     (b == HL_NPU_UART_SOF0) ? S_SOF1 : S_SOF0;
			break;
		case S_CMD:
			acc[0] = b; *out_cmd = b; st = S_FLAGS; break;
		case S_FLAGS:
			acc[1] = b; *out_flags = b; st = S_LEN0; break;
		case S_LEN0:
			acc[2] = b; len = b; st = S_LEN1; break;
		case S_LEN1:
			acc[3] = b; len |= (uint16_t)b << 8;
			if (len > HL_NPU_UART_MAX_PAYLOAD) {
				st = S_SOF0;          /* bogus length; resync */
				break;
			}
			idx = 0;
			st = (len == 0) ? S_CRC0 : S_PAY;
			break;
		case S_PAY:
			acc[4 + idx] = b;
			if (idx < cap && payload) {
				payload[idx] = b;
			}
			if (++idx >= len) {
				st = S_CRC0;
			}
			break;
		case S_CRC0:
			rxcrc = b; st = S_CRC1; break;
		case S_CRC1:
			rxcrc |= (uint16_t)b << 8;
			*out_len = len;
			{
				uint16_t calc = crc16_ccitt(0xffff, acc, 4 + len);
				return (calc == rxcrc) ? 0 : -EIO;
			}
		}
	}
}

int hpi_npu_uart_xact(uint8_t cmd, const uint8_t *tx, uint16_t tx_len,
		      uint8_t *rx, uint16_t rx_cap, uint16_t *rx_len,
		      uint8_t *rx_flags, int timeout_ms)
{
	if (!device_is_ready(npu_uart)) {
		return -ENODEV;
	}
	uint8_t rcmd = 0, rflags = 0;
	uint16_t rlen = 0;

	(void)ring_buf_get(&npu_rx_rb, NULL, ring_buf_size_get(&npu_rx_rb)); /* flush stale */
	send_frame(cmd, tx, tx_len);
	int rc = recv_frame(&rcmd, &rflags, rx, rx_cap, &rlen, timeout_ms);
	if (rc != 0) {
		return rc;
	}
	if (rx_len) {
		*rx_len = rlen;
	}
	if (rx_flags) {
		*rx_flags = rflags;
	}
	if (rcmd != cmd) {
		return -EBADMSG;
	}
	if (rflags & HL_NPU_FLAG_ERROR) {
		return -EPROTO;   /* remote NPU reported an error flag (no EREMOTEIO in picolibc) */
	}
	return 0;
}

static int npu_comms_check(void)
{
	if (!device_is_ready(npu_uart)) {
		LOG_ERR("NPU UART: USART2 not ready");
		return -ENODEV;
	}
	if (!npu_uart_inited) {
		k_sem_init(&npu_rx_sem, 0, 1);
		uart_irq_callback_user_data_set(npu_uart, npu_uart_isr, NULL);
		uart_irq_rx_enable(npu_uart);
		npu_uart_inited = true;
	}

	uint8_t pl[sizeof(struct hl_npu_info)];
	uint16_t rlen = 0;
	uint8_t rflags = 0;

	int rc = hpi_npu_uart_xact(HL_NPU_CMD_STATUS, NULL, 0, pl, sizeof(pl),
				   &rlen, &rflags, 200);
	uint8_t status = (rc == 0 && rlen >= 1) ? pl[0] : 0xff;

	struct hl_npu_info info = {0};
	int rc2 = hpi_npu_uart_xact(HL_NPU_CMD_GET_INFO, NULL, 0, (uint8_t *)&info,
				    sizeof(info), &rlen, &rflags, 200);

	LOG_INF("NPU UART: STATUS rc=%d st=0x%02x (busy=%d) | GET_INFO rc=%d "
		"in=%u out=%u proto=%u modelv=%u",
		rc, status, !!(status & HL_NPU_ST_BUSY), rc2,
		(rc2 == 0) ? info.input_size : 0, (rc2 == 0) ? info.output_size : 0,
		(rc2 == 0) ? info.proto_version : 0, (rc2 == 0) ? info.model_version : 0);

	if (rc != 0 || rc2 != 0) {
		LOG_WRN("NPU UART comms FAIL (rc=%d rc2=%d) -- check N657 USART1 server, "
			"1 Mbaud both ends, RTS/CTS, and wiring (host PD5/PD6 <-> mod PB7/PB6)",
			rc, rc2);
		return (rc != 0) ? rc : rc2;
	}
	LOG_INF("NPU UART comms OK -- module responding (input=%u, output=%u)",
		info.input_size, info.output_size);
	return 0;
}

/* ---- one-shot off the boot path ---- */
K_THREAD_STACK_DEFINE(npu_uart_wq_stack, 3072);
static struct k_work_q npu_uart_wq;
static struct k_work npu_uart_work;
static bool npu_uart_wq_started;

static void npu_uart_work_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	npu_uart_last = npu_comms_check();
}

int hpi_npu_uart_last_result(void)
{
	return npu_uart_last;
}

void hpi_npu_uart_kick(void)
{
	if (!npu_uart_wq_started) {
		k_work_queue_init(&npu_uart_wq);
		k_work_queue_start(&npu_uart_wq, npu_uart_wq_stack,
				   K_THREAD_STACK_SIZEOF(npu_uart_wq_stack),
				   K_LOWEST_APPLICATION_THREAD_PRIO, NULL);
		k_thread_name_set(&npu_uart_wq.thread, "npu_uart");
		k_work_init(&npu_uart_work, npu_uart_work_fn);
		npu_uart_wq_started = true;
	}
	npu_uart_last = -1;
	k_work_submit_to_queue(&npu_uart_wq, &npu_uart_work);
}

#else /* !NPU_UART_AVAILABLE */

void hpi_npu_uart_kick(void)
{
	LOG_WRN("NPU UART: disabled (CONFIG_HPI_NPU_UART) or USART2 not enabled");
}

int hpi_npu_uart_last_result(void)
{
	return -2;
}

int hpi_npu_uart_xact(uint8_t cmd, const uint8_t *tx, uint16_t tx_len,
		      uint8_t *rx, uint16_t rx_cap, uint16_t *rx_len,
		      uint8_t *rx_flags, int timeout_ms)
{
	ARG_UNUSED(cmd); ARG_UNUSED(tx); ARG_UNUSED(tx_len);
	ARG_UNUSED(rx); ARG_UNUSED(rx_cap); ARG_UNUSED(rx_len);
	ARG_UNUSED(rx_flags); ARG_UNUSED(timeout_ms);
	return -ENOTSUP;
}

#endif /* NPU_UART_AVAILABLE */
