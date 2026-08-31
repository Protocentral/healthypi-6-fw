/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Control/event plane (cold path) -- a zbus channel for lifecycle, health,
 * and async device events. This is the slow-path sibling of the sample bus:
 * the sample bus carries high-rate signal frames; this carries occasional
 * events (boot, fs state, module hot-plug, battery, faults) that the UI and
 * the MCUmgr/event adapter observe. Producers never block on it.
 */

#ifndef HPI_BUS_EVENTS_H
#define HPI_BUS_EVENTS_H

#include <stdint.h>
#include <zephyr/zbus/zbus.h>

#ifdef __cplusplus
extern "C" {
#endif

enum hpi_evt_type {
    HPI_EVT_BOOT = 1,
    HPI_EVT_FS_READY,
    HPI_EVT_FS_NOT_READY,
    HPI_EVT_BUS_READY,
    HPI_EVT_HEALTH,    /* arg = (subsys_id << 8) | health_state */
    HPI_EVT_USER_MARK, /* user event marker (UI MARK EVENT / hardware short-press) */
    /* later: module insert/remove, battery, lead-off, over-temp, ... */
};

struct hpi_sys_evt {
    uint16_t type;     /* enum hpi_evt_type */
    uint32_t t_ms;     /* device-monotonic ms */
    int32_t  arg;      /* event-specific */
};

ZBUS_CHAN_DECLARE(hpi_sys_evt_chan);

/* Publish a system event (non-blocking, best-effort). */
void hpi_events_publish(uint16_t type, int32_t arg);

#ifdef __cplusplus
}
#endif

#endif /* HPI_BUS_EVENTS_H */
