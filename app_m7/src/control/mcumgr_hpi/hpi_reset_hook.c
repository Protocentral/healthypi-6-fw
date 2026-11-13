/*
 * Copyright (c) 2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Refuse the stock MCUmgr os/reset while an ESP32-C6 update could be lost to it
 * (L5 control; the decision is the ESP32 update service's).
 *
 * Resetting the M7 holds the C6 in reset (CONFIG_HEALTHYBRIDGE_ESP32_HOLD_
 * RESET_AT_BOOT). Mid-transfer that abandons the update; after COMMIT, while the
 * new image is still PENDING_VERIFY, it rolls the image back. Both are what
 * hpi_esp32_update_busy() reports. A host doing a three-processor update is
 * expected to order its resets around the C6 (docs/MCUMGR_COMMANDS.md §5.12);
 * this catches the one that does not.
 *
 * The refusal is a group-64 error, ESP_UPDATE_ACTIVE (274). os_mgmt encodes it
 * itself with smp_add_cmd_err(zse, group, rc) from the group and code this
 * callback hands back -- which is why the code is returned through *rc and *group
 * here and never as a handler return value.
 *
 * {"force": true} bypasses the guard: the host has decided.
 */

#include <zcbor_common.h>   /* must precede os_mgmt_callbacks.h */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/mgmt/mcumgr/mgmt/callbacks.h>
#include <zephyr/mgmt/mcumgr/grp/os_mgmt/os_mgmt_callbacks.h>

#include "hpi_mgmt_group.h"
#include "services/esp32_update_service.h"

LOG_MODULE_DECLARE(hpi_mgmt, LOG_LEVEL_INF);

static enum mgmt_cb_return hpi_reset_cb(uint32_t event, enum mgmt_cb_return prev_status,
					int32_t *rc, uint16_t *group, bool *abort_more,
					void *data, size_t data_size)
{
	ARG_UNUSED(prev_status);
	ARG_UNUSED(abort_more);

	if (event != MGMT_EVT_OP_OS_MGMT_RESET || data == NULL ||
	    data_size < sizeof(struct os_mgmt_reset_data)) {
		return MGMT_CB_OK;
	}

	const struct os_mgmt_reset_data *r = data;

	if (r->force || !hpi_esp32_update_busy()) {
		return MGMT_CB_OK;
	}

	LOG_WRN("os/reset refused: ESP32-C6 update in progress (force:true overrides)");
	*rc = HPI_MGMT_ERR_ESP_UPDATE_ACTIVE;
	*group = HPI_MGMT_GROUP_ID;
	return MGMT_CB_ERROR_ERR;
}

static struct mgmt_callback hpi_reset_hook = {
	.callback = hpi_reset_cb,
	.event_id = MGMT_EVT_OP_OS_MGMT_RESET,
};

static int hpi_reset_hook_init(void)
{
	mgmt_callback_register(&hpi_reset_hook);
	return 0;
}

SYS_INIT(hpi_reset_hook_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
