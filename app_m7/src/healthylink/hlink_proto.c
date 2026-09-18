/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * HLink v2 codec, host side. Mirrors healthylink-compute-fw's
 * app/src/link/hlink_proto.c -- same function names, same errno vocabulary, so
 * the two can be diffed and a log line means the same thing on both sides of
 * the connector.
 *
 * Pure arithmetic: no device, no thread, no transport. It is compiled
 * unconditionally (not behind CONFIG_HPI_NPU_COMMS_CHECK) so a default build
 * type-checks it.
 */

#include "hlink_proto.h"

#include <errno.h>
#include <string.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>

LOG_MODULE_REGISTER(hlink_proto, CONFIG_HPI_APP_LOG_LEVEL);

const uint8_t hlink_alive_magic[4] = { 'H', 'L', 'N', 'K' };

/* Compile-time guards on the mirrored reply geometry. If the module's layout
 * moves, this is where it should stop -- at a build error next to the offsets,
 * not at a field decoded from the wrong byte. */
BUILD_ASSERT(HLINK_GET_INFO_LEN == 14, "GET_INFO reply is 14 B (HLINK_PROTOCOL.md 4.1)");
BUILD_ASSERT(HLINK_STATUS_LEN == 66, "STATUS reply is 66 B (HLINK_PROTOCOL.md 4.2)");
BUILD_ASSERT(HLINK_ST_OFF_UPTIME_MS + 4 == HLINK_STATUS_LEN, "STATUS ends at uptime_ms");
BUILD_ASSERT(HLINK_GI_OFF_CAPS + 4 == HLINK_GET_INFO_LEN, "GET_INFO ends at caps");
BUILD_ASSERT(HLINK_OVERHEAD == 10, "8-byte header + 2-byte CRC");
BUILD_ASSERT(sizeof(struct hlink_model_list_hdr) == HLINK_MODEL_LIST_HDR_LEN,
	     "MODEL_LIST header is 4 B");
BUILD_ASSERT(sizeof(struct hlink_model_list_entry) == HLINK_MODEL_LIST_ENTRY_LEN,
	     "MODEL_LIST entry is 56 B");
BUILD_ASSERT(sizeof(struct hlink_tensor_desc) == HLINK_TENSOR_DESC_LEN,
	     "tensor descriptor is 36 B");
BUILD_ASSERT(sizeof(struct hlink_model_info_req) == HLINK_MODEL_INFO_REQ_LEN,
	     "MODEL_INFO request is 33 B");
BUILD_ASSERT(sizeof(struct hlink_model_info_part0) == HLINK_MODEL_INFO_PART0_LEN,
	     "MODEL_INFO part 0 is 161 B");
BUILD_ASSERT(sizeof(struct hlink_tensor_load_req) == HLINK_TENSOR_LOAD_REQ_HDR_LEN,
	     "TENSOR_LOAD request header is 4 B");
BUILD_ASSERT(sizeof(struct hlink_tensor_load_rsp) == HLINK_TENSOR_LOAD_RSP_LEN,
	     "TENSOR_LOAD reply is 8 B");
BUILD_ASSERT(sizeof(struct hlink_run_req) == HLINK_RUN_REQ_LEN, "RUN request is 1 B");
BUILD_ASSERT(sizeof(struct hlink_run_rsp) == HLINK_RUN_RSP_LEN, "RUN reply is 4 B");
BUILD_ASSERT(sizeof(struct hlink_read_result_req) == HLINK_READ_RESULT_REQ_LEN,
	     "READ_RESULT request is 4 B");
BUILD_ASSERT(sizeof(struct hlink_result_hdr) == HLINK_RESULT_HDR_LEN,
	     "result header is 36 B");
BUILD_ASSERT(sizeof(struct hlink_stream_push_req) == HLINK_STREAM_PUSH_REQ_HDR_LEN,
	     "STREAM_PUSH request header is 12 B");
BUILD_ASSERT(sizeof(struct hlink_stream_push_rsp) == HLINK_STREAM_PUSH_RSP_LEN,
	     "STREAM_PUSH reply is 8 B");
BUILD_ASSERT(sizeof(struct hlink_stream_event_req) == HLINK_STREAM_EVENT_REQ_LEN,
	     "STREAM_EVENT request is 12 B");
BUILD_ASSERT(sizeof(struct hlink_stream_event_rsp) == HLINK_STREAM_EVENT_RSP_LEN,
	     "STREAM_EVENT reply is 4 B");

uint16_t hlink_crc16(uint16_t crc, const uint8_t *data, size_t len)
{
	if (data == NULL) {
		return crc;
	}
	/* crc16_itu_t, NOT crc16_ccitt -- see the header. Zephyr's is
	 * table-driven; the module's is the equivalent bitwise loop. */
	return crc16_itu_t(crc, data, len);
}

int hlink_encode(uint8_t cmd, uint8_t flags, uint8_t seq,
		 const void *payload, uint16_t len, uint8_t *out, size_t out_cap)
{
	if (out == NULL || (payload == NULL && len > 0)) {
		return -EINVAL;
	}
	if (out_cap < (size_t)HLINK_OVERHEAD + len) {
		return -ENOSPC;
	}

	out[HLINK_OFF_SOF] = HLINK_SOF;
	out[HLINK_OFF_CMD] = cmd;
	out[HLINK_OFF_FLAGS] = flags;
	out[HLINK_OFF_SEQ] = seq;
	sys_put_le16(len, &out[HLINK_OFF_LEN]);
	sys_put_le16(0, &out[HLINK_OFF_RSVD]);
	if (len > 0) {
		memcpy(&out[HLINK_OFF_PAYLOAD], payload, len);
	}

	/* The CRC spans cmd..payload -- the header too, so a corrupted seq or
	 * cmd is caught rather than acted on. */
	uint16_t crc = hlink_crc16(HLINK_CRC16_INIT, &out[HLINK_OFF_CMD],
				   (size_t)(HLINK_HDR_LEN - 1) + len);

	sys_put_le16(crc, &out[HLINK_HDR_LEN + len]);
	return (int)(HLINK_OVERHEAD + len);
}

int hlink_decode(const uint8_t *buf, size_t len, struct hlink_frame *out)
{
	if (buf == NULL || out == NULL) {
		return -EINVAL;
	}
	if (len < HLINK_HDR_LEN) {
		return -EAGAIN;
	}
	if (buf[HLINK_OFF_SOF] != HLINK_SOF) {
		return -EBADMSG;
	}

	uint16_t plen = sys_get_le16(&buf[HLINK_OFF_LEN]);
	size_t total = (size_t)HLINK_OVERHEAD + plen;

	if (len < total) {
		return -EAGAIN;
	}

	uint16_t want = hlink_crc16(HLINK_CRC16_INIT, &buf[HLINK_OFF_CMD],
				    (size_t)(HLINK_HDR_LEN - 1) + plen);
	uint16_t got = sys_get_le16(&buf[HLINK_HDR_LEN + plen]);

	if (want != got) {
		return -EILSEQ;
	}

	out->cmd = buf[HLINK_OFF_CMD];
	out->flags = buf[HLINK_OFF_FLAGS];
	out->seq = buf[HLINK_OFF_SEQ];
	out->len = plen;
	out->rsvd = sys_get_le16(&buf[HLINK_OFF_RSVD]);
	out->payload = plen ? &buf[HLINK_OFF_PAYLOAD] : NULL;
	return (int)total;
}

int hlink_find_sof(const uint8_t *buf, size_t len)
{
	if (buf == NULL || len < HLINK_HDR_LEN) {
		return -ENOENT;
	}
	for (size_t i = 0; i + HLINK_HDR_LEN <= len; i++) {
		if (buf[i] != HLINK_SOF) {
			continue;
		}
		/* Require the declared length to fit what is left, so a stray
		 * 0xA5 inside payload data is rejected cheaply. The CRC is the
		 * real test and hlink_decode() applies it. */
		uint16_t plen = sys_get_le16(&buf[i + HLINK_OFF_LEN]);

		if (i + HLINK_OVERHEAD + plen <= len) {
			return (int)i;
		}
	}
	return -ENOENT;
}

const char *hlink_status_str(uint8_t status)
{
	static const char *const N[] = {
		"OK", "BAD_CMD", "BAD_LEN", "BAD_CRC", "NOT_READY", "NO_MODEL",
		"BUSY", "UNSUPPORTED", "INTERNAL", "PENDING", "NO_RESULT", "RANGE",
	};

	return (status < ARRAY_SIZE(N)) ? N[status] : "?";
}

int hlink_selftest(void)
{
	static const uint8_t vec[] = "123456789";
	uint16_t crc = hlink_crc16(HLINK_CRC16_INIT, vec, sizeof(vec) - 1);

	if (crc != 0x29B1) {
		/* The whole link is dead in a way that reads as bad wiring, so
		 * say which mistake it is. */
		LOG_ERR("HLink CRC is wrong: got 0x%04x, want 0x29B1. The codec "
			"must use crc16_itu_t (non-reflected), not crc16_ccitt.",
			crc);
		return -EILSEQ;
	}

	uint8_t buf[HLINK_OVERHEAD + 4];
	const uint8_t pay[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
	struct hlink_frame f;

	int n = hlink_encode(HLINK_CMD_PING, HLINK_FLAG_REPLY, 0x5A, pay,
			     sizeof(pay), buf, sizeof(buf));

	if (n != (int)sizeof(buf) || hlink_decode(buf, sizeof(buf), &f) != n ||
	    f.cmd != HLINK_CMD_PING || f.seq != 0x5A || f.len != sizeof(pay) ||
	    f.payload == NULL || memcmp(f.payload, pay, sizeof(pay)) != 0) {
		LOG_ERR("HLink codec round-trip failed");
		return -EILSEQ;
	}

	LOG_DBG("HLink codec OK (CRC 0x29B1, round-trip %d B)", n);
	return 0;
}
