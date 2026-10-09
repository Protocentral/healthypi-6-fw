/*
 * Copyright (c) 2026 ProtoCentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * WS4 / PR9: MODEL_LIST and MODEL_ACTIVATE over SPI4. No group-64 wrapper,
 * no USART, no FILE_*. .hlm load stays on the module USB CDC.
 *
 * MODEL_ACTIVATE is queued on the module; the reply is not "active". This
 * file watches STATUS.active_name. Re-activating the already-active model
 * is a no-op there (hlc_registry_activate) and is how the round-trip is
 * proven when beat_classifier is already bound.
 */

#include "npu_models.h"
#include "npu_link.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(npu_models, CONFIG_HPI_APP_LOG_LEVEL);

#define NPU_MODELS_MAX      8
#define NPU_MODELS_WAIT_MS  5000
#define NPU_MODELS_POLL_MS  100
#define NPU_MODEL_DEFAULT   "beat_classifier"

static void field_str(char *dst, size_t dst_len, const char *src, size_t src_len)
{
	size_t n = src_len;

	if (dst_len == 0) {
		return;
	}
	if (n >= dst_len) {
		n = dst_len - 1;
	}
	memcpy(dst, src, n);
	dst[n] = '\0';
}

static int npu_models_list(struct hlink_model_list_entry *ents, int cap,
			   uint8_t *total_out)
{
	uint8_t start = 0;
	int n_got = 0;
	uint8_t total = 0;

	do {
		struct hlink_frame reply;
		int rc = npu_cmd(HLINK_CMD_MODEL_LIST, &start, 1, &reply,
				 NPU_WAIT_REPLY);

		if (rc != 0) {
			return rc;
		}
		if (reply.len < HLINK_MODEL_LIST_HDR_LEN) {
			return -EBADMSG;
		}

		struct hlink_model_list_hdr hdr;

		memcpy(&hdr, reply.payload, sizeof(hdr));
		total = hdr.total;

		uint8_t count = hdr.count;
		size_t need = (size_t)HLINK_MODEL_LIST_HDR_LEN +
			      (size_t)count * HLINK_MODEL_LIST_ENTRY_LEN;

		if (reply.len < need) {
			return -EBADMSG;
		}

		for (uint8_t i = 0; i < count && n_got < cap; i++) {
			memcpy(&ents[n_got],
			       reply.payload + HLINK_MODEL_LIST_HDR_LEN +
				       (size_t)i * HLINK_MODEL_LIST_ENTRY_LEN,
			       sizeof(ents[n_got]));
			n_got++;
		}

		start = (uint8_t)(hdr.start + hdr.count);
		if ((reply.flags & HLINK_FLAG_MORE) == 0 || start >= total ||
		    n_got >= cap) {
			break;
		}
		if (npu_link_stale()) {
			return -ECANCELED;
		}
	} while (true);

	if (total_out != NULL) {
		*total_out = total;
	}
	return n_got;
}

static int npu_models_activate(const char *name)
{
	char req[HLINK_NAME_LEN];

	memset(req, 0, sizeof(req));
	strncpy(req, name, sizeof(req) - 1);

	struct hlink_frame reply;
	int rc = npu_cmd(HLINK_CMD_MODEL_ACTIVATE, req, sizeof(req),
			 &reply, NPU_WAIT_REPLY);

	if (rc != 0) {
		return rc;
	}
	if (reply.len < 1 || reply.payload[0] != HLINK_OK) {
		LOG_WRN("NPU models: ACTIVATE '%s' status %u", name,
			reply.len ? reply.payload[0] : 0xff);
		return -EPROTO;
	}
	return 0;
}

static int npu_models_wait_active(const char *want, char *got, size_t got_len)
{
	int64_t deadline = k_uptime_get() + NPU_MODELS_WAIT_MS;

	while (k_uptime_get() < deadline) {
		if (npu_link_stale()) {
			return -ECANCELED;
		}

		struct hlink_frame reply;
		int rc = npu_cmd(HLINK_CMD_STATUS, NULL, 0, &reply,
				 NPU_WAIT_REPLY);

		if (rc != 0) {
			return rc;
		}
		if (reply.len < HLINK_STATUS_LEN) {
			return -EBADMSG;
		}

		field_str(got, got_len,
			  (const char *)&reply.payload[HLINK_ST_OFF_ACTIVE_NAME],
			  HLINK_NAME_LEN);

		uint8_t idx = reply.payload[HLINK_ST_OFF_ACTIVE_IDX];

		if (idx != HLINK_ACTIVE_IDX_NONE && got[0] != '\0' &&
		    strncmp(got, want, HLINK_NAME_LEN) == 0) {
			return 0;
		}
		k_msleep(NPU_MODELS_POLL_MS);
	}

	LOG_WRN("NPU models: STATUS.active_name did not become '%s' in %d ms",
		want, NPU_MODELS_WAIT_MS);
	return -ETIMEDOUT;
}

int npu_models_sync(char *active_out, size_t active_len)
{
	struct hlink_model_list_entry ents[NPU_MODELS_MAX];
	uint8_t total = 0;
	int n;

	if (npu_link_stale()) {
		return -ECANCELED;
	}

	n = npu_models_list(ents, NPU_MODELS_MAX, &total);
	if (n < 0) {
		if (n != -ECANCELED) {
			LOG_WRN("NPU models: MODEL_LIST failed (%d)", n);
		}
		return n;
	}

	LOG_INF("NPU models: %u total (%d in this page set)", total, n);

	int active_i = -1;
	int default_i = -1;
	int ok_i = -1;

	for (int i = 0; i < n; i++) {
		char name[HLINK_NAME_LEN + 1];
		char ver[HLINK_VER_LEN + 1];

		field_str(name, sizeof(name), ents[i].name, HLINK_NAME_LEN);
		field_str(ver, sizeof(ver), ents[i].version, HLINK_VER_LEN);
		LOG_INF("NPU models: [%d] '%s' ver='%s' size=%u ok=%u active=%u",
			i, name, ver, ents[i].size, ents[i].ok, ents[i].active);

		if (ents[i].active && active_i < 0) {
			active_i = i;
		}
		if (strcmp(name, NPU_MODEL_DEFAULT) == 0) {
			default_i = i;
		}
		if (ents[i].ok == 0 && ok_i < 0) {
			ok_i = i;
		}
	}

	int pick = -1;

	if (active_i >= 0) {
		pick = active_i;
	} else if (default_i >= 0) {
		pick = default_i;
	} else if (ok_i >= 0) {
		pick = ok_i;
	} else if (n > 0) {
		pick = 0;
	}

	if (pick < 0) {
		LOG_WRN("NPU models: registry empty");
		return -ENOENT;
	}

	char want[HLINK_NAME_LEN + 1];

	field_str(want, sizeof(want), ents[pick].name, HLINK_NAME_LEN);
	LOG_INF("NPU models: ACTIVATE '%s'%s", want,
		ents[pick].active ? " (already active; round-trip)" : "");

	int rc = npu_models_activate(want);

	if (rc != 0) {
		if (rc != -ECANCELED) {
			LOG_WRN("NPU models: ACTIVATE '%s' failed (%d)", want, rc);
		}
		return rc;
	}

	char got[HLINK_NAME_LEN + 1];

	rc = npu_models_wait_active(want, got, sizeof(got));
	if (rc != 0) {
		return rc;
	}

	LOG_INF("NPU models: active='%s'", got);
	if (active_out != NULL && active_len > 0) {
		field_str(active_out, active_len, got, strlen(got));
	}
	return 0;
}
