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

#include <stddef.h>
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

/*
 * The co-processor's firmware version string (e.g. "1.1.0"), from the cache
 * conn_thread fills over HealthyBridge GET_VERSION once the link comes up.
 * Never blocks and never touches the UART -- safe from SMP handlers and the
 * LVGL thread.
 *
 * @p out is always NUL-terminated, and is "" unless the return is 0:
 *   0           known
 *   -EHOSTDOWN  co-processor held in reset (the boot default)
 *   -EAGAIN     powered but has not answered yet (starting, or restarted)
 *   -ENOTSUP    its firmware predates GET_VERSION, or connectivity is
 *               compiled out
 *   -ENODEV     service not running
 * The cache is dropped on power-down, link fault and any C6 restart.
 */
int hpi_connectivity_esp_version(char *out, size_t cap);

/* True when the co-processor is powered and answering -- not merely that the
 * service thread started. Drives the status-bar link glyph. */
bool hpi_connectivity_ready(void);

/*
 * Link ownership for a co-processor firmware update (services/
 * esp32_update_service.c is the only caller).
 *
 * hold(): power the C6 if it is held in reset, and for as long as the hold
 * lasts
 *   - stop forwarding waveforms and vitals to it, and stop the status poll,
 *     version fetch and keepalive once the link is up -- the updater's own
 *     frames are the traffic, and a short-deadline frame cut off mid-way by
 *     the C6 holding RTS through a flash erase would cost it a reply;
 *   - never power it down: enable/disable requests from anywhere (the UI's
 *     Wi-Fi toggle, the shell) are recorded and applied after release, so a
 *     C6 whose new image is pending verification is never reset (a reset
 *     then rolls it back).
 * Non-blocking; hpi_connectivity_ready() says when the link is up.
 *
 * release(keep_powered): end the hold and apply whatever is requested --
 * including putting the C6 back into reset if that is what was asked for
 * before or during the update. keep_powered = true overrides that, leaving the
 * C6 powered with no radio: for an outcome the updater could not confirm, where
 * the image may still be pending and a reset would roll it back.
 *
 * Returns -ENODEV when the service is not running.
 */
int hpi_connectivity_update_hold(void);
void hpi_connectivity_update_release(bool keep_powered);
bool hpi_connectivity_update_held(void);

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
