/*
 * Copyright (c) 2026 Protocentral
 *
 * SPDX-License-Identifier: MIT
 *
 * HealthyBridge frame codec -- see healthybridge_esp32_codec.h.
 */

#include <errno.h>
#include <string.h>

#include "healthybridge_esp32_codec.h"

/*
 * CRC-16/CCITT -- REFLECTED form, seed 0xFFFF (Zephyr crc16_ccitt).
 * Byte-identical to the ESP HealthyBridge codec (hb_crc16_ccitt in hb_codec.c);
 * both ends of the link must agree on the wire. This is the reflected
 * (LSB-first) variant, NOT non-reflected CCITT-FALSE.
 */
uint16_t hpi_hb_crc16(const uint8_t *data, size_t len)
{
	uint16_t crc = 0xFFFF;

	for (size_t i = 0; i < len; i++) {
		uint8_t e = (uint8_t)(crc ^ data[i]);
		uint8_t f = (uint8_t)(e ^ (e << 4));

		crc = (uint16_t)((crc >> 8) ^ ((uint16_t)f << 8) ^
				 ((uint16_t)f << 3) ^ ((uint16_t)f >> 4));
	}

	return crc;
}

/* Write the fixed header at `frame` and return a pointer past it. */
static uint8_t *put_header(uint8_t *frame, uint8_t type, uint8_t flags,
			   uint16_t seq, uint16_t payload_len)
{
	struct hpi_hb_frame_header *h = (struct hpi_hb_frame_header *)frame;

	h->sync = HPI_HB_SYNC_WORD;
	h->type = type;
	h->flags = flags;
	h->length = payload_len;
	h->seq = seq;

	return frame + HPI_HB_HEADER_SIZE;
}

/* Append the little-endian CRC over TYPE..PAYLOAD and return the frame length. */
static int put_crc(uint8_t *frame, uint16_t payload_len)
{
	uint16_t crc = hpi_hb_crc16(frame + 2, (HPI_HB_HEADER_SIZE - 2) + payload_len);
	size_t off = HPI_HB_HEADER_SIZE + payload_len;

	frame[off] = (uint8_t)(crc & 0xFF);
	frame[off + 1] = (uint8_t)(crc >> 8);

	return (int)(off + HPI_HB_CRC_SIZE);
}

int hpi_hb_encode(uint8_t *out, size_t out_cap,
		  uint8_t type, uint8_t flags, uint16_t seq,
		  const uint8_t *payload, uint16_t payload_len)
{
	if (out == NULL || (payload_len > 0 && payload == NULL)) {
		return -EINVAL;
	}
	if (payload_len > HPI_HB_MAX_PAYLOAD_SIZE) {
		return -EINVAL;
	}

	size_t total = HPI_HB_HEADER_SIZE + payload_len + HPI_HB_CRC_SIZE;

	if (total > out_cap) {
		return -ENOMEM;
	}

	uint8_t *body = put_header(out, type, flags, seq, payload_len);

	if (payload_len > 0) {
		memcpy(body, payload, payload_len);
	}

	return put_crc(out, payload_len);
}

int hpi_hb_encode_batch(uint8_t *out, size_t out_cap,
			uint8_t type, uint16_t seq,
			uint32_t timestamp_ms, uint16_t rate_hz,
			const void *samples, uint16_t count, size_t sample_sz)
{
	if (out == NULL || samples == NULL || count == 0 || sample_sz == 0) {
		return -EINVAL;
	}

	/*
	 * The shared batch payload header. Deliberately not a struct cast: the
	 * three batch payloads in the protocol header (ecg, ecg_multi, ppg) all
	 * begin with these same 8 bytes, and writing them field-by-field keeps
	 * this one encoder valid for all three without picking one struct to
	 * stand in for the others.
	 */
	const size_t pl_hdr = sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint16_t);
	size_t bulk = (size_t)count * sample_sz;
	size_t payload_len = pl_hdr + bulk;

	if (payload_len > HPI_HB_MAX_PAYLOAD_SIZE) {
		return -ENOMEM;
	}

	size_t total = HPI_HB_HEADER_SIZE + payload_len + HPI_HB_CRC_SIZE;

	if (total > out_cap) {
		return -ENOMEM;
	}

	uint8_t *body = put_header(out, type, 0, seq, (uint16_t)payload_len);

	/* Little-endian, unaligned-safe. */
	body[0] = (uint8_t)(timestamp_ms & 0xFF);
	body[1] = (uint8_t)((timestamp_ms >> 8) & 0xFF);
	body[2] = (uint8_t)((timestamp_ms >> 16) & 0xFF);
	body[3] = (uint8_t)((timestamp_ms >> 24) & 0xFF);
	body[4] = (uint8_t)(count & 0xFF);
	body[5] = (uint8_t)(count >> 8);
	body[6] = (uint8_t)(rate_hz & 0xFF);
	body[7] = (uint8_t)(rate_hz >> 8);

	memcpy(body + pl_hdr, samples, bulk);

	return put_crc(out, (uint16_t)payload_len);
}

/* ---- streaming decoder --------------------------------------------------
 *
 * The sync word is a little-endian uint16_t in the packed header, so on the
 * wire it is 0x55 then 0xAA. Deriving both bytes from HPI_HB_SYNC_WORD rather
 * than writing the literals keeps the decoder honest if the constant ever moves.
 */
#define SYNC_LO ((uint8_t)(HPI_HB_SYNC_WORD & 0xFF))
#define SYNC_HI ((uint8_t)(HPI_HB_SYNC_WORD >> 8))

void hpi_hb_parser_reset(struct hpi_hb_parser *p)
{
	if (p != NULL) {
		memset(p, 0, sizeof(*p));
	}
}

void hpi_hb_parser_feed(struct hpi_hb_parser *p, const uint8_t *bytes, size_t n,
			hpi_hb_frame_cb cb, void *user)
{
	if (p == NULL || bytes == NULL) {
		return;
	}

	for (size_t i = 0; i < n; i++) {
		uint8_t b = bytes[i];

		/* --- sync hunt: nothing accumulated yet --- */
		if (p->have == 0) {
			if (b == SYNC_LO) {
				p->buf[p->have++] = b;
			}
			continue;
		}
		if (p->have == 1) {
			if (b == SYNC_HI) {
				p->buf[p->have++] = b;
			} else if (b == SYNC_LO) {
				/* Stay latched on this byte as a fresh candidate
				 * rather than dropping it -- 55 55 AA is a valid
				 * start and a naive reset would miss it. */
				p->buf[0] = b;
			} else {
				p->have = 0;
				p->resyncs++;
			}
			continue;
		}

		p->buf[p->have++] = b;

		/* --- header complete: learn the frame length --- */
		if (p->need == 0 && p->have == HPI_HB_HEADER_SIZE) {
			const struct hpi_hb_frame_header *h =
				(const struct hpi_hb_frame_header *)p->buf;

			if (h->length > HPI_HB_MAX_PAYLOAD_SIZE) {
				/* A length this large is noise that happened to
				 * follow a sync word. Re-hunt from the byte
				 * after the false sync, not from here, or a
				 * real frame overlapping it would be lost. */
				p->have = 0;
				p->need = 0;
				p->resyncs++;
				continue;
			}
			p->need = (size_t)HPI_HB_HEADER_SIZE + h->length + HPI_HB_CRC_SIZE;
		}

		/* --- frame complete: verify and emit --- */
		if (p->need != 0 && p->have == p->need) {
			const struct hpi_hb_frame_header *h =
				(const struct hpi_hb_frame_header *)p->buf;
			uint16_t want = hpi_hb_crc16(p->buf + 2,
						     (HPI_HB_HEADER_SIZE - 2) + h->length);
			uint16_t got = (uint16_t)p->buf[p->need - 2] |
				       ((uint16_t)p->buf[p->need - 1] << 8);

			if (want == got) {
				p->frames++;
				if (cb != NULL) {
					cb(h->type, h->flags, h->seq,
					   h->length ? p->buf + HPI_HB_HEADER_SIZE : NULL,
					   h->length, user);
				}
			} else {
				p->crc_errors++;
			}
			p->have = 0;
			p->need = 0;
		}
	}
}
