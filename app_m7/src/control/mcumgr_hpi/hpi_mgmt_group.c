/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * HealthyPi custom MCUmgr group 64 -- registration (L5 control).
 *
 * One Zephyr mgmt_group registered via MCUMGR_HANDLER_DEFINE(); each command
 * ID in enum hpi_mgmt_cmd_id maps to an entry in the handler array.
 * Unimplemented IDs return MGMT_ERR_ENOTSUP from the dispatcher, so adding a
 * handler later never forces a renumber.
 *
 * Adapter-parity: these handlers are thin adapters over the same service APIs
 * the shell adapter (control/shell_hpi) calls -- neither reimplements logic.
 */

#include "hpi_mgmt_group.h"

#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/mgmt/mcumgr/mgmt/handlers.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>

LOG_MODULE_REGISTER(hpi_mgmt, CONFIG_HPI_APP_LOG_LEVEL);

/* Implemented in hpi_system.c. */
int hpi_system_device_info(struct smp_streamer *ctxt);
int hpi_system_fw_versions(struct smp_streamer *ctxt);
/* Implemented in hpi_streaming.c. */
int hpi_streaming_start(struct smp_streamer *ctxt);
int hpi_streaming_stop(struct smp_streamer *ctxt);
int hpi_streaming_status(struct smp_streamer *ctxt);
#if defined(CONFIG_USBD_MSC_CLASS)
/* Implemented in hpi_transfer.c. */
int hpi_transfer_mode_write(struct smp_streamer *ctxt);
int hpi_transfer_mode_read(struct smp_streamer *ctxt);
#endif
/* Implemented in hpi_recording.c. */
int hpi_recording_status_read(struct smp_streamer *ctxt);
int hpi_recording_start_write(struct smp_streamer *ctxt);
int hpi_recording_stop_write(struct smp_streamer *ctxt);
#if defined(CONFIG_HPI_M4_UPDATE)
/* Implemented in hpi_m4fw.c. */
int hpi_m4fw_begin(struct smp_streamer *ctxt);
int hpi_m4fw_chunk(struct smp_streamer *ctxt);
int hpi_m4fw_commit(struct smp_streamer *ctxt);
int hpi_m4fw_status(struct smp_streamer *ctxt);
int hpi_m4fw_abort(struct smp_streamer *ctxt);
#endif
#if defined(CONFIG_HPI_RECOVERY_MODE)
/* Implemented in hpi_recovery.c. */
int hpi_recovery_read(struct smp_streamer *ctxt);
int hpi_recovery_write(struct smp_streamer *ctxt);
#endif
/* Implemented in hpi_telemetry.c / hpi_diag.c. */
int hpi_telemetry_read(struct smp_streamer *ctxt);
int hpi_diag_run_selftest(struct smp_streamer *ctxt);
int hpi_diag_lead_off_read(struct smp_streamer *ctxt);
/* Implemented in hpi_modules.c. */
int hpi_module_list_read(struct smp_streamer *ctxt);
int hpi_module_power_write(struct smp_streamer *ctxt);
#if defined(CONFIG_HPI_SECURITY)
/* Implemented in hpi_security_cmd.c. */
int hpi_unlock_challenge(struct smp_streamer *ctxt);
int hpi_unlock_response(struct smp_streamer *ctxt);
int hpi_lock_write(struct smp_streamer *ctxt);
int hpi_lock_state_read(struct smp_streamer *ctxt);
#endif
#if defined(CONFIG_HPI_CONNECTIVITY)
/* Implemented in hpi_wifi.c. */
int hpi_wifi_status_read(struct smp_streamer *ctxt);
int hpi_wifi_scan_read(struct smp_streamer *ctxt);
int hpi_wifi_set_write(struct smp_streamer *ctxt);
int hpi_wifi_forget_write(struct smp_streamer *ctxt);
int hpi_wifi_softap_write(struct smp_streamer *ctxt);
int hpi_conn_enable_write(struct smp_streamer *ctxt);
int hpi_conn_disable_write(struct smp_streamer *ctxt);
int hpi_conn_status_read(struct smp_streamer *ctxt);
#endif

/* Handler table; array index = command ID within group 64. Holes yield
 * MGMT_ERR_ENOTSUP. Sized to (max implemented id + 1). */
static const struct mgmt_handler hpi_mgmt_group_handlers[] = {
	[HPI_MGMT_CMD_DEVICE_INFO]   = { .mh_read = hpi_system_device_info,
					 .mh_write = NULL },
#if defined(CONFIG_HPI_SECURITY)
	[HPI_MGMT_CMD_UNLOCK_CHALLENGE] = { .mh_read = hpi_unlock_challenge,
					    .mh_write = NULL },
	[HPI_MGMT_CMD_UNLOCK_RESPONSE]  = { .mh_read = NULL,
					    .mh_write = hpi_unlock_response },
	[HPI_MGMT_CMD_LOCK]             = { .mh_read = NULL,
					    .mh_write = hpi_lock_write },
	[HPI_MGMT_CMD_LOCK_STATE]       = { .mh_read = hpi_lock_state_read,
					    .mh_write = NULL },
#endif
	[HPI_MGMT_CMD_STREAM_START]  = { .mh_read = NULL,
					 .mh_write = hpi_streaming_start },
	[HPI_MGMT_CMD_STREAM_STOP]   = { .mh_read = NULL,
					 .mh_write = hpi_streaming_stop },
	[HPI_MGMT_CMD_STREAM_STATUS] = { .mh_read = hpi_streaming_status,
					 .mh_write = NULL },
	[HPI_MGMT_CMD_FW_VERSIONS]   = { .mh_read = hpi_system_fw_versions,
					 .mh_write = NULL },
#if defined(CONFIG_HPI_M4_UPDATE)
	/* The M4 update path -- the M4 is not an MCUboot image, so it has its own upload
	 * path rather than the stock img group. See hpi_m4fw.c. */
	[HPI_MGMT_CMD_M4FW_BEGIN]    = { .mh_read = NULL,
					 .mh_write = hpi_m4fw_begin },
	[HPI_MGMT_CMD_M4FW_CHUNK]    = { .mh_read = NULL,
					 .mh_write = hpi_m4fw_chunk },
	[HPI_MGMT_CMD_M4FW_COMMIT]   = { .mh_read = NULL,
					 .mh_write = hpi_m4fw_commit },
	[HPI_MGMT_CMD_M4FW_STATUS]   = { .mh_read = hpi_m4fw_status,
					 .mh_write = NULL },
	[HPI_MGMT_CMD_M4FW_ABORT]    = { .mh_read = NULL,
					 .mh_write = hpi_m4fw_abort },
#endif
#if defined(CONFIG_HPI_RECOVERY_MODE)
	[HPI_MGMT_CMD_ENTER_RECOVERY] = { .mh_read = hpi_recovery_read,
					  .mh_write = hpi_recovery_write },
#endif
	[HPI_MGMT_CMD_TELEMETRY]     = { .mh_read = hpi_telemetry_read,
					 .mh_write = NULL },
	[HPI_MGMT_CMD_DIAG_RUN_SELFTEST] = { .mh_read = hpi_diag_run_selftest,
					     .mh_write = NULL },
	[HPI_MGMT_CMD_DIAG_LEAD_OFF] = { .mh_read = hpi_diag_lead_off_read,
					 .mh_write = NULL },
	[HPI_MGMT_CMD_MODULE_LIST]   = { .mh_read = hpi_module_list_read,
					 .mh_write = NULL },
	[HPI_MGMT_CMD_MODULE_POWER]  = { .mh_read = NULL,
					 .mh_write = hpi_module_power_write },
	[HPI_MGMT_CMD_SD_STATUS]       = { .mh_read = hpi_recording_status_read,
					   .mh_write = NULL },
	[HPI_MGMT_CMD_SD_RECORD_START] = { .mh_read = NULL,
					   .mh_write = hpi_recording_start_write },
	[HPI_MGMT_CMD_SD_RECORD_STOP]  = { .mh_read = NULL,
					   .mh_write = hpi_recording_stop_write },
#if defined(CONFIG_USBD_MSC_CLASS)
	[HPI_MGMT_CMD_TRANSFER_MODE] = { .mh_read = hpi_transfer_mode_read,
					 .mh_write = hpi_transfer_mode_write },
#endif
#if defined(CONFIG_HPI_CONNECTIVITY)
	[HPI_MGMT_CMD_WIFI_STATUS]   = { .mh_read = hpi_wifi_status_read,
					 .mh_write = NULL },
	[HPI_MGMT_CMD_WIFI_SCAN]     = { .mh_read = hpi_wifi_scan_read,
					 .mh_write = NULL },
	[HPI_MGMT_CMD_WIFI_SET]      = { .mh_read = NULL,
					 .mh_write = hpi_wifi_set_write },
	[HPI_MGMT_CMD_WIFI_FORGET]   = { .mh_read = NULL,
					 .mh_write = hpi_wifi_forget_write },
	[HPI_MGMT_CMD_WIFI_SOFTAP]   = { .mh_read = NULL,
					 .mh_write = hpi_wifi_softap_write },
	[HPI_MGMT_CMD_CONN_ENABLE]   = { .mh_read = NULL,
					 .mh_write = hpi_conn_enable_write },
	[HPI_MGMT_CMD_CONN_DISABLE]  = { .mh_read = NULL,
					 .mh_write = hpi_conn_disable_write },
	[HPI_MGMT_CMD_CONN_STATUS]   = { .mh_read = hpi_conn_status_read,
					 .mh_write = NULL },
#endif
};

static struct mgmt_group hpi_mgmt_group = {
	.mg_handlers = hpi_mgmt_group_handlers,
	.mg_handlers_count = ARRAY_SIZE(hpi_mgmt_group_handlers),
	.mg_group_id = HPI_MGMT_GROUP_ID,
#ifdef CONFIG_MCUMGR_GRP_ENUM_DETAILS_NAME
	.mg_group_name = "hpi mgmt",
#endif
};

static void hpi_mgmt_register_group(void)
{
	mgmt_register_group(&hpi_mgmt_group);
	LOG_INF("hpi mgmt group %u registered (schema v%u.%u)",
		HPI_MGMT_GROUP_ID,
		(HPI_MGMT_SCHEMA_VERSION >> 8) & 0xFFU,
		HPI_MGMT_SCHEMA_VERSION & 0xFFU);
}

MCUMGR_HANDLER_DEFINE(hpi_mgmt, hpi_mgmt_register_group);
