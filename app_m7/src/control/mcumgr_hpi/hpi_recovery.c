/*
 * Copyright (c) 2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Group 64 recovery-mode handler -- a thin adapter over the DFU service
 * (services/dfu_service.h). No recovery logic lives here.
 *
 *   0x00A5 hpi/enter_recovery
 *     read  -> { av, armed }              is recovery possible / already armed
 *     write { arm, rst } -> { armed, rst } arm (or disarm) and optionally reboot
 *
 * On reboot the device comes back as a SINGLE USB CDC port ("HealthyPi 6
 * Recovery", PID 0x0101) speaking the stock MCUmgr img group from MCUboot --
 * not the two-CDC application composite. A host tool must re-enumerate rather
 * than expect its old port to reappear.
 */

#include "hpi_mgmt_group.h"
#include "services/dfu_service.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zcbor_common.h>
#include <zcbor_decode.h>
#include <zcbor_encode.h>
#include <mgmt/mcumgr/util/zcbor_bulk.h>
#include <errno.h>

LOG_MODULE_DECLARE(hpi_mgmt, LOG_LEVEL_INF);

/* The reset cannot happen inside the handler: the SMP reply is only written to
 * the transport after the handler returns, so resetting here would leave the
 * host waiting for a response that was never sent and unable to tell a
 * successful reboot from a crash. Defer it, the same way os_mgmt's reset does. */
static void recovery_reboot_work(struct k_work *work)
{
	ARG_UNUSED(work);
	hpi_dfu_reboot();
}

static K_WORK_DELAYABLE_DEFINE(recovery_reboot, recovery_reboot_work);

#define RECOVERY_REBOOT_DELAY_MS 250

int hpi_recovery_read(struct smp_streamer *ctxt)
{
	zcbor_state_t *zse = ctxt->writer->zs;

	bool ok =
		zcbor_tstr_put_lit(zse, "av")    &&
		zcbor_bool_put(zse, hpi_dfu_recovery_available()) &&
		zcbor_tstr_put_lit(zse, "armed") &&
		zcbor_bool_put(zse, hpi_dfu_recovery_armed());

	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

int hpi_recovery_write(struct smp_streamer *ctxt)
{
	zcbor_state_t *zsd = ctxt->reader->zs;
	zcbor_state_t *zse = ctxt->writer->zs;

	/* Both default true: the common request is the whole gesture, "put this
	 * device in recovery now". Send {"arm": false} to cancel an armed flag,
	 * or {"rst": false} to arm without rebooting yet. */
	bool arm = true;
	bool rst = true;
	size_t decoded = 0;

	struct zcbor_map_decode_key_val decoders[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("arm", zcbor_bool_decode, &arm),
		ZCBOR_MAP_DECODE_KEY_DECODER("rst", zcbor_bool_decode, &rst),
	};

	if (zcbor_map_decode_bulk(zsd, decoders, ARRAY_SIZE(decoders), &decoded) != 0) {
		return MGMT_ERR_EINVAL;
	}

	int rc = arm ? hpi_dfu_recovery_arm() : hpi_dfu_recovery_disarm();

	if (rc != 0) {
		uint16_t code = (rc == -ENOTSUP) ? HPI_MGMT_ERR_NOT_READY
						 : HPI_MGMT_ERR_HW_FAULT;
		LOG_WRN("enter_recovery: refused (%d)", rc);
		return smp_add_cmd_err(zse, HPI_MGMT_GROUP_ID, code)
			? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
	}

	/* Disarming never reboots -- rebooting after a cancel would be the
	 * opposite of what was asked. */
	bool will_reset = arm && rst;

	if (will_reset) {
		k_work_schedule(&recovery_reboot, K_MSEC(RECOVERY_REBOOT_DELAY_MS));
	}

	return zcbor_tstr_put_lit(zse, "armed") && zcbor_bool_put(zse, arm) &&
	       zcbor_tstr_put_lit(zse, "rst")   && zcbor_bool_put(zse, will_reset)
		? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}
