/*
 * Copyright (c) 2026 Protocentral
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file healthybridge_esp32_codec.h
 * @brief HealthyBridge frame codec -- transport-independent.
 *
 * Frame encode, decode and CRC for the HealthyBridge host link. Mirrors the
 * ESP32 side's hb_codec.c; the CRC here is byte-identical to its
 * hb_crc16_ccitt(). Payload structs and type IDs live in
 * healthybridge_esp32_protocol.h.
 */

#ifndef HEALTHYBRIDGE_ESP32_CODEC_H
#define HEALTHYBRIDGE_ESP32_CODEC_H

#include <stdint.h>
#include <stddef.h>

#include "healthybridge_esp32_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Advertised sample rates -- must match what the acquisition layer actually
 * produces. PPG derives from the same Kconfig that programs the AFE4400's PRF
 * (as app_m7/src/core/sample_formats.h does), so the advertised rate and the
 * hardware cannot drift apart. A driver must not include app headers, hence
 * the duplicated derivation from the shared Kconfig symbol.
 */
#define HPI_HB_ECG_RATE_HZ 500
#if defined(CONFIG_AFE4400_PRF_HZ)
#define HPI_HB_PPG_RATE_HZ CONFIG_AFE4400_PRF_HZ
#else
#define HPI_HB_PPG_RATE_HZ 250
#endif

/**
 * @brief CRC-16/CCITT, reflected form, seed 0xFFFF.
 *
 * Byte-identical to Zephyr's crc16_ccitt() and to the ESP codec's
 * hb_crc16_ccitt(). NOT the non-reflected CCITT-FALSE variant.
 */
uint16_t hpi_hb_crc16(const uint8_t *data, size_t len);

/**
 * @brief Encode a frame with a caller-supplied payload.
 *
 * Layout: SYNC | TYPE | FLAGS | LEN | SEQ | PAYLOAD | CRC16.
 * The CRC covers TYPE..PAYLOAD -- everything after the 2-byte sync.
 *
 * @param out         Output buffer.
 * @param out_cap     Capacity of @p out.
 * @param type        HPI_HB_MSG_TYPE_*.
 * @param flags       HPI_HB_FLAG_* bitfield.
 * @param seq         Sequence number.
 * @param payload     Payload bytes, may be NULL when @p payload_len is 0.
 * @param payload_len Payload length.
 *
 * @return Total encoded length, or negative errno.
 */
int hpi_hb_encode(uint8_t *out, size_t out_cap,
		  uint8_t type, uint8_t flags, uint16_t seq,
		  const uint8_t *payload, uint16_t payload_len);

/**
 * @brief Encode a batched-sample frame without a temporary payload buffer.
 *
 * ECG (single and multi-channel) and PPG payloads share one 8-byte header --
 * {u32 timestamp_ms, u16 sample_count, u16 sample_rate_hz} -- followed by a
 * flat sample array. This builds that payload directly in @p out, so a batch
 * frame costs no heap allocation and no second copy.
 *
 * @param samples   Source array, @p count elements of @p sample_sz bytes.
 * @param sample_sz sizeof() one element.
 *
 * @return Total encoded length, or negative errno.
 */
int hpi_hb_encode_batch(uint8_t *out, size_t out_cap,
			uint8_t type, uint16_t seq,
			uint32_t timestamp_ms, uint16_t rate_hz,
			const void *samples, uint16_t count, size_t sample_sz);

/*
 * Streaming decoder.
 *
 * Mirrors hb_codec_feed() on the ESP side. A UART is a byte stream with no
 * message boundaries, so the decoder sync-hunts for 0xAA55, length-guards the
 * header, and only emits a frame once its CRC checks. Anything that fails
 * re-enters the hunt from the next plausible sync byte rather than discarding a
 * fixed amount, so one corrupt frame cannot swallow the frames behind it.
 *
 * Do NOT reset the parser between reads: a frame routinely straddles two DMA
 * chunks and resetting mid-stream would drop it.
 */

/** Emitted for each frame whose CRC validates. @p payload is NULL if len == 0. */
typedef void (*hpi_hb_frame_cb)(uint8_t type, uint8_t flags, uint16_t seq,
				const uint8_t *payload, uint16_t len, void *user);

struct hpi_hb_parser {
	uint8_t buf[HPI_HB_HEADER_SIZE + HPI_HB_MAX_PAYLOAD_SIZE + HPI_HB_CRC_SIZE];
	size_t have;          /* bytes accumulated in buf */
	size_t need;          /* total frame length once the header is in, else 0 */
	uint32_t frames;      /* frames emitted */
	uint32_t crc_errors;  /* frames whose CRC failed */
	uint32_t resyncs;     /* sync-hunt restarts (bad sync or oversize length) */
};

/** Clear parser state. Call once at init, not per read. */
void hpi_hb_parser_reset(struct hpi_hb_parser *p);

/** Push received bytes through the decoder, emitting whole frames via @p cb. */
void hpi_hb_parser_feed(struct hpi_hb_parser *p, const uint8_t *bytes, size_t n,
			hpi_hb_frame_cb cb, void *user);

#ifdef __cplusplus
}
#endif

#endif /* HEALTHYBRIDGE_ESP32_CODEC_H */
