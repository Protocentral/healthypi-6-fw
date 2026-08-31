/*
 * Copyright (c) 2024-2026 ProtoCentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * HealthyPi 6 custom MCUmgr group 64.
 *
 * This is the device-side source of truth for HealthyPi-specific
 * command IDs inside management group 64 ("hpi"); the host-facing
 * reference is docs/MCUMGR_COMMANDS.md.
 *
 * Not every declared ID is routed: the enum reserves every ID so
 * handlers can be added without renumbering.
 */

#ifndef HPI_MGMT_GROUP_H
#define HPI_MGMT_GROUP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MCUmgr user-defined group IDs start at 64 per the SMP protocol spec. */
#define HPI_MGMT_GROUP_ID 64U

/* Group-64 schema version returned by hpi/device_info.gv.
 * (major << 8) | minor — bump minor for additive changes, major for
 * breaking ones. */
#define HPI_MGMT_SCHEMA_VERSION 0x0001U

/* Command IDs within group 64, allocated in subranges by
 * subsystem. */
enum hpi_mgmt_cmd_id {
	/* 0x0000 – 0x000F  System / metadata */
	HPI_MGMT_CMD_DEVICE_INFO       = 0x0001,

	/* 0x0010 – 0x001F  Unlock & security (§14) */
	HPI_MGMT_CMD_UNLOCK_CHALLENGE  = 0x0010,
	HPI_MGMT_CMD_UNLOCK_RESPONSE   = 0x0011,
	HPI_MGMT_CMD_LOCK              = 0x0012,
	HPI_MGMT_CMD_LOCK_STATE        = 0x0013,

	/* 0x0020 – 0x002F  Streaming control */
	HPI_MGMT_CMD_STREAM_START      = 0x0020,
	HPI_MGMT_CMD_STREAM_STOP       = 0x0021,
	HPI_MGMT_CMD_STREAM_STATUS     = 0x0022,

	/* 0x0030 – 0x003F  Telemetry */
	HPI_MGMT_CMD_TELEMETRY         = 0x0030,
	HPI_MGMT_CMD_FW_VERSIONS       = 0x0031,

	/* 0x0050 – 0x005F  HealthyLink modules */
	HPI_MGMT_CMD_MODULE_LIST       = 0x0050,
	HPI_MGMT_CMD_MODULE_INFO       = 0x0051,
	HPI_MGMT_CMD_MODULE_POWER      = 0x0052,

	/* 0x0060 – 0x006F  SD card / recordings */
	HPI_MGMT_CMD_SD_STATUS         = 0x0060,
	HPI_MGMT_CMD_SD_RECORD_START   = 0x0061,
	HPI_MGMT_CMD_SD_RECORD_STOP    = 0x0062,
	HPI_MGMT_CMD_SD_LIST           = 0x0063,
	HPI_MGMT_CMD_SD_DOWNLOAD_BEGIN = 0x0064,
	HPI_MGMT_CMD_SD_DOWNLOAD_CHUNK = 0x0065,
	HPI_MGMT_CMD_SD_DOWNLOAD_END   = 0x0066,
	HPI_MGMT_CMD_SD_DELETE         = 0x0067,
	HPI_MGMT_CMD_SD_FORMAT         = 0x0068,
	HPI_MGMT_CMD_TRANSFER_MODE     = 0x0069,  /* USB MSC arm/disarm */

	/* 0x0070 – 0x007F  WiFi */
	HPI_MGMT_CMD_WIFI_STATUS       = 0x0070,
	HPI_MGMT_CMD_WIFI_SCAN         = 0x0071,
	HPI_MGMT_CMD_WIFI_SET          = 0x0072,
	HPI_MGMT_CMD_WIFI_FORGET       = 0x0073,
	HPI_MGMT_CMD_WIFI_SOFTAP       = 0x0074,
	/* Radio power control -- whether the co-processor is powered at all,
	 * distinct from wifi_* association. The device boots with the C6 in
	 * reset and both radios down; on production builds (shell off) these
	 * commands are the only way to turn it on. */
	HPI_MGMT_CMD_CONN_ENABLE       = 0x0075,
	HPI_MGMT_CMD_CONN_DISABLE      = 0x0076,
	HPI_MGMT_CMD_CONN_STATUS       = 0x0077,

	/* 0x0080 – 0x008F  Diagnostics / self-test */
	HPI_MGMT_CMD_DIAG_RUN_SELFTEST = 0x0080,
	HPI_MGMT_CMD_DIAG_LEAD_OFF     = 0x0081,
	HPI_MGMT_CMD_DIAG_SIGNAL_STATS = 0x0082,
	/* 0x0083 reserved — historically hpi/diag_memory, moved to stat group */

	/* 0x0090 – 0x009F  Logs */
	HPI_MGMT_CMD_LOG_SUBSCRIBE     = 0x0090,
	HPI_MGMT_CMD_LOG_UNSUBSCRIBE   = 0x0091,
	HPI_MGMT_CMD_LOG_GET_BUFFERED  = 0x0092,

	/* 0x00A0 – 0x00AF  M4 firmware update. The M4 is NOT an MCUboot image
	 * (see m4_update_service.h), so it has its own upload path instead of
	 * the stock img group: host -> QSPI staging -> M7 verifies -> M7 writes
	 * bank 2 -> reset. */
	HPI_MGMT_CMD_M4FW_BEGIN        = 0x00A0,
	HPI_MGMT_CMD_M4FW_CHUNK        = 0x00A1,
	HPI_MGMT_CMD_M4FW_COMMIT       = 0x00A2,
	HPI_MGMT_CMD_M4FW_STATUS       = 0x00A3,
	HPI_MGMT_CMD_M4FW_ABORT        = 0x00A4,

	/* Reboot into MCUboot serial recovery over USB CDC. The counterpart to
	 * the bootloader's no-bootable-image entry: this one is for a device that
	 * still runs but needs reflashing. Signed flavor only. */
	HPI_MGMT_CMD_ENTER_RECOVERY    = 0x00A5,

	/* 0x00F0 – 0x00FF  Async notifications (device-initiated) */
	HPI_MGMT_EVT_CHARGE_STATE      = 0x00F0,
	HPI_MGMT_EVT_BATT_LOW          = 0x00F1,
	HPI_MGMT_EVT_USB_PLUG          = 0x00F2,
	HPI_MGMT_EVT_MODULE_INSERTED   = 0x00F3,
	HPI_MGMT_EVT_MODULE_REMOVED    = 0x00F4,
	HPI_MGMT_EVT_WIFI_STATE        = 0x00F5,
	HPI_MGMT_EVT_SD_INSERTED       = 0x00F6,
	HPI_MGMT_EVT_SD_REMOVED        = 0x00F7,
	HPI_MGMT_EVT_SD_ERROR          = 0x00F8,
	HPI_MGMT_EVT_LEAD_OFF          = 0x00F9,
	HPI_MGMT_EVT_BUTTON            = 0x00FA,
	HPI_MGMT_EVT_OVER_TEMP         = 0x00FB,
	HPI_MGMT_EVT_DIAG_RESULT       = 0x00FC,
	HPI_MGMT_EVT_DIAG_COMPLETE     = 0x00FD,
	HPI_MGMT_EVT_REBOOT_PENDING    = 0x00FE,
	HPI_MGMT_EVT_SD_FORMAT_PROGRESS = 0x00FF,
};

/* Group-64 extension error codes start at MGMT_ERR_USER_START (256)
 * */
enum hpi_mgmt_err {
	HPI_MGMT_ERR_NOT_READY             = 256,
	HPI_MGMT_ERR_HW_FAULT              = 257,
	HPI_MGMT_ERR_CHANNEL_NOT_AVAILABLE = 258,
	HPI_MGMT_ERR_INSUFFICIENT_STORAGE  = 259,
	HPI_MGMT_ERR_TRANSFER_INVALID      = 260,
	HPI_MGMT_ERR_VERSION_MISMATCH      = 261,
	HPI_MGMT_ERR_CONFIG_KEY_UNKNOWN    = 262,
	HPI_MGMT_ERR_CONFIG_TYPE_MISMATCH  = 263,
	HPI_MGMT_ERR_LOCK_CHALLENGE_MISSING = 264,
	HPI_MGMT_ERR_LOCK_HMAC_INVALID     = 265,
	HPI_MGMT_ERR_LOCK_EXPIRED          = 266,
	HPI_MGMT_ERR_NO_MEDIA              = 267,
	HPI_MGMT_ERR_IMAGE_INVALID         = 268,  /* digest/signature mismatch */
	HPI_MGMT_ERR_IMAGE_TOO_LARGE       = 269,  /* exceeds staging or bank 2 */
	HPI_MGMT_ERR_BUSY                  = 270,  /* an update is already in flight */
	HPI_MGMT_ERR_IMAGE_NOT_M4          = 271,  /* verifies, but is not an M4 image */
};

#ifdef __cplusplus
}
#endif

#endif /* HPI_MGMT_GROUP_H */
