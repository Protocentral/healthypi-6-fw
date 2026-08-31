/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Group 64 WiFi/connectivity handlers (0x0070-0x0077) -- thin adapters over the
 * connectivity service (ESP32 HealthyBridge). Compiled only when
 * CONFIG_HPI_CONNECTIVITY is enabled.
 *
 *   0x0070 wifi_status  (read)  -> { state, rssi, ssid, ip }
 *   0x0071 wifi_scan    (read)  -> ENOTSUP; provisioning is the SoftAP portal
 *   0x0072 wifi_set     (write) <- { ssid, pw } -> { ok }
 *   0x0073 wifi_forget  (write) -> { ok }
 *   0x0074 wifi_softap  (write) -> { ok }  -- open the provisioning captive portal
 *   0x0075 conn_enable  (write) <- { radios } -> { ok }
 *   0x0076 conn_disable (write) -> { ok }
 *   0x0077 conn_status  (read)  -> { link, radios, state, rssi, ssid, ip, ble_* }
 *
 * The device boots with the co-processor in reset and both radios down, so
 * 0x0075 is how a production build turns connectivity on at all -- the shell
 * adapter that could otherwise do it is compiled out of prod.
 */

#include "hpi_mgmt_group.h"
#include "services/connectivity/healthybridge_service.h"
#include "control/security/hpi_security.h"

#include <zephyr/logging/log.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zcbor_common.h>
#include <zcbor_decode.h>
#include <zcbor_encode.h>
#include <mgmt/mcumgr/util/zcbor_bulk.h>
#include <stdio.h>
#include <string.h>

LOG_MODULE_DECLARE(hpi_mgmt, LOG_LEVEL_INF);

int hpi_wifi_status_read(struct smp_streamer *ctxt)
{
	zcbor_state_t *zse = ctxt->writer->zs;

	struct hpi_wifi_info wi = {0};
	int rc = hpi_connectivity_wifi_status(&wi);

	/*
	 * -EHOSTDOWN (powered down on request) and -EAGAIN (still coming up) are
	 * normal states, never a HW fault: answer with a well-formed
	 * DISCONNECTED. conn_status (0x0077) carries the `link` field for a host
	 * that needs to tell the two apart.
	 */
	if (rc == -EHOSTDOWN || rc == -EAGAIN) {
		memset(&wi, 0, sizeof(wi));
		rc = 0;
	}
	if (rc != 0) {
		bool e = smp_add_cmd_err(zse, HPI_MGMT_GROUP_ID, HPI_MGMT_ERR_HW_FAULT);
		return e ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
	}
	char ip[16];
	(void)snprintf(ip, sizeof(ip), "%u.%u.%u.%u", wi.ip[0], wi.ip[1], wi.ip[2], wi.ip[3]);

	bool ok =
		zcbor_tstr_put_lit(zse, "state") && zcbor_uint32_put(zse, wi.state) &&
		zcbor_tstr_put_lit(zse, "rssi")  && zcbor_int32_put(zse, wi.rssi) &&
		zcbor_tstr_put_lit(zse, "ssid")  && zcbor_tstr_put_term(zse, wi.ssid, sizeof(wi.ssid)) &&
		zcbor_tstr_put_lit(zse, "ip")    && zcbor_tstr_put_term(zse, ip, sizeof(ip));
	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

int hpi_wifi_scan_read(struct smp_streamer *ctxt)
{
	ARG_UNUSED(ctxt);
	/* The SPI command channel doesn't surface scan results yet; provisioning is
	 * done via the ESP32 SoftAP captive portal. */
	return MGMT_ERR_ENOTSUP;
}

int hpi_wifi_set_write(struct smp_streamer *ctxt)
{
	zcbor_state_t *zsd = ctxt->reader->zs;
	zcbor_state_t *zse = ctxt->writer->zs;

	/* §14.3: WiFi credentials are privileged -- gate on unlock. */
	int gate = hpi_security_require_unlocked();
	if (gate != MGMT_ERR_EOK) {
		return gate;
	}

	struct zcbor_string ssid = {0}, pw = {0};
	size_t decoded = 0;
	struct zcbor_map_decode_key_val decoders[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("ssid", zcbor_tstr_decode, &ssid),
		ZCBOR_MAP_DECODE_KEY_DECODER("pw",   zcbor_tstr_decode, &pw),
	};
	char ssid_z[33], pw_z[65];

	/* Both fields are length-checked up front and rejected -- never quietly
	 * clamped: a truncated passphrase fails to associate with no useful
	 * error. WPA2 allows 63 characters (or a 64-hex-digit PSK), so
	 * sizeof(pw_z) - 1 is the real protocol limit, not a buffer artefact. */
	if (zcbor_map_decode_bulk(zsd, decoders, ARRAY_SIZE(decoders), &decoded) != 0 ||
	    ssid.len == 0 || ssid.len > sizeof(ssid_z) - 1 ||
	    pw.len > sizeof(pw_z) - 1) {
		return MGMT_ERR_EINVAL;
	}

	memcpy(ssid_z, ssid.value, ssid.len);
	ssid_z[ssid.len] = '\0';
	memcpy(pw_z, pw.value, pw.len);
	pw_z[pw.len] = '\0';

	int rc = hpi_connectivity_wifi_connect(ssid_z, (pw.len ? pw_z : NULL));
	bool ok = zcbor_tstr_put_lit(zse, "ok") && zcbor_bool_put(zse, rc == 0);
	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

int hpi_wifi_forget_write(struct smp_streamer *ctxt)
{
	zcbor_state_t *zse = ctxt->writer->zs;

	int gate = hpi_security_require_unlocked();
	if (gate != MGMT_ERR_EOK) {
		return gate;
	}
	int rc = hpi_connectivity_wifi_disconnect();
	bool ok = zcbor_tstr_put_lit(zse, "ok") && zcbor_bool_put(zse, rc == 0);
	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

/*
 * 0x0075 conn_enable -- power the co-processor and bring up the radios in
 * `radios` (bit0 WiFi, bit1 BLE). A mask of 0 is legal and means "powered, no
 * radio" -- needed for flashing, since a C6 held in reset cannot be flashed
 * over its own USB port. Not unlock-gated (like wifi_softap): a locked device
 * must still be able to get online, and this changes no stored secret.
 */
int hpi_conn_enable_write(struct smp_streamer *ctxt)
{
	zcbor_state_t *zsd = ctxt->reader->zs;
	zcbor_state_t *zse = ctxt->writer->zs;

	uint32_t radios = HPI_CONN_RADIO_WIFI;   /* default: WiFi, the common case */
	size_t decoded = 0;
	struct zcbor_map_decode_key_val decoders[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("radios", zcbor_uint32_decode, &radios),
	};

	if (zcbor_map_decode_bulk(zsd, decoders, ARRAY_SIZE(decoders), &decoded) != 0) {
		return MGMT_ERR_EINVAL;
	}
	if (radios & ~(uint32_t)(HPI_CONN_RADIO_WIFI | HPI_CONN_RADIO_BLE)) {
		return MGMT_ERR_EINVAL;
	}

	int rc = hpi_connectivity_enable((uint8_t)radios);

	if (rc != 0) {
		LOG_WRN("conn_enable failed: %d", rc);
		bool e = smp_add_cmd_err(zse, HPI_MGMT_GROUP_ID, HPI_MGMT_ERR_HW_FAULT);
		return e ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
	}
	/* "Accepted", not "radio up": the service performs the transition on its
	 * own thread; the host watches conn_status for the outcome. */
	return zcbor_tstr_put_lit(zse, "ok") && zcbor_bool_put(zse, true)
		       ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

/* 0x0076 conn_disable -- radios down, co-processor back into reset. */
int hpi_conn_disable_write(struct smp_streamer *ctxt)
{
	zcbor_state_t *zse = ctxt->writer->zs;

	hpi_connectivity_disable();
	return zcbor_tstr_put_lit(zse, "ok") && zcbor_bool_put(zse, true)
		       ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

/*
 * 0x0077 conn_status -- the whole link picture in one read, answered from the
 * service's cache so it never costs a round-trip to the co-processor.
 *
 * `link` is the field wifi_status could never express: it separates "the radio
 * is off because that is what was asked for" from "the co-processor should be
 * answering and is not".
 */
int hpi_conn_status_read(struct smp_streamer *ctxt)
{
	zcbor_state_t *zse = ctxt->writer->zs;
	struct hpi_conn_status st;
	char ip[16];

	hpi_connectivity_get_status(&st);
	(void)snprintf(ip, sizeof(ip), "%u.%u.%u.%u",
		       st.ip[0], st.ip[1], st.ip[2], st.ip[3]);

	bool ok =
		zcbor_tstr_put_lit(zse, "link")   && zcbor_uint32_put(zse, st.link_state) &&
		zcbor_tstr_put_lit(zse, "radios") && zcbor_uint32_put(zse, st.radios) &&
		zcbor_tstr_put_lit(zse, "state")  && zcbor_uint32_put(zse, st.wifi_state) &&
		zcbor_tstr_put_lit(zse, "rssi")   && zcbor_int32_put(zse, st.rssi) &&
		zcbor_tstr_put_lit(zse, "ssid")   && zcbor_tstr_put_term(zse, st.ssid, sizeof(st.ssid)) &&
		zcbor_tstr_put_lit(zse, "ip")     && zcbor_tstr_put_term(zse, ip, sizeof(ip)) &&
		zcbor_tstr_put_lit(zse, "ble_adv")  && zcbor_bool_put(zse, st.ble_adv) &&
		zcbor_tstr_put_lit(zse, "ble_conn") && zcbor_bool_put(zse, st.ble_conn);
	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

int hpi_wifi_softap_write(struct smp_streamer *ctxt)
{
	zcbor_state_t *zse = ctxt->writer->zs;

	/*
	 * Open the co-processor's SoftAP captive portal -- the only supported
	 * way to provision credentials (never pushed over the host link).
	 * Deliberately NOT unlock-gated: it changes no stored secret, and the
	 * portal is how a locked device gets onto a network in the first place.
	 */
	int rc = hpi_connectivity_wifi_softap();

	if (rc != 0) {
		LOG_WRN("wifi_softap failed: %d", rc);
		bool e = smp_add_cmd_err(zse, HPI_MGMT_GROUP_ID, HPI_MGMT_ERR_HW_FAULT);
		return e ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
	}
	return zcbor_tstr_put_lit(zse, "ok") && zcbor_bool_put(zse, true)
		       ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}
