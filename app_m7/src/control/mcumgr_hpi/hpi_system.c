/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Group 64 -- system/metadata handlers (L5 control).
 *
 * hpi/device_info (cmd 0x0001) -- Returns a CBOR map with the
 * HealthyPi-specific fields stock `os/info` cannot carry (multi-core firmware
 * versions, board revision, group-64 schema version, raw STM32 UID, uptime).
 */

#include "hpi_mgmt_group.h"
#include "platform/ipc.h"

#include <string.h>

#include <app_version.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zcbor_common.h>
#include <zcbor_encode.h>

LOG_MODULE_DECLARE(hpi_mgmt, LOG_LEVEL_INF);

/* Hex-encode the low 8 bytes of the STM32 UID into a 16-char serial. */
static void fill_serial_number_hex(char out[17])
{
	uint8_t uid[16] = {0};
	ssize_t n = hwinfo_get_device_id(uid, sizeof(uid));

	if (n < 0) {
		strcpy(out, "unknown");
		return;
	}
	static const char hex[] = "0123456789ABCDEF";
	size_t start = (n >= 8) ? (size_t)(n - 8) : 0;
	size_t have = (n >= 8) ? 8 : (size_t)n;

	for (size_t i = 0; i < have; i++) {
		out[i * 2 + 0] = hex[(uid[start + i] >> 4) & 0xF];
		out[i * 2 + 1] = hex[uid[start + i] & 0xF];
	}
	out[have * 2] = '\0';
}

/*
 * Board revision as a short tag ("v5"), derived from CONFIG_BOARD rather than
 * hardcoded, so a future healthypi6_v6 is right for free.
 */
static const char *hpi_board_rev(void)
{
	const char *board = CONFIG_BOARD;
	const char *last = NULL;

	/* Take the final "_v<something>" so "healthypi6_v5" -> "v5". */
	for (const char *p = board; p[0] != '\0' && p[1] != '\0'; p++) {
		if (p[0] == '_' && p[1] == 'v') {
			last = p + 1;
		}
	}
	return last != NULL ? last : board;
}

int hpi_system_device_info(struct smp_streamer *ctxt)
{
	zcbor_state_t *zse = ctxt->writer->zs;

	char serial[17] = {0};
	fill_serial_number_hex(serial);

	uint8_t hw[16] = {0};
	ssize_t hw_len = hwinfo_get_device_id(hw, sizeof(hw));
	if (hw_len < 0) {
		hw_len = 0;
	}

	const char *m7fw  = APP_VERSION_STRING;
	/* Reported by the M4 over IPC at bind; empty until it has. */
	const char *m4fw  = hpi_ipc_m4_version();
	/* TODO(D6): needs a HealthyBridge info field, which is the ESP->M7
	 * direction -- blocked on the dead MISO return path. Empty string keeps
	 * the response well-formed. */
	const char *espfw = "";
	const char *board_rev = hpi_board_rev();

	uint32_t uptime_s = k_uptime_get_32() / 1000U;

	bool ok =
		zcbor_tstr_put_lit(zse, "sn")    && zcbor_tstr_put_term(zse, serial, sizeof(serial)) &&
		zcbor_tstr_put_lit(zse, "fw")    && zcbor_tstr_put_term(zse, m7fw, 32) &&
		zcbor_tstr_put_lit(zse, "gv")    && zcbor_uint32_put(zse, HPI_MGMT_SCHEMA_VERSION) &&
		zcbor_tstr_put_lit(zse, "br")    && zcbor_tstr_put_term(zse, board_rev, 8) &&
		zcbor_tstr_put_lit(zse, "hw")    && zcbor_bstr_encode_ptr(zse, hw, (size_t)hw_len) &&
		zcbor_tstr_put_lit(zse, "m4fw")  && zcbor_tstr_put_term(zse, m4fw, 32) &&
		zcbor_tstr_put_lit(zse, "espfw") && zcbor_tstr_put_term(zse, espfw, 32) &&
		zcbor_tstr_put_lit(zse, "up")    && zcbor_uint32_put(zse, uptime_s);

	if (!ok) {
		LOG_ERR("device_info CBOR encode failed");
		return MGMT_ERR_EMSGSIZE;
	}
	return MGMT_ERR_EOK;
}

/* hpi/fw_versions (cmd 0x0031) -- M7 from the build; M4/ESP32/module
 * strings populate from IPC / HealthyBridge / EEPROM in later phases. Empty
 * strings keep the schema stable until then. */
int hpi_system_fw_versions(struct smp_streamer *ctxt)
{
	zcbor_state_t *zse = ctxt->writer->zs;

	const char *m7fw  = APP_VERSION_STRING;
	const char *m4fw  = hpi_ipc_m4_version();
	/* TODO(D6): espfw needs a HealthyBridge info field (ESP->M7), blocked on
	 * the dead MISO return path. mod_a/mod_b need a version field in the
	 * HealthyLink module contract, which does not exist yet. */
	const char *espfw = "";
	const char *mod_a = "";
	const char *mod_b = "";

	bool ok =
		zcbor_tstr_put_lit(zse, "m7fw")     && zcbor_tstr_put_term(zse, m7fw, 32) &&
		zcbor_tstr_put_lit(zse, "m4fw")     && zcbor_tstr_put_term(zse, m4fw, 32) &&
		zcbor_tstr_put_lit(zse, "espfw")    && zcbor_tstr_put_term(zse, espfw, 32) &&
		zcbor_tstr_put_lit(zse, "mod_a_fw") && zcbor_tstr_put_term(zse, mod_a, 32) &&
		zcbor_tstr_put_lit(zse, "mod_b_fw") && zcbor_tstr_put_term(zse, mod_b, 32);

	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}
