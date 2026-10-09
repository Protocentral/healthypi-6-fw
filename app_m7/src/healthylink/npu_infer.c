/*
 * Copyright (c) 2026 ProtoCentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * WS2a bring-up: one canned 187-byte INT8 beat over SPI, publish HPI_CH_INFER.
 * Auto-submitted on link-up only while the stream producer is not compiled.
 * Never clear HP6_INF_STUB on a five-zero reply.
 */

#include "npu_infer.h"
#include "npu_link.h"

#include "core/sample_bus.h"
#include "core/sample_formats.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(npu_infer, CONFIG_HPI_APP_LOG_LEVEL);

#include "npu_infer_beat187.inc"

BUILD_ASSERT(sizeof(npu_infer_beat187) == 187, "beat window is 187 INT8 samples");
BUILD_ASSERT(sizeof(struct hp6_infer_sample) == 16, "HPI_CH_INFER payload is 16 B");

#define NPU_INFER_TX_LEN (HLINK_TENSOR_LOAD_REQ_HDR_LEN + 187)

static uint8_t npu_infer_tx[NPU_INFER_TX_LEN];
static struct k_work npu_infer_work;
static bool npu_infer_inited;

static void npu_infer_publish(const int8_t scores[5], uint8_t class_id,
			      uint8_t extra_flags, bool stub)
{
	struct hp6_infer_sample s;
	int best = scores[class_id < 5 ? class_id : 0];

	memset(&s, 0, sizeof(s));
	s.ts_ms = (uint32_t)k_uptime_get();
	s.model_id = 0;
	s.class_id = class_id;
	s.confidence = (uint8_t)(best + 128);
	memcpy(s.scores, scores, 5);
	s.flags = extra_flags | (stub ? HP6_INF_STUB : 0);

	struct hpi_sample_frame f = {
		.channel = HPI_CH_INFER,
		.sample_rate = 0,
		.sample_count = 1,
		.t_mono_us = (uint64_t)k_uptime_get() * 1000ULL,
		.len = sizeof(s),
		.flags = 0,
		.payload = &s,
	};
	(void)hpi_bus_publish(&f);

	LOG_INF("NPU infer: class=%u conf=%u stub=%d scores=%d %d %d %d %d",
		s.class_id, s.confidence, stub ? 1 : 0,
		scores[0], scores[1], scores[2], scores[3], scores[4]);
}

static int npu_infer_load(void)
{
	struct hlink_frame reply;

	npu_infer_tx[0] = HLINK_MODEL_IDX_ACTIVE;
	npu_infer_tx[1] = 0;
	sys_put_le16(0, &npu_infer_tx[2]);
	memcpy(&npu_infer_tx[4], npu_infer_beat187, sizeof(npu_infer_beat187));

	int rc = npu_cmd(HLINK_CMD_TENSOR_LOAD, npu_infer_tx, NPU_INFER_TX_LEN,
			 &reply, NPU_WAIT_ACK);
	if (rc != 0) {
		return rc;
	}
	if (reply.len < HLINK_TENSOR_LOAD_RSP_LEN) {
		return -EBADMSG;
	}
	if (reply.payload[0] != HLINK_OK) {
		LOG_WRN("NPU infer: TENSOR_LOAD status %u", reply.payload[0]);
		return -EPROTO;
	}
	return 0;
}

static int npu_infer_run(uint16_t *run_id)
{
	struct hlink_frame reply;
	uint8_t model = HLINK_MODEL_IDX_ACTIVE;

	int rc = npu_cmd(HLINK_CMD_RUN, &model, 1, &reply, NPU_WAIT_REPLY);
	if (rc != 0) {
		return rc;
	}
	if (reply.len < HLINK_RUN_RSP_LEN) {
		return -EBADMSG;
	}
	if (reply.payload[0] != HLINK_OK) {
		LOG_WRN("NPU infer: RUN status %u", reply.payload[0]);
		return -EPROTO;
	}
	*run_id = sys_get_le16(&reply.payload[2]);
	return 0;
}

static int npu_infer_read(uint16_t run_id, int8_t scores[5], uint8_t *class_id,
			  uint8_t *extra_flags)
{
	struct hlink_read_result_req req = {
		.run_id = run_id,
		.out_offset = 0,
	};
	struct hlink_frame reply;
	int rc = -EAGAIN;

	for (int attempt = 0; attempt < 10; attempt++) {
		if (npu_link_stale()) {
			return -ECANCELED;
		}
		rc = npu_cmd(HLINK_CMD_READ_RESULT, &req, sizeof(req),
			     &reply, NPU_WAIT_REPLY);
		if (rc == -EAGAIN) {
			k_msleep(5);
			continue;
		}
		break;
	}
	if (rc != 0) {
		return rc;
	}
	if (reply.len < HLINK_RESULT_HDR_LEN) {
		return -EBADMSG;
	}

	struct hlink_result_hdr hdr;

	memcpy(&hdr, reply.payload, sizeof(hdr));
	uint16_t n_out = hdr.n_out;
	size_t have = (size_t)reply.len - HLINK_RESULT_HDR_LEN;

	if (n_out > have) {
		n_out = (uint16_t)have;
	}

	memset(scores, 0, 5);
	if (n_out >= 5) {
		memcpy(scores, reply.payload + HLINK_RESULT_HDR_LEN, 5);
	} else if (n_out > 0) {
		memcpy(scores, reply.payload + HLINK_RESULT_HDR_LEN, n_out);
	}

	*class_id = hdr.argmax;
	if (*class_id > 4) {
		uint8_t best_i = 0;
		int8_t best = scores[0];

		for (uint8_t i = 1; i < 5; i++) {
			if (scores[i] > best) {
				best = scores[i];
				best_i = i;
			}
		}
		*class_id = best_i;
	}

	*extra_flags = 0;
	if (hdr.flags & HLINK_RES_LOW_CONF) {
		*extra_flags |= HP6_INF_LOW_CONF;
	}
	return 0;
}

static void npu_infer_work_fn(struct k_work *w)
{
	ARG_UNUSED(w);

	if (npu_link_stale()) {
		return;
	}

	LOG_INF("NPU infer: TENSOR_LOAD 187 B (testset record 0)");

	int rc = npu_infer_load();
	if (rc != 0) {
		if (rc != -ECANCELED) {
			LOG_WRN("NPU infer: TENSOR_LOAD failed (%d)", rc);
		}
		return;
	}

	if (npu_link_stale()) {
		return;
	}

	uint16_t run_id = HLINK_RUN_ID_LATEST;

	rc = npu_infer_run(&run_id);
	if (rc != 0) {
		if (rc != -ECANCELED) {
			LOG_WRN("NPU infer: RUN failed (%d)", rc);
		}
		return;
	}

	int8_t scores[5] = { 0, 0, 0, 0, 0 };
	uint8_t class_id = 0;
	uint8_t extra = 0;

	rc = npu_infer_read(run_id, scores, &class_id, &extra);
	if (rc != 0) {
		if (rc != -ECANCELED) {
			LOG_WRN("NPU infer: READ_RESULT failed (%d)", rc);
		}
		return;
	}

	bool zeros = (scores[0] | scores[1] | scores[2] | scores[3] | scores[4]) == 0;

	npu_infer_publish(scores, class_id, extra, zeros);
}

void npu_infer_on_link_up(void)
{
	if (!npu_infer_inited) {
		k_work_init(&npu_infer_work, npu_infer_work_fn);
		npu_infer_inited = true;
	}
	(void)npu_link_submit(&npu_infer_work);
}

void npu_infer_cancel(void)
{
	if (npu_infer_inited) {
		npu_link_cancel(&npu_infer_work);
	}
}

int npu_infer_selftest(void)
{
	if (!npu_infer_inited) {
		k_work_init(&npu_infer_work, npu_infer_work_fn);
		npu_infer_inited = true;
	}
	return npu_link_submit(&npu_infer_work);
}
