/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Config service (L4) -- persistent settings on the QSPI-flash LittleFS
 * (/lfs/settings via the Zephyr settings subsystem). Device code reads typed
 * values with defaults; the host reads/writes keys over the stock MCUmgr
 * settings group. Keys live under the "hpi" subtree.
 */

#ifndef HPI_SERVICES_CONFIG_SERVICE_H
#define HPI_SERVICES_CONFIG_SERVICE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Init the settings subsystem and load persisted values. Call once at boot
 * after the LittleFS is mounted (fstab automount handles that). */
int hpi_config_service_init(void);

/* Typed accessors (return the cached value; setters also persist). */
uint32_t hpi_config_stream_channels(void);            /* default 0x03 (ECG+PPG) */
int      hpi_config_set_stream_channels(uint32_t ch);

/* Brightness is driven continuously (a slider value per touch sample), so it
 * does NOT persist inline: the setter updates the cache and returns, and the
 * write is coalesced onto the service's own workqueue -- a QSPI erase/program
 * (plus periodic settings-file compaction) must never run on the LVGL UI
 * thread. Cost: a value set then power-cut within the debounce window is not
 * persisted. */
uint8_t  hpi_config_display_brightness(void);         /* default 80 (0..100) */
int      hpi_config_set_display_brightness(uint8_t b);

/* Idle seconds before the display drops to the ambient clock. 0 = never sleep.
 * Read by the UI's idle loop, set by Settings -> Display sleep. Persisted like
 * brightness, and for the same reason: it is a UI-rate control, so the write is
 * deferred onto the config workqueue rather than run on the LVGL thread. */
uint16_t hpi_config_display_sleep_s(void);            /* default 60 */
int      hpi_config_set_display_sleep_s(uint16_t s);

const char *hpi_config_device_name(void);             /* default "HealthyPi 6" */
int      hpi_config_set_device_name(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* HPI_SERVICES_CONFIG_SERVICE_H */
