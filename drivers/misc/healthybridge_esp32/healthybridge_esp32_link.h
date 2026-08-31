/*
 * Copyright (c) 2026 Protocentral
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file healthybridge_esp32_link.h
 * @brief Transport-neutral HealthyBridge host-link API.
 *
 * Callers use HEALTHYBRIDGE_LINK_NODE + the healthybridge_send_*() wrappers; the
 * driver publishes a `struct healthybridge_link_api` as its device API. The
 * vtable lets the transport change underneath the connectivity service without
 * the service noticing.
 */

#ifndef HEALTHYBRIDGE_ESP32_LINK_H
#define HEALTHYBRIDGE_ESP32_LINK_H

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <errno.h>
#include <stdbool.h>

#include "healthybridge_esp32_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The host-link node. Defined only when the link is present in devicetree, so
 * consumers can compile it out with #if defined(HEALTHYBRIDGE_LINK_NODE). */
#define HEALTHYBRIDGE_LINK_UART_NODE DT_NODELABEL(healthybridge_esp32_uart)

#if DT_NODE_HAS_STATUS(HEALTHYBRIDGE_LINK_UART_NODE, okay)
#define HEALTHYBRIDGE_LINK_NODE HEALTHYBRIDGE_LINK_UART_NODE
#endif

/** Receive-side link counters (see get_rx_stats). */
struct healthybridge_rx_stats {
	uint32_t bytes;    /**< bytes received from the co-processor */
	uint32_t frames;   /**< frames decoded with a valid CRC */
	uint32_t crc_err;  /**< frames whose CRC failed */
	uint32_t resync;   /**< decoder sync-hunt restarts */
	uint32_t ovf;      /**< bytes dropped because the staging ring was full */
	uint32_t stopped;  /**< receiver error events (framing/parity/overrun) */
};

/**
 * @brief Driver-published operations for a HealthyBridge host link.
 *
 * Every entry may be NULL; the wrappers below return -ENOSYS for a capability
 * the transport has not implemented.
 */
struct healthybridge_link_api {
	int (*send_ecg_multi)(const struct device *dev, uint32_t timestamp_ms,
			      const struct hpi_ecg_sample_multi *samples, uint16_t count);
	int (*send_ppg)(const struct device *dev, uint32_t timestamp_ms,
			const struct hpi_ppg_sample *samples, uint16_t count);
	int (*send_vitals)(const struct device *dev,
			   const struct hpi_hb_vitals_payload *vitals);
	int (*send_hrv)(const struct device *dev,
			const struct hpi_hb_hrv_payload *hrv);

	/**
	 * Power the co-processor up or down by driving its EN/reset line.
	 *
	 * on=false asserts reset and leaves it asserted: the C6 is in shutdown,
	 * not merely idle, and draws microamps. on=true releases it and waits
	 * CONFIG_HEALTHYBRIDGE_ESP32_BOOT_DELAY_MS for the part to come up, so
	 * the caller may issue a command as soon as it returns.
	 *
	 * Blocking, and the boot wait is the better part of a second -- never
	 * call it from the LVGL thread or any producer callback.
	 */
	int (*link_power)(const struct device *dev, bool on);

	int (*wifi_status)(const struct device *dev,
			   struct hpi_hb_wifi_status_resp *out);
	int (*wifi_enable)(const struct device *dev, bool on);

	/** Start/stop BLE advertising on the co-processor (HB 0x10 / 0x11). */
	int (*ble_adv)(const struct device *dev, bool on);
	/** Set the advertised BLE device name (HB 0x12). */
	int (*ble_set_name)(const struct device *dev, const char *name);
	int (*wifi_connect)(const struct device *dev, const char *ssid,
			    const char *password);
	int (*wifi_disconnect)(const struct device *dev);
	/** Open the SoftAP captive portal (the documented provisioning path --
	 *  credentials are never pushed over this link). */
	int (*wifi_softap)(const struct device *dev);

	/** Receive-side counters, for diagnostics. -ENOSYS if the transport has
	 *  no receive path worth reporting. */
	int (*get_rx_stats)(const struct device *dev, struct healthybridge_rx_stats *out);

	/** Human-readable transport name for logs ("spi" / "uart"). */
	const char *name;
};

static inline const struct healthybridge_link_api *healthybridge_link_api(
	const struct device *dev)
{
	return (const struct healthybridge_link_api *)dev->api;
}

#define HB_LINK_CALL(dev, op, ...)                                             \
	({                                                                     \
		const struct healthybridge_link_api *_api =                    \
			healthybridge_link_api(dev);                           \
		(_api == NULL || _api->op == NULL) ? -ENOSYS                   \
						   : _api->op(dev, ##__VA_ARGS__); \
	})

static inline int healthybridge_send_ecg_multi(const struct device *dev,
					       uint32_t timestamp_ms,
					       const struct hpi_ecg_sample_multi *samples,
					       uint16_t count)
{
	return HB_LINK_CALL(dev, send_ecg_multi, timestamp_ms, samples, count);
}

static inline int healthybridge_send_ppg(const struct device *dev,
					 uint32_t timestamp_ms,
					 const struct hpi_ppg_sample *samples,
					 uint16_t count)
{
	return HB_LINK_CALL(dev, send_ppg, timestamp_ms, samples, count);
}

static inline int healthybridge_send_vitals(const struct device *dev,
					    const struct hpi_hb_vitals_payload *vitals)
{
	return HB_LINK_CALL(dev, send_vitals, vitals);
}

static inline int healthybridge_send_hrv(const struct device *dev,
					 const struct hpi_hb_hrv_payload *hrv)
{
	return HB_LINK_CALL(dev, send_hrv, hrv);
}

static inline int healthybridge_link_power(const struct device *dev, bool on)
{
	return HB_LINK_CALL(dev, link_power, on);
}

static inline int healthybridge_wifi_status(const struct device *dev,
					    struct hpi_hb_wifi_status_resp *out)
{
	return HB_LINK_CALL(dev, wifi_status, out);
}

static inline int healthybridge_wifi_enable(const struct device *dev, bool on)
{
	return HB_LINK_CALL(dev, wifi_enable, on);
}

static inline int healthybridge_wifi_connect(const struct device *dev,
					     const char *ssid, const char *password)
{
	return HB_LINK_CALL(dev, wifi_connect, ssid, password);
}

static inline int healthybridge_wifi_disconnect(const struct device *dev)
{
	return HB_LINK_CALL(dev, wifi_disconnect);
}

static inline int healthybridge_wifi_softap(const struct device *dev)
{
	return HB_LINK_CALL(dev, wifi_softap);
}

static inline int healthybridge_ble_adv(const struct device *dev, bool on)
{
	return HB_LINK_CALL(dev, ble_adv, on);
}

static inline int healthybridge_ble_set_name(const struct device *dev, const char *name)
{
	return HB_LINK_CALL(dev, ble_set_name, name);
}

static inline int healthybridge_get_rx_stats(const struct device *dev,
					     struct healthybridge_rx_stats *out)
{
	return HB_LINK_CALL(dev, get_rx_stats, out);
}

static inline const char *healthybridge_link_name(const struct device *dev)
{
	const struct healthybridge_link_api *api = healthybridge_link_api(dev);

	return (api != NULL && api->name != NULL) ? api->name : "unknown";
}

#ifdef __cplusplus
}
#endif

#endif /* HEALTHYBRIDGE_ESP32_LINK_H */
