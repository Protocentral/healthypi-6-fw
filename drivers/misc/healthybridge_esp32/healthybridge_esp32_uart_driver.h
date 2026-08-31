/*
 * Copyright (c) 2026 Protocentral
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file healthybridge_esp32_uart_driver.h
 * @brief HealthyBridge host link over UART -- transmit path.
 *
 * Consumers should use the transport-neutral API in
 * healthybridge_esp32_link.h rather than these symbols directly; they exist for
 * diagnostics and for callers that need to address the UART transport
 * specifically.
 */

#ifndef HEALTHYBRIDGE_ESP32_UART_DRIVER_H
#define HEALTHYBRIDGE_ESP32_UART_DRIVER_H

#include <zephyr/device.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Link counters, for the connectivity service's debug line. */
struct healthybridge_uart_stats {
	uint32_t frames;     /**< frames handed to the UART and completed */
	uint32_t bytes;      /**< wire bytes in those frames */
	uint32_t tx_timeout; /**< aborted: CTS held the transmitter off past the deadline */
	uint32_t tx_error;   /**< uart_tx() rejected the request, or encoding failed */

	uint32_t rx_bytes;   /**< bytes received from the co-processor */
	uint32_t rx_frames;  /**< frames decoded with a valid CRC */
	uint32_t rx_crc_err; /**< frames whose CRC failed */
	uint32_t rx_resync;  /**< decoder sync-hunt restarts */
	uint32_t rx_ovf;     /**< bytes dropped: the RX staging ring was full */
	uint32_t rx_stopped; /**< UART_RX_STOPPED events (framing/parity/overrun) */
};

/**
 * @brief Read and optionally clear the transmit counters.
 *
 * @param dev   HealthyBridge UART device.
 * @param out   Destination, may be NULL.
 * @param clear Zero the counters after reading.
 */
void healthybridge_uart_get_stats(const struct device *dev,
				  struct healthybridge_uart_stats *out, bool clear);

#ifdef __cplusplus
}
#endif

#endif /* HEALTHYBRIDGE_ESP32_UART_DRIVER_H */
