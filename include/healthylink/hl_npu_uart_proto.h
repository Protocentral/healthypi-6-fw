/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * HealthyLink NPU UART transport protocol -- the HealthyPi 6 host half
 * (STM32H757 M7, USART2).
 *
 * ===========================================================================
 * PARKED, AND POINTED AT THE WRONG PLANE. Do not extend this without reading
 * the following; three separate things about it are now known to be wrong.
 *
 * 1. THE MODULE DOES NOT SERVE HLINK ON A WIRE THE M7 CAN REACH. The Compute
 *    module binds its HLink UART to `chosen hlc,link-uart = &cdc_acm_uart0` --
 *    its USB CDC, reachable from a PC and not from us -- and its usart1
 *    (PF13 TX / PF12 RX / PF15 RTS / PF14 CTS -> J5 pins 21/19/25/23) now
 *    carries MCUmgr SMP, not HLink. On a fitted module HLink is reachable over
 *    SPI ONLY. See app_m7/src/healthylink/hlink_proto.h.
 *
 * 2. THE PIN TABLE BELOW WAS THE HOST'S OWN PINS, LISTED AS THE MODULE'S. It
 *    said module USART1 was PB7 RX / PB6 TX / PA11 CTS / PB0 RTS. That is the
 *    STM32H7 mapping; on the STM32N657 those PB pins are spi4_miso / spi4_mosi
 *    / spi4_nss and cannot be a UART at all. Retracted rather than corrected,
 *    because the wire this file describes is no longer the wire to use.
 *
 * 3. THE CRC CALL BELOW IS THE WRONG VARIANT (see the framing note).
 *
 * 4. The HL_NPU_CMD_* numbering is HLink v1 and COLLIDES semantically with v2:
 *    0x01 STATUS is now PING, 0x02 RESET is GET_INFO, 0x10 LOAD_INPUT is
 *    MODEL_LIST, 0x20 RUN_INFERENCE is TENSOR_LOAD, 0x30 READ_OUTPUT is
 *    STREAM_PUSH. A v2 module would act on these, not reject them.
 *
 * Kept in tree because the framing itself is sound and identical to the
 * module's byte-stream framing, so it is the right starting point if a serial
 * HLink transport is ever wanted again.
 * ===========================================================================
 *
 * Historical link description (DF9 slot A, 4-wire + GND, crossover), host side
 * only, module side retracted per note 2:
 *   host PD5 TX  -> pin 19
 *   host PD6 RX  <- pin 21
 *   host PD4 RTS -> pin 23
 *   host PD3 CTS <- pin 25
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
 *   NOT the SOF bytes).
 *
 *   USE crc16_itu_t(0xFFFF, p, n), NOT crc16_ccitt(). This line said
 *   crc16_ccitt for two years and it was wrong: Zephyr's crc16_ccitt() is the
 *   REFLECTED 0x1021 variant (CRC-16/KERMIT at seed 0, CRC-16/X-25 at seed
 *   0xFFFF), while the module computes the non-reflected CRC-16/IBM-3740
 *   (check value 0x29B1 over "123456789"). crc16_itu_t() is that one.
 *   npu_uart_host.c still calls crc16_ccitt() and MUST be fixed if this
 *   transport is ever revived -- as written, every frame it sends is rejected,
 *   with a symptom indistinguishable from bad wiring.
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
