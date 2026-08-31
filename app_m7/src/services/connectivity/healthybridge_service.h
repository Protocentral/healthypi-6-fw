/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Connectivity service (L4) -- streams the sample bus to the ESP32-C6 network
 * co-processor over the HealthyBridge UART link, and proxies radio control.
 *
 * An L4 bus consumer (mirror of stream_service, which feeds USB CDC0): a
 * dedicated thread subscribes to ECG/PPG/VITALS, maps the canonical hp6_*
 * payloads onto HealthyBridge frames, and hands them to the driver
 * (drivers/misc/healthybridge_esp32). The C6 re-streams to WiFi TCP / BLE GATT
 * clients and serves the MCUmgr wireless gateway; group-64 conn_/wifi_
 * commands call into this service.
 *
 * ---- Radios are OFF at boot ----
 *
 * The co-processor is held in reset until something asks for it, and its own
 * firmware starts neither radio even once powered. Nothing here enables a
 * radio on its own, and nothing persists an "on" state: every boot starts
 * quiet. Reason: radios concurrent with boot brown out USB-powered units (the
 * BQ24074's input ceiling is strapped in hardware; drawing less is the only
 * lever). Enabling is always an explicit act, from exactly three places: the
 * Connectivity screen (ui/screens/scr_link.c), group 64, and the dev shell.
 */

#ifndef HPI_CONNECTIVITY_HEALTHYBRIDGE_SERVICE_H
#define HPI_CONNECTIVITY_HEALTHYBRIDGE_SERVICE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * WiFi association state, as reported by the co-processor. Numerically
 * identical to the driver's HPI_WIFI_STATE_* (and the ESP's HB_WIFI_STATE_*)
 * but deliberately spelled differently, so UI/control code can render a state
 * without including driver headers (same names would collide macro-vs-enum).
 */
enum hpi_conn_wifi_state {
	HPI_CONN_WIFI_DISCONNECTED = 0x00,
	HPI_CONN_WIFI_CONNECTING   = 0x01,
	HPI_CONN_WIFI_CONNECTED    = 0x02,
	HPI_CONN_WIFI_AP_MODE      = 0x03,   /* provisioning portal is up */
	HPI_CONN_WIFI_ERROR        = 0xFF,
};

/* WiFi state mirrored to the host (decoupled from the driver's struct so the
 * control layer needn't include driver headers). */
struct hpi_wifi_info {
	uint8_t state;       /* enum hpi_conn_wifi_state */
	int8_t  rssi;        /* dBm when connected */
	uint8_t ip[4];       /* IPv4 when connected */
	char    ssid[33];    /* connected SSID (null-terminated) */
};

/* Power state of the co-processor itself, distinct from any radio's state. */
enum hpi_conn_link_state {
	HPI_CONN_LINK_OFF = 0,   /* held in reset; draws microamps */
	HPI_CONN_LINK_STARTING,  /* released, waiting for it to answer */
	HPI_CONN_LINK_UP,        /* answering */
	HPI_CONN_LINK_FAULT,     /* powered but silent past its budget */
};

/* Radio selection bits for hpi_connectivity_enable(). */
#define HPI_CONN_RADIO_WIFI  0x01u
#define HPI_CONN_RADIO_BLE   0x02u

struct hpi_conn_status {
	uint8_t  link_state;   /* enum hpi_conn_link_state */
	uint8_t  radios;       /* HPI_CONN_RADIO_* currently requested */
	uint8_t  wifi_state;   /* HPI_WIFI_STATE_* last reported by the C6 */
	int8_t   rssi;
	uint8_t  ip[4];
	char     ssid[33];
	bool     ble_adv;      /* co-processor is advertising */
	bool     ble_conn;     /* a central is connected */
	uint32_t frames_sent;
	uint32_t frames_dropped;
};

/* Start the service: subscribe to the bus and start the consumer thread. Does
 * NOT power the co-processor. No-op when CONFIG_HPI_CONNECTIVITY=n or the
 * HealthyBridge DT node is absent. */
void hpi_connectivity_init(void);

/*
 * Power the co-processor (if it is not already) and bring up the radios in
 * radio_mask. A mask of 0 is legal and meaningful: power the C6 with no radio
 * -- needed for flashing, since a C6 held in reset cannot be flashed over its
 * own USB port.
 *
 * Non-blocking: records the intent and returns; the consumer thread performs
 * the transition. Safe to call from the LVGL thread and from SMP handlers.
 */
int hpi_connectivity_enable(uint8_t radio_mask);

/* Radios down and the co-processor back into reset. Idempotent, non-blocking. */
void hpi_connectivity_disable(void);

/* Cached; never blocks and never issues a round-trip. */
void hpi_connectivity_get_status(struct hpi_conn_status *out);

/* True when the co-processor is powered and answering -- not merely that the
 * service thread started. Drives the status-bar link glyph. */
bool hpi_connectivity_ready(void);

/*
 * Compatibility wrappers over the two calls above, kept because the shell and
 * the group-64 wifi_* handlers are written against them. Return 0 on success or
 * a negative errno; -ENOTSUP when connectivity is compiled out.
 */
int hpi_connectivity_wifi_status(struct hpi_wifi_info *out);
int hpi_connectivity_wifi_connect(const char *ssid, const char *password);
int hpi_connectivity_wifi_disconnect(void);
int hpi_connectivity_wifi_enable(bool on);
int hpi_connectivity_ble_enable(bool on);
/* Open the ESP32 SoftAP captive portal for provisioning. Powers the C6 and
 * lifts the WiFi gate first -- it is the only way credentials ever get in. */
int hpi_connectivity_wifi_softap(void);

#ifdef __cplusplus
}
#endif

#endif /* HPI_CONNECTIVITY_HEALTHYBRIDGE_SERVICE_H */
