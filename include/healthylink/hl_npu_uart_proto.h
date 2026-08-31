/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * HealthyLink NPU UART transport protocol -- SHARED between the HealthyPi 6 host
 * (STM32H757 M7, USART2) and the HealthyLink Compute module (STM32N657, USART1).
 *
 * Link (DF9 slot A, 4-wire + GND, crossover):
 *   host PD5 TX  -> pin 19 -> module PB7 RX
 *   host PD6 RX  <- pin 21 <- module PB6 TX
 *   host PD4 RTS -> pin 23 -> module PA11 CTS
 *   host PD3 CTS <- pin 25 <- module PB0  RTS
 * Recommended line config: 1 Mbaud, 8N1, hardware RTS/CTS flow control.
 *
 * Framing (little-endian on the wire):
 *
 *   +------+------+-----+-------+--------+--------+===========+--------+--------+
 *   | SOF0 | SOF1 | cmd | flags | len_lo | len_hi |  payload  | crc_lo | crc_hi |
 *   +------+------+-----+-------+--------+--------+===========+--------+--------+
 *     0xA5   0xC3                  <-- u16 len -->   len bytes   CRC16-CCITT
 *
 *   CRC16-CCITT (poly 0x1021, seed 0xFFFF) is computed over the bytes from
 *   `cmd` through the end of `payload` (i.e. it covers cmd, flags, len, payload;
 *   NOT the SOF bytes). Use Zephyr's crc16_ccitt(0xFFFF, p, n).
 *
 * Transaction model: the host sends a request frame (flags = 0); the module
 * replies with one response frame carrying the same `cmd`, flags |= RESPONSE,
 * and flags |= ERROR on failure (payload then = 1 status byte).
 */

#ifndef HEALTHYLINK_HL_NPU_UART_PROTO_H_
#define HEALTHYLINK_HL_NPU_UART_PROTO_H_

#include <stdint.h>
#include <zephyr/toolchain.h>   /* __packed */

#ifdef __cplusplus
extern "C" {
#endif

/* --- Line configuration --- */
#define HL_NPU_UART_BAUD            1000000u   /* 1 Mbaud */

/* --- Framing --- */
#define HL_NPU_UART_SOF0           0xA5u
#define HL_NPU_UART_SOF1           0xC3u
#define HL_NPU_UART_HDR_LEN        6u          /* SOF0,SOF1,cmd,flags,len_lo,len_hi */
#define HL_NPU_UART_CRC_LEN        2u
#define HL_NPU_UART_MAX_PAYLOAD    1024u       /* >= 876-byte arrhythmia input + margin */
#define HL_NPU_UART_MAX_FRAME      (HL_NPU_UART_HDR_LEN + HL_NPU_UART_MAX_PAYLOAD + \
                                    HL_NPU_UART_CRC_LEN)

/* CRC16 is computed starting at this byte offset (cmd), for (HDR_LEN-2)+len bytes. */
#define HL_NPU_UART_CRC_OFFSET     2u

/* --- Frame flags --- */
#define HL_NPU_FLAG_RESPONSE       0x01u       /* set in replies */
#define HL_NPU_FLAG_ERROR          0x02u       /* reply indicates failure (payload[0]=status) */

/* --- Commands (mirror the legacy SPI AI_CMD_* set) --- */
#define HL_NPU_CMD_NOP             0x00u
#define HL_NPU_CMD_STATUS          0x01u       /* req: none; resp: [status u8] */
#define HL_NPU_CMD_RESET           0x02u       /* req: none; resp: [status u8] */
#define HL_NPU_CMD_LOAD_INPUT      0x10u       /* req: input bytes; resp: [status u8] */
#define HL_NPU_CMD_RUN_INFERENCE   0x20u       /* req: none; resp: [status u8] (poll STATUS) */
#define HL_NPU_CMD_READ_OUTPUT     0x30u       /* req: none; resp: output bytes */
#define HL_NPU_CMD_GET_INFO        0xF0u       /* req: none; resp: hl_npu_info */

/* --- Status byte bits (mirror the legacy SPI AI_STATUS_* set) --- */
#define HL_NPU_ST_READY            0x01u
#define HL_NPU_ST_BUSY             0x02u
#define HL_NPU_ST_ERROR            0x04u
#define HL_NPU_ST_OUTPUT_READY     0x08u

/* GET_INFO response payload (little-endian). */
struct hl_npu_info {
	uint16_t input_size;     /* model input length in bytes  */
	uint16_t output_size;    /* model output length in bytes */
	uint8_t  proto_version;  /* HL_NPU_UART_PROTO_VERSION    */
	uint8_t  model_version;  /* module-defined               */
} __packed;

#define HL_NPU_UART_PROTO_VERSION  1u

#ifdef __cplusplus
}
#endif

#endif /* HEALTHYLINK_HL_NPU_UART_PROTO_H_ */
