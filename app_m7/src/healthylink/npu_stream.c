/*
 * Copyright (c) 2026 ProtoCentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Product path: subscribe to bus ECG, STREAM_PUSH lead II as int32 µV, poll
 * streaming results, publish HPI_CH_INFER. The module windows (748 @ 500 Hz
 * -> 187 @ 125 Hz); this file does not. Sliding windows until PR8 supplies
 * STREAM_EVENT from M4 BEAT_NOTIFY.
 *
 * npu_wq only. Scratch is file-static -- a 748-sample int32 window would
 * overflow the 3072 B work-queue stack. Never clear HP6_INF_STUB on a
 * five-zero reply.
 *
 * RESULT_POLL uses NPU_WAIT_ACK (2 ms), not the 50 ms IRQ wait. On this
 * board the IRQ line is already asserted, so NPU_WAIT_REPLY sleeps 50 ms
 * every poll and the ECG ring drops.
 *
 * beat_classifier on the fitted module is event-triggered. Sliding ingest
 * alone leaves runs=0. Until PR8 (M4 BEAT_NOTIFY -> STREAM_EVENT), a
 * synthetic STREAM_EVENT is sent on a 1.5 s period so the windower has a
 * centre. Sliding models ignore beat markers.
 */

#include "npu_stream.h"
#include "npu_link.h"
#include "mod_npu.h"

#include "core/sample_bus.h"
#include "core/sample_formats.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(npu_stream, CONFIG_HPI_APP_LOG_LEVEL);

BUILD_ASSERT(sizeof(struct hp6_infer_sample) == 16, "HPI_CH_INFER payload is 16 B");
BUILD_ASSERT(sizeof(struct hp6_ecg_sample) == 20, "ECG sample is 20 B");

#define NPU_STREAM_RING           32
#define NPU_STREAM_POLL_MS        500
#define NPU_STREAM_EMPTY_SLEEP_MS 5
#define NPU_STREAM_EVENT_MS       1500
#define NPU_STREAM_EVENT_DELAY_MS 1200 /* centre this far in the past so the window is in the ring */
#define NPU_STREAM_EVENT_MIN_ACC  1000
/* 48 int32s = 3 ECG bus frames; 58 fit a 256 B SPI frame after the 12 B hdr. */
#define NPU_STREAM_MAX_N          48

static uint8_t npu_stream_tx[HLINK_STREAM_PUSH_REQ_HDR_LEN +
			     NPU_STREAM_MAX_N * sizeof(int32_t)];
static int32_t npu_stream_samples[NPU_STREAM_MAX_N];
static struct k_work npu_stream_work;
static struct hpi_bus_sub *npu_stream_sub;
static struct k_mutex npu_stream_lock;
static bool npu_stream_inited;
static int64_t npu_stream_last_poll_ms;
static int64_t npu_stream_last_event_ms;
static int npu_stream_window_mode = -1; /* -1 unknown, 0 slide, 1 beat */

static uint32_t npu_stream_pushes;
static uint32_t npu_stream_accepted;
static uint32_t npu_stream_mod_drop;
static uint32_t npu_stream_results;
static uint32_t npu_stream_stub;
static uint32_t npu_stream_poll_err;
static uint32_t npu_stream_events;
static int64_t npu_stream_stats_ms;

static void npu_stream_unsub_locked(void)
{
	if (npu_stream_sub != NULL) {
		hpi_bus_unsubscribe(npu_stream_sub);
		npu_stream_sub = NULL;
	}
}

static bool npu_stream_abort(void)
{
	if (!npu_link_stale()) {
		return false;
	}
	k_mutex_lock(&npu_stream_lock, K_FOREVER);
	npu_stream_unsub_locked();
	k_mutex_unlock(&npu_stream_lock);
	return true;
}

static void npu_stream_publish(const int8_t scores[5], uint8_t class_id,
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

	npu_stream_results++;
	if (stub) {
		npu_stream_stub++;
	}

	LOG_INF("NPU stream: class=%u conf=%u stub=%d scores=%d %d %d %d %d",
		s.class_id, s.confidence, stub ? 1 : 0,
		scores[0], scores[1], scores[2], scores[3], scores[4]);
}

static int npu_stream_push(uint16_t n, uint64_t t_ms)
{
	struct hlink_stream_push_req req;

	if (n == 0 || n > NPU_STREAM_MAX_N) {
		return -EINVAL;
	}

	req.channel = HLINK_STREAM_CH_ECG_II;
	req.format = HLINK_STREAM_FMT_I32;
	req.n = n;
	req.t_ms = t_ms;
	memcpy(npu_stream_tx, &req, sizeof(req));
	memcpy(npu_stream_tx + HLINK_STREAM_PUSH_REQ_HDR_LEN,
	       npu_stream_samples, (size_t)n * sizeof(int32_t));

	struct hlink_frame reply;
	uint16_t plen = (uint16_t)(HLINK_STREAM_PUSH_REQ_HDR_LEN +
				   n * sizeof(int32_t));
	int rc = npu_cmd(HLINK_CMD_STREAM_PUSH, npu_stream_tx, plen,
			 &reply, NPU_WAIT_ACK);

	if (rc != 0) {
		return rc;
	}
	if (reply.len < HLINK_STREAM_PUSH_RSP_LEN) {
		return -EBADMSG;
	}

	struct hlink_stream_push_rsp rsp;

	memcpy(&rsp, reply.payload, sizeof(rsp));
	npu_stream_pushes++;
	npu_stream_accepted += rsp.accepted;
	if (rsp.dropped > 0) {
		npu_stream_mod_drop += rsp.dropped;
		LOG_WRN("NPU stream: module dropped %u samples (accepted %u/%u)",
			rsp.dropped, rsp.accepted, n);
	}
	return 0;
}

static int npu_stream_decode_result(const struct hlink_frame *reply,
				    int8_t scores[5], uint8_t *class_id,
				    uint8_t *extra_flags)
{
	if (reply->len < HLINK_RESULT_HDR_LEN) {
		return -EBADMSG;
	}

	struct hlink_result_hdr hdr;

	memcpy(&hdr, reply->payload, sizeof(hdr));
	uint16_t n_out = hdr.n_out;
	size_t have = (size_t)reply->len - HLINK_RESULT_HDR_LEN;

	if (n_out > have) {
		n_out = (uint16_t)have;
	}

	memset(scores, 0, 5);
	if (n_out >= 5) {
		memcpy(scores, reply->payload + HLINK_RESULT_HDR_LEN, 5);
	} else if (n_out > 0) {
		memcpy(scores, reply->payload + HLINK_RESULT_HDR_LEN, n_out);
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

/* 0 = published one; -ENOENT empty; -EAGAIN pending; else errno. */
static int npu_stream_poll_one(void)
{
	struct hlink_result_poll_req req = { .out_offset = 0 };
	struct hlink_frame reply;
	/* ACK wait: empty RESULT_POLL is an instant error reply. The 50 ms
	 * IRQ path is already-asserted on this board and starves STREAM_PUSH. */
	int rc = npu_cmd(HLINK_CMD_RESULT_POLL, &req, sizeof(req),
			 &reply, NPU_WAIT_ACK);

	if (rc != 0) {
		return rc;
	}

	int8_t scores[5] = { 0, 0, 0, 0, 0 };
	uint8_t class_id = 0;
	uint8_t extra = 0;

	rc = npu_stream_decode_result(&reply, scores, &class_id, &extra);
	if (rc != 0) {
		return rc;
	}

	bool zeros = (scores[0] | scores[1] | scores[2] | scores[3] | scores[4]) == 0;

	npu_stream_publish(scores, class_id, extra, zeros);
	return 0;
}

static void npu_stream_log_model(void)
{
	struct hpi_npu_link_info info;

	if (hpi_npu_link_get(&info) != 0 || !info.status_valid ||
	    info.active_name[0] == '\0') {
		return;
	}

	struct hlink_model_info_req req;

	memset(&req, 0, sizeof(req));
	strncpy(req.name, info.active_name, sizeof(req.name) - 1);
	req.part = 0;

	struct hlink_frame reply;
	int rc = npu_cmd(HLINK_CMD_MODEL_INFO, &req, sizeof(req),
			 &reply, NPU_WAIT_REPLY);

	if (rc != 0 || reply.len < HLINK_MODEL_INFO_PART0_LEN) {
		LOG_WRN("NPU stream: MODEL_INFO failed (%d len=%u)", rc, reply.len);
		return;
	}

	struct hlink_model_info_part0 p0;

	memcpy(&p0, reply.payload, sizeof(p0));
	npu_stream_window_mode = p0.window_mode;
	LOG_INF("NPU stream: model '%s' mode=%u (0=slide 1=beat) win=%u hop=%u ch='%s'",
		p0.name, p0.window_mode, p0.window, p0.hop, p0.channel);
	if (p0.window_mode != 0) {
		LOG_WRN("NPU stream: model is beat-triggered; sending synthetic "
			"STREAM_EVENT until PR8 (M4 BEAT_NOTIFY)");
	}
}

static void npu_stream_maybe_stats(void)
{
	int64_t now = k_uptime_get();

	if (now - npu_stream_stats_ms < 5000) {
		return;
	}
	npu_stream_stats_ms = now;

	struct hpi_bus_sub_stats st = { 0 };

	k_mutex_lock(&npu_stream_lock, K_FOREVER);
	if (npu_stream_sub != NULL) {
		hpi_bus_sub_get_stats(npu_stream_sub, &st);
	}
	k_mutex_unlock(&npu_stream_lock);

	uint16_t runs_ok = 0;
	uint16_t runs_fail = 0;
	uint32_t err_ovr = 0;
	uint32_t err_crc = 0;
	struct hlink_frame reply;
	int rc = npu_cmd(HLINK_CMD_STATUS, NULL, 0, &reply, NPU_WAIT_REPLY);

	if (rc == 0 && reply.len >= HLINK_STATUS_LEN) {
		runs_ok = sys_get_le16(&reply.payload[HLINK_ST_OFF_RUNS_OK]);
		runs_fail = sys_get_le16(&reply.payload[HLINK_ST_OFF_RUNS_FAILED]);
		err_crc = sys_get_le32(&reply.payload[HLINK_ST_OFF_ERR_CRC]);
		err_ovr = sys_get_le32(&reply.payload[HLINK_ST_OFF_ERR_OVERRUN]);
	}

	LOG_INF("NPU stream 5s: push=%u acc=%u mod_drop=%u results=%u stub=%u "
		"bus_drop=%u poll_err=%u ev=%u | mod runs=%u/%u crc=%u ovr=%u",
		npu_stream_pushes, npu_stream_accepted, npu_stream_mod_drop,
		npu_stream_results, npu_stream_stub, st.frames_dropped,
		npu_stream_poll_err, npu_stream_events, runs_ok, runs_fail,
		err_crc, err_ovr);
}

/* Beat-triggered models ignore sliding samples until STREAM_EVENT. M4
 * BEAT_NOTIFY is PR8; until then stamp a beat 1.2 s in the past so the
 * 748-sample window is already in the module ring. Sliding models discard
 * the marker. */
static int npu_stream_maybe_event(void)
{
	if (npu_stream_window_mode == 0) {
		return 0;
	}
	if (npu_stream_accepted < NPU_STREAM_EVENT_MIN_ACC) {
		return 0;
	}

	int64_t now = k_uptime_get();

	if (npu_stream_last_event_ms != 0 &&
	    (now - npu_stream_last_event_ms) < NPU_STREAM_EVENT_MS) {
		return 0;
	}

	uint64_t t_ms = (uint64_t)now;

	if (t_ms > NPU_STREAM_EVENT_DELAY_MS) {
		t_ms -= NPU_STREAM_EVENT_DELAY_MS;
	}

	struct hlink_stream_event_req req = {
		.kind = HLINK_STREAM_EVENT_BEAT,
		.t_ms = t_ms,
	};
	struct hlink_frame reply;
	int rc = npu_cmd(HLINK_CMD_STREAM_EVENT, &req, sizeof(req),
			 &reply, NPU_WAIT_ACK);

	if (rc == -ECANCELED) {
		return rc;
	}
	if (rc != 0) {
		LOG_WRN("NPU stream: STREAM_EVENT failed (%d)", rc);
		return rc;
	}

	npu_stream_last_event_ms = now;
	npu_stream_events++;
	if (npu_stream_events <= 3 || (npu_stream_events % 10u) == 0u) {
		LOG_INF("NPU stream: STREAM_EVENT beat t=%u (synthetic, #%u)",
			(uint32_t)t_ms, npu_stream_events);
	}
	return 0;
}

static void npu_stream_work_fn(struct k_work *w)
{
	ARG_UNUSED(w);

	if (npu_stream_abort()) {
		return;
	}

	uint16_t n_acc = 0;
	uint64_t t0 = 0;

	while (n_acc < NPU_STREAM_MAX_N) {
		struct hpi_sample_frame f;
		int rc;

		if (npu_stream_abort()) {
			return;
		}

		k_mutex_lock(&npu_stream_lock, K_FOREVER);
		if (npu_link_stale() || npu_stream_sub == NULL) {
			npu_stream_unsub_locked();
			k_mutex_unlock(&npu_stream_lock);
			return;
		}
		rc = hpi_bus_pull(npu_stream_sub, &f);
		if (rc != 0 || f.channel != HPI_CH_ECG || f.payload == NULL ||
		    f.sample_count == 0) {
			k_mutex_unlock(&npu_stream_lock);
			break;
		}

		uint16_t n = f.sample_count;
		size_t need = (size_t)n * sizeof(struct hp6_ecg_sample);

		if (need > f.len) {
			n = (uint16_t)(f.len / sizeof(struct hp6_ecg_sample));
		}
		uint16_t room = (uint16_t)(NPU_STREAM_MAX_N - n_acc);

		if (n > room) {
			n = room;
		}

		const struct hp6_ecg_sample *s = f.payload;

		for (uint16_t k = 0; k < n; k++) {
			npu_stream_samples[n_acc + k] = s[k].lead_ii;
		}
		if (n_acc == 0) {
			t0 = f.t_mono_us / 1000ULL;
		}
		n_acc = (uint16_t)(n_acc + n);
		k_mutex_unlock(&npu_stream_lock);
	}

	if (n_acc > 0) {
		int rc = npu_stream_push(n_acc, t0);

		if (rc == -ECANCELED || npu_stream_abort()) {
			return;
		}
		if (rc != 0) {
			LOG_WRN("NPU stream: STREAM_PUSH failed (%d)", rc);
		}
	}

	if (npu_stream_abort()) {
		return;
	}

	if (npu_stream_maybe_event() == -ECANCELED || npu_stream_abort()) {
		return;
	}

	int64_t now = k_uptime_get();

	if ((now - npu_stream_last_poll_ms) >= NPU_STREAM_POLL_MS) {
		npu_stream_last_poll_ms = now;
		for (int i = 0; i < 4; i++) {
			if (npu_stream_abort()) {
				return;
			}
			int rc = npu_stream_poll_one();

			if (rc == -ECANCELED) {
				(void)npu_stream_abort();
				return;
			}
			if (rc == 0) {
				continue;
			}
			if (rc != -ENOENT && rc != -EAGAIN) {
				npu_stream_poll_err++;
				LOG_WRN("NPU stream: RESULT_POLL failed (%d)", rc);
			}
			break;
		}
	}

	npu_stream_maybe_stats();

	if (n_acc == 0) {
		k_msleep(NPU_STREAM_EMPTY_SLEEP_MS);
	}

	if (npu_stream_abort()) {
		return;
	}
	(void)npu_link_submit(&npu_stream_work);
}

void npu_stream_on_link_up(void)
{
	if (!npu_stream_inited) {
		k_mutex_init(&npu_stream_lock);
		k_work_init(&npu_stream_work, npu_stream_work_fn);
		npu_stream_inited = true;
	}

	k_mutex_lock(&npu_stream_lock, K_FOREVER);
	npu_stream_unsub_locked();

	struct hpi_bus_sub_cfg cfg = {
		.name = "npu_stream",
		.channel_mask = HPI_CH_BIT(HPI_CH_ECG),
		.ring_frames = NPU_STREAM_RING,
	};
	static const uint16_t depths[] = { 32, 16, 8 };

	for (size_t i = 0; i < ARRAY_SIZE(depths); i++) {
		cfg.ring_frames = depths[i];
		npu_stream_sub = hpi_bus_subscribe(&cfg);
		if (npu_stream_sub != NULL) {
			if (i > 0) {
				LOG_WRN("NPU stream: ring %u (wanted %u; heap tight)",
					depths[i], depths[0]);
			}
			break;
		}
	}
	k_mutex_unlock(&npu_stream_lock);

	if (npu_stream_sub == NULL) {
		LOG_ERR("NPU stream: subscribe failed");
		return;
	}

	npu_stream_pushes = 0;
	npu_stream_accepted = 0;
	npu_stream_mod_drop = 0;
	npu_stream_results = 0;
	npu_stream_stub = 0;
	npu_stream_poll_err = 0;
	npu_stream_events = 0;
	npu_stream_window_mode = -1;
	npu_stream_last_event_ms = 0;
	npu_stream_last_poll_ms = k_uptime_get();
	npu_stream_stats_ms = npu_stream_last_poll_ms;

	npu_stream_log_model();

	LOG_INF("NPU stream: ECG lead II -> STREAM_PUSH (ring %u)",
		cfg.ring_frames);
	(void)npu_link_submit(&npu_stream_work);
}

void npu_stream_cancel(void)
{
	if (!npu_stream_inited) {
		return;
	}
	npu_link_cancel(&npu_stream_work);
	/* Work fn unsubscribes on stale. If it is not in pull, take the sub
	 * now so a leaked ring does not sit until the next start. Never wait
	 * on SPI -- the lock is not held across npu_cmd. */
	if (k_mutex_lock(&npu_stream_lock, K_MSEC(50)) == 0) {
		npu_stream_unsub_locked();
		k_mutex_unlock(&npu_stream_lock);
	}
}
