/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Host-side (STM32H757 M7) UART transport to the HealthyLink Compute NPU module
 * over USART2 @ 1 Mbaud. See include/healthylink/hl_npu_uart_proto.h.
 */

#ifndef HPI_NPU_UART_HOST_H_
#define HPI_NPU_UART_HOST_H_

#include <stdint.h>
#include <stddef.h>

/* Schedule a one-shot NPU comms check (STATUS + GET_INFO) off the boot path.
 * Safe to call from npu_start(); never blocks the caller. No-op stub when
 * CONFIG_HPI_NPU_UART is disabled. */
void hpi_npu_uart_kick(void);

/* Last comms-check result: -2 not run, -1 in flight, 0 OK, <0 error. */
int hpi_npu_uart_last_result(void);

/* Synchronous request/response transaction (request flags=0). Returns 0 on a
 * valid CRC-checked reply, negative errno otherwise. `rx`/`rx_cap` may be NULL/0
 * if no response payload is expected. On success *rx_len holds the reply payload
 * length and *rx_flags the reply flags (check HL_NPU_FLAG_ERROR). Only available
 * when CONFIG_HPI_NPU_UART is enabled. */
int hpi_npu_uart_xact(uint8_t cmd, const uint8_t *tx, uint16_t tx_len,
		      uint8_t *rx, uint16_t rx_cap, uint16_t *rx_len,
		      uint8_t *rx_flags, int timeout_ms);

#endif /* HPI_NPU_UART_HOST_H_ */
