/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Group 64 unlock/security handlers (0x0010-0x0013, the group-64 catalog/§14):
 *   0x0010 unlock_challenge (read)  -> { nonce: bstr[16], ttl_ms }
 *   0x0011 unlock_response  (write) <- { tag: bstr[32] }  -> { state, ttl_ms }
 *   0x0012 lock             (write) -> { state }
 *   0x0013 lock_state       (read)  -> { state }
 *
 * Thin adapters over control/security/hpi_security. Compiled only when
 * CONFIG_HPI_SECURITY is enabled.
 */

#include "hpi_mgmt_group.h"
#include "control/security/hpi_security.h"

#include <zephyr/logging/log.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zcbor_common.h>
#include <zcbor_decode.h>
#include <zcbor_encode.h>
#include <mgmt/mcumgr/util/zcbor_bulk.h>

LOG_MODULE_DECLARE(hpi_mgmt, LOG_LEVEL_INF);

/* hpi_security_unlock_response() returns 0 or a negative HPI_MGMT_ERR_LOCK_*;
 * surface the positive group error to the host. */
static int put_lock_err(zcbor_state_t *zse, int neg_rc)
{
	uint16_t code = (uint16_t)(-neg_rc);
	bool ok = smp_add_cmd_err(zse, HPI_MGMT_GROUP_ID, code);
	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

int hpi_unlock_challenge(struct smp_streamer *ctxt)
{
	zcbor_state_t *zse = ctxt->writer->zs;
	uint8_t nonce[HPI_UNLOCK_NONCE_LEN];
	uint32_t ttl = 0;

	int rc = hpi_security_unlock_challenge(nonce, &ttl);
	if (rc != 0) {
		/* No secret provisioned (OTP not set / dev hex invalid). */
		return MGMT_ERR_ENOTSUP;
	}
	bool ok = zcbor_tstr_put_lit(zse, "nonce") &&
		  zcbor_bstr_encode_ptr(zse, nonce, sizeof(nonce)) &&
		  zcbor_tstr_put_lit(zse, "ttl_ms") && zcbor_uint32_put(zse, ttl);
	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

int hpi_unlock_response(struct smp_streamer *ctxt)
{
	zcbor_state_t *zsd = ctxt->reader->zs;
	zcbor_state_t *zse = ctxt->writer->zs;

	struct zcbor_string tag = { 0 };
	size_t decoded = 0;
	struct zcbor_map_decode_key_val decoders[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("tag", zcbor_bstr_decode, &tag),
	};
	if (zcbor_map_decode_bulk(zsd, decoders, ARRAY_SIZE(decoders), &decoded) != 0) {
		return MGMT_ERR_EINVAL;
	}

	uint32_t grant = 0;
	int rc = hpi_security_unlock_response(tag.value, tag.len, &grant);
	if (rc != 0) {
		return put_lock_err(zse, rc);
	}
	bool ok = zcbor_tstr_put_lit(zse, "state") &&
		  zcbor_uint32_put(zse, (uint32_t)HPI_LOCK_UNLOCKED) &&
		  zcbor_tstr_put_lit(zse, "ttl_ms") && zcbor_uint32_put(zse, grant);
	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

int hpi_lock_write(struct smp_streamer *ctxt)
{
	zcbor_state_t *zse = ctxt->writer->zs;
	hpi_security_lock();
	bool ok = zcbor_tstr_put_lit(zse, "state") &&
		  zcbor_uint32_put(zse, (uint32_t)HPI_LOCK_LOCKED);
	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

int hpi_lock_state_read(struct smp_streamer *ctxt)
{
	zcbor_state_t *zse = ctxt->writer->zs;
	bool ok = zcbor_tstr_put_lit(zse, "state") &&
		  zcbor_uint32_put(zse, (uint32_t)hpi_security_state());
	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}
