/*
 * Copyright (c) 2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Group 64 M4 firmware-update handlers (the M4 update path) -- thin adapters over the
 * M4 update service (services/m4_update_service.h). No update logic lives here.
 *
 *   0x00A0 hpi/m4fw_begin  { len, sha }      -> erase staging, start upload
 *   0x00A1 hpi/m4fw_chunk  { off, data }     -> append (sequential only)
 *   0x00A2 hpi/m4fw_commit                    -> verify digest, write bank 2
 *   0x00A3 hpi/m4fw_status -> { st, len, rx, err, rst }
 *   0x00A4 hpi/m4fw_abort                     -> discard (bank 2 untouched)
 *
 * The M4 is not an MCUboot image, so this exists instead of the stock img group
 * -- see services/m4_update_service.h for why that is forced rather than chosen.
 */

#include "hpi_mgmt_group.h"
#include "services/m4_update_service.h"
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

/* Map service errno onto the group-64 extension codes so the host can tell a
 * bad image from a full staging region from a sequencing mistake. */
static int m4fw_err_reply(zcbor_state_t *zse, int rc)
{
	uint16_t code;

	switch (rc) {
	case -EBADMSG: code = HPI_MGMT_ERR_IMAGE_INVALID;      break;
	case -EILSEQ:  code = HPI_MGMT_ERR_IMAGE_NOT_M4;       break;
	case -ENOSPC:  code = HPI_MGMT_ERR_IMAGE_TOO_LARGE;    break;
	case -EBUSY:   code = HPI_MGMT_ERR_BUSY;               break;
	case -EINVAL:  code = HPI_MGMT_ERR_TRANSFER_INVALID;   break;
	case -ENOTSUP: code = HPI_MGMT_ERR_NOT_READY;          break;
	case -ENODEV:  code = HPI_MGMT_ERR_HW_FAULT;           break;
	default:       code = HPI_MGMT_ERR_HW_FAULT;           break;
	}

	bool ok = smp_add_cmd_err(zse, HPI_MGMT_GROUP_ID, code);
	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

int hpi_m4fw_begin(struct smp_streamer *ctxt)
{
	zcbor_state_t *zsd = ctxt->reader->zs;
	zcbor_state_t *zse = ctxt->writer->zs;

	/* Writing the core that computes vitals is at least as sensitive as the
	 * data it produces -- gate on unlock like the other write paths. */
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
		/* Optional: ECDSA-P256 over "sha", raw r||s. Whether an upload
		 * without it is accepted is a build decision
		 * (CONFIG_HPI_M4_UPDATE_REQUIRE_SIGNATURE), enforced by the
		 * service -- not something an adapter gets to soften. */
		ZCBOR_MAP_DECODE_KEY_DECODER("sig", zcbor_bstr_decode,   &sig),
	};

	if (zcbor_map_decode_bulk(zsd, decoders, ARRAY_SIZE(decoders), &decoded) != 0) {
		return MGMT_ERR_EINVAL;
	}
	if (sha.len != HPI_M4FW_SHA256_LEN) {
		return MGMT_ERR_EINVAL;
	}

	int rc = hpi_m4_update_begin(len, sha.value,
				     sig.len ? sig.value : NULL, sig.len);
	if (rc != 0) {
		return m4fw_err_reply(zse, rc);
	}

	LOG_INF("m4fw: upload started (%u B)", len);
	/* No payload on success. A top-level "rc" key IS the SMP legacy-error
	 * shape (ErrorV1), so emitting {"rc": 0} makes every client parse a
	 * successful reply as an error. Bare MGMT_ERR_EOK, like the others. */
	return MGMT_ERR_EOK;
}

int hpi_m4fw_chunk(struct smp_streamer *ctxt)
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

	int rc = hpi_m4_update_chunk(off, data.value, data.len);
	if (rc != 0) {
		return m4fw_err_reply(zse, rc);
	}

	/* Echo back the next expected offset so the host can resync without a
	 * separate status round-trip on every chunk. */
	struct hpi_m4fw_status st;
	hpi_m4_update_status(&st);

	return zcbor_tstr_put_lit(zse, "off") && zcbor_uint32_put(zse, st.received)
		? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

int hpi_m4fw_commit(struct smp_streamer *ctxt)
{
	zcbor_state_t *zse = ctxt->writer->zs;

	int gate = hpi_security_require_unlocked();
	if (gate != MGMT_ERR_EOK) {
		return gate;
	}

	int rc = hpi_m4_update_commit();
	if (rc != 0) {
		LOG_WRN("m4fw: commit refused (%d) -- bank 2 untouched", rc);
		return m4fw_err_reply(zse, rc);
	}

	LOG_INF("m4fw: committed -- reset required for the M4 to run it");

	/* "rst" tells the host a reset is needed; the reset itself is left to the
	 * host (os/reset) so it can close the session cleanly first. */
	return zcbor_tstr_put_lit(zse, "rst") && zcbor_bool_put(zse, true)
		? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

int hpi_m4fw_status(struct smp_streamer *ctxt)
{
	zcbor_state_t *zse = ctxt->writer->zs;

	struct hpi_m4fw_status st;
	hpi_m4_update_status(&st);

	bool ok =
		zcbor_tstr_put_lit(zse, "st")  && zcbor_uint32_put(zse, (uint32_t)st.state) &&
		zcbor_tstr_put_lit(zse, "len") && zcbor_uint32_put(zse, st.total_size) &&
		zcbor_tstr_put_lit(zse, "rx")  && zcbor_uint32_put(zse, st.received) &&
		zcbor_tstr_put_lit(zse, "err") && zcbor_int32_put(zse, st.err) &&
		zcbor_tstr_put_lit(zse, "rst") && zcbor_bool_put(zse, hpi_m4_update_reset_required()) &&
		/* Lets a host tool find out whether it must sign BEFORE it spends a
		 * minute uploading an image the device will refuse at commit. */
		zcbor_tstr_put_lit(zse, "sig") && zcbor_bool_put(zse, hpi_m4_update_signature_required());

	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

int hpi_m4fw_abort(struct smp_streamer *ctxt)
{
	ARG_UNUSED(ctxt);

	hpi_m4_update_abort();

	/* No payload -- see the note in hpi_m4fw_begin() on why "rc" must not
	 * appear in a success reply. */
	return MGMT_ERR_EOK;
}
