/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Group 64 Transfer-Mode handler (cmd 0x0069) -- thin adapter over
 * transport/msc/transfer_mode. WRITE { on: bool } arms/disarms USB MSC; READ
 * returns { armed: bool }.
 *
 * Compiled only when the MSC class is built (CONFIG_USBD_MSC_CLASS).
 */

#include "hpi_mgmt_group.h"
#include "transport/msc/transfer_mode.h"
#include "control/security/hpi_security.h"

#include <zephyr/logging/log.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zcbor_common.h>
#include <zcbor_decode.h>
#include <zcbor_encode.h>
#include <mgmt/mcumgr/util/zcbor_bulk.h>

LOG_MODULE_DECLARE(hpi_mgmt, LOG_LEVEL_INF);

int hpi_transfer_mode_write(struct smp_streamer *ctxt)
{
	zcbor_state_t *zsd = ctxt->reader->zs;
	zcbor_state_t *zse = ctxt->writer->zs;

	/* §14.3: arming MSC exposes the SD (PHI) for read-write -- gate on unlock. */
	int gate = hpi_security_require_unlocked();
	if (gate != MGMT_ERR_EOK) {
		return gate;
	}

	bool on = false;
	size_t decoded = 0;
	struct zcbor_map_decode_key_val decoders[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("on", zcbor_bool_decode, &on),
	};
	if (zcbor_map_decode_bulk(zsd, decoders, ARRAY_SIZE(decoders), &decoded) != 0) {
		return MGMT_ERR_EINVAL;
	}

	int rc = hpi_transfer_set(on);
	if (rc != 0) {
		/* -ENODEV from arm = no SD card present; surface it distinctly so
		 * the host can tell "insert a card" from a real hardware fault. */
		uint16_t err = (rc == -ENODEV) ? HPI_MGMT_ERR_NO_MEDIA
					       : HPI_MGMT_ERR_HW_FAULT;
		bool ok = smp_add_cmd_err(zse, HPI_MGMT_GROUP_ID, err);
		return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
	}

	bool ok = zcbor_tstr_put_lit(zse, "armed") &&
		  zcbor_bool_put(zse, hpi_transfer_armed());
	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

int hpi_transfer_mode_read(struct smp_streamer *ctxt)
{
	zcbor_state_t *zse = ctxt->writer->zs;
	bool ok = zcbor_tstr_put_lit(zse, "armed") &&
		  zcbor_bool_put(zse, hpi_transfer_armed());
	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}
