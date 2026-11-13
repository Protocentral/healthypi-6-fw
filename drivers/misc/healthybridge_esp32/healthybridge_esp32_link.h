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
	/**
	 * Times the co-processor was seen to restart: every release from reset
	 * by link_power(true), plus every in-place reboot (panic, watchdog,
	 * esp_restart after an OTA or provisioning) detected by its frame
	 * sequence number running backwards. Monotonic; a caller holding state
	 * learned from the peer (e.g. its firmware version) compares this
	 * against the value it saw when it learned it.
	 */
	uint32_t peer_restarts;
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

	/**
	 * Ask the co-processor for its firmware version (HB 0x31 GET_VERSION).
	 * A round-trip: blocks up to the transport's command timeout. -EIO when
	 * the peer acks with an error (an image older than HealthyBridge 1.1.0,
	 * which has no such command); -ETIMEDOUT when it does not answer.
	 */
	int (*get_version)(const struct device *dev,
			   struct hpi_hb_version_resp *out);

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

	/**
	 * Make sure the co-processor hears from this host: if nothing has been
	 * transmitted for @p idle_ms, send a real GET_STATUS on the wire --
	 * bypassing the status cache that wifi_status() answers from -- and
	 * refresh that cache from the reply. Otherwise do nothing.
	 *
	 * Why: a freshly updated C6 image keeps its OTA slot only after it has
	 * received a few CRC-valid frames from the host within 60 s of booting.
	 * With the radios off and the M4 silent, everything this end used to
	 * send was answered from cache, i.e. not sent at all.
	 *
	 * Blocking for at most one command round-trip when a frame is due.
	 * Returns 0, or the command's error (-ETIMEDOUT still means the frame
	 * went out; only the reply is missing).
	 */
	int (*keepalive)(const struct device *dev, uint32_t idle_ms);

	/**
	 * Send one firmware-update request (HealthyBridge 0x80) and wait for the
	 * reply that answers it: a 0x81 with the same op and, for DATA, the same
	 * req_offset. Stale and duplicate replies -- answers to an earlier
	 * request that arrive late -- are discarded, never handed back.
	 *
	 * @p begin is the BEGIN params (NULL for every other op); @p offset and
	 * @p data / @p len are the DATA params (ignored for every other op).
	 * @p timeout_ms runs from the moment the frame has left the UART.
	 *
	 * The frame is transmitted with a deadline of at least
	 * HB_UART_OTA_TX_DEADLINE_US: the C6 de-asserts RTS while it erases a
	 * flash sector (~0.4 s), and the ordinary short deadline would cut the
	 * frame mid-way.
	 *
	 * Returns 0 when a matching reply arrived -- whatever its status, which
	 * the caller interprets -- with @p out filled; -ETIMEDOUT with none;
	 * -EHOSTDOWN when the co-processor is held in reset; -EINVAL for a
	 * request the contract does not allow (e.g. a DATA chunk over
	 * HPI_HB_OTA_MAX_CHUNK); or a transmit error. One request at a time per
	 * link: a second caller blocks until the first has its answer.
	 *
	 * Blocking: never call it from the system workqueue (the receive path
	 * runs there, so the reply could never be delivered) or the LVGL thread.
	 */
	int (*ota_request)(const struct device *dev, uint8_t op,
			   const struct hpi_hb_ota_begin *begin, uint32_t offset,
			   const uint8_t *data, uint16_t len, uint32_t timeout_ms,
			   struct hpi_hb_ota_reply *out);

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

static inline int healthybridge_get_version(const struct device *dev,
					    struct hpi_hb_version_resp *out)
{
	return HB_LINK_CALL(dev, get_version, out);
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

static inline int healthybridge_ota_request(const struct device *dev, uint8_t op,
					    const struct hpi_hb_ota_begin *begin,
					    uint32_t offset, const uint8_t *data,
					    uint16_t len, uint32_t timeout_ms,
					    struct hpi_hb_ota_reply *out)
{
	return HB_LINK_CALL(dev, ota_request, op, begin, offset, data, len,
			    timeout_ms, out);
}

static inline int healthybridge_keepalive(const struct device *dev, uint32_t idle_ms)
{
	return HB_LINK_CALL(dev, keepalive, idle_ms);
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
