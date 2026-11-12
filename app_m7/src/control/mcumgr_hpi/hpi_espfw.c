/*
 * Copyright (c) 2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Group 64 ESP32-C6 firmware-update handlers -- thin adapters over the ESP32
 * update service (services/esp32_update_service.h). No update logic lives here.
 *
 *   0x00A6 hpi/espfw_begin  { len, sha, sig }  -> verify signature, start session
 *   0x00A7 hpi/espfw_chunk  { off, data }      -> relay to the C6, wait for its ack
 *   0x00A8 hpi/espfw_commit                     -> check length + digest, send COMMIT
 *   0x00A9 hpi/espfw_status -> { st, len, rx, err, c6_err, c6_st, img, other,
 *                                slot, ver, valid, pend, busy }
 *   0x00AA hpi/espfw_abort                      -> ABORT (before COMMIT only)
 *
 * begin and commit return at once: the session runs on the service's thread
 * and the host polls espfw_status. chunk blocks for at most
 * HPI_ESPFW_CHUNK_BOUND_MS (1.6 s) waiting for the C6's ack -- a host needs a
 * per-request timeout above that (5 s is the documented figure).
 */

#include "hpi_mgmt_group.h"
#include "services/esp32_update_service.h"
#include "control/security/hpi_security.h"

#include <zephyr/logging/log.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zcbor_common.h>
#include <zcbor_decode.h>
#include <zcbor_encode.h>
#include <mgmt/mcumgr/util/zcbor_bulk.h>
#include <errno.h>
#include <string.h>

LOG_MODULE_DECLARE(hpi_mgmt, LOG_LEVEL_INF);

static int espfw_err(zcbor_state_t *zse, uint16_t code)
{
	bool ok = smp_add_cmd_err(zse, HPI_MGMT_GROUP_ID, code);

	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

/* Map a service errno onto the group-64 codes so the host can tell a bad image
 * from a sequencing mistake from a silent C6. -EIO means "the C6 refused" only
 * on the chunk path (c6_path); elsewhere it is a local crypto failure. */
static int espfw_err_reply(zcbor_state_t *zse, int rc, bool c6_path)
{
	uint16_t code;

	switch (rc) {
	case -EBADMSG:   code = HPI_MGMT_ERR_IMAGE_INVALID;    break; /* signature / digest */
	case -ENOSPC:    code = HPI_MGMT_ERR_IMAGE_TOO_LARGE;  break;
	case -EBUSY:     code = HPI_MGMT_ERR_BUSY;             break;
	case -EINVAL:    code = HPI_MGMT_ERR_TRANSFER_INVALID; break;
	case -ENOTSUP:   code = HPI_MGMT_ERR_NOT_READY;        break;
	case -ETIMEDOUT: code = HPI_MGMT_ERR_ESP_NO_REPLY;     break;
	case -EIO:
		code = c6_path ? HPI_MGMT_ERR_ESP_REFUSED : HPI_MGMT_ERR_HW_FAULT;
		break;
	default:         code = HPI_MGMT_ERR_HW_FAULT;         break; /* -ENODEV, -EHOSTDOWN */
	}
	return espfw_err(zse, code);
}

int hpi_espfw_begin(struct smp_streamer *ctxt)
{
	zcbor_state_t *zsd = ctxt->reader->zs;
	zcbor_state_t *zse = ctxt->writer->zs;

	/* Writing the co-processor's firmware is gated like the M4's. */
	int gate = hpi_security_require_unlocked();
	if (gate != MGMT_ERR_EOK) {
		return gate;
	}

	uint32_t len = 0;
	struct zcbor_string sha = {0};
	struct zcbor_string sig = {0};
	size_t decoded = 0;

	struct zcbor_map_decode_key_val decoders[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("len", zcbor_uint32_decode, &len),
		ZCBOR_MAP_DECODE_KEY_DECODER("sha", zcbor_bstr_decode,   &sha),
		/* Required, unlike m4fw_begin's: there is no unsigned mode. */
		ZCBOR_MAP_DECODE_KEY_DECODER("sig", zcbor_bstr_decode,   &sig),
	};

	if (zcbor_map_decode_bulk(zsd, decoders, ARRAY_SIZE(decoders), &decoded) != 0 ||
	    sha.len != HPI_ESPFW_SHA256_LEN) {
		return MGMT_ERR_EINVAL;
	}

	int rc = hpi_esp32_update_begin(len, sha.value, sig.len ? sig.value : NULL, sig.len);
	if (rc != 0) {
		LOG_WRN("espfw: begin refused (%d)", rc);
		return espfw_err_reply(zse, rc, false);
	}
	/* Accepted, not ready: poll espfw_status until st = 2 (receiving). */
	return MGMT_ERR_EOK;
}

int hpi_espfw_chunk(struct smp_streamer *ctxt)
{
	zcbor_state_t *zsd = ctxt->reader->zs;
	zcbor_state_t *zse = ctxt->writer->zs;

	int gate = hpi_security_require_unlocked();
	if (gate != MGMT_ERR_EOK) {
		return gate;
	}

	uint32_t off = 0;
	struct zcbor_string data = {0};
	size_t decoded = 0;

	struct zcbor_map_decode_key_val decoders[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("off",  zcbor_uint32_decode, &off),
		ZCBOR_MAP_DECODE_KEY_DECODER("data", zcbor_bstr_decode,   &data),
	};

	if (zcbor_map_decode_bulk(zsd, decoders, ARRAY_SIZE(decoders), &decoded) != 0 ||
	    data.len == 0) {
		return MGMT_ERR_EINVAL;
	}

	uint32_t next = 0;
	int rc = hpi_esp32_update_chunk(off, data.value, data.len, &next);

	if (rc != 0) {
		return espfw_err_reply(zse, rc, true);
	}
	return zcbor_tstr_put_lit(zse, "off") && zcbor_uint32_put(zse, next)
		? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

int hpi_espfw_commit(struct smp_streamer *ctxt)
{
	zcbor_state_t *zse = ctxt->writer->zs;

	int gate = hpi_security_require_unlocked();
	if (gate != MGMT_ERR_EOK) {
		return gate;
	}

	int rc = hpi_esp32_update_commit();

	if (rc == -EINVAL) {
		return espfw_err(zse, HPI_MGMT_ERR_IMAGE_INCOMPLETE);
	}
	if (rc != 0) {
		LOG_WRN("espfw: commit refused (%d) -- ABORT sent, C6 untouched", rc);
		return espfw_err_reply(zse, rc, false);
	}
	/* COMMIT is on its way; the outcome arrives in espfw_status. */
	return MGMT_ERR_EOK;
}

int hpi_espfw_status(struct smp_streamer *ctxt)
{
	zcbor_state_t *zse = ctxt->writer->zs;
	struct hpi_espfw_status st;

	hpi_esp32_update_status(&st);

	bool ok =
		zcbor_tstr_put_lit(zse, "st")     && zcbor_uint32_put(zse, st.state) &&
		zcbor_tstr_put_lit(zse, "len")    && zcbor_uint32_put(zse, st.size) &&
		zcbor_tstr_put_lit(zse, "rx")     && zcbor_uint32_put(zse, st.relayed) &&
		zcbor_tstr_put_lit(zse, "err")    && zcbor_uint32_put(zse, st.err) &&
		zcbor_tstr_put_lit(zse, "c6_err") && zcbor_uint32_put(zse, st.c6_err) &&
		zcbor_tstr_put_lit(zse, "c6_st")  && zcbor_uint32_put(zse, st.c6_state) &&
		zcbor_tstr_put_lit(zse, "img")    && zcbor_uint32_put(zse, st.img) &&
		zcbor_tstr_put_lit(zse, "other")  && zcbor_uint32_put(zse, st.other) &&
		zcbor_tstr_put_lit(zse, "slot")   && zcbor_uint32_put(zse, st.slot) &&
		zcbor_tstr_put_lit(zse, "ver")    &&
		zcbor_tstr_put_term(zse, st.version, sizeof(st.version)) &&
		/* Derived from img, spelled out so a host need not know the
		 * HealthyBridge image-state numbering to ask the two questions
		 * that matter. */
		zcbor_tstr_put_lit(zse, "valid")  && zcbor_bool_put(zse, st.img == 0U) &&
		zcbor_tstr_put_lit(zse, "pend")   && zcbor_bool_put(zse, st.img == 1U) &&
		zcbor_tstr_put_lit(zse, "busy")   && zcbor_bool_put(zse, st.busy);

	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

int hpi_espfw_abort(struct smp_streamer *ctxt)
{
	zcbor_state_t *zse = ctxt->writer->zs;

	int rc = hpi_esp32_update_abort();

	if (rc != 0) {
		/* -EBUSY: COMMIT has been sent; too late, and the watch must run
		 * to the end. -ENOTSUP: not built. */
		return espfw_err_reply(zse, rc, false);
	}
	return MGMT_ERR_EOK;
}
