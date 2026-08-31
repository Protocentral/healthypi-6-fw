/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Subsystem health & heartbeat monitor -- the software layer above the IWDG
 * hardware backstop (platform/watchdog.h).
 *
 * Two ways a subsystem reports health: heartbeat (always-on producers call
 * hpi_health_alive() regularly; a monitor thread degrades them on a missed
 * deadline and restores OK when they resume) and explicit (hpi_health_set(),
 * e.g. M4 bind timeout -> FAILED, module quarantine -> DEGRADED).
 *
 * Philosophy: the device DEGRADES, never bricks. The monitor only logs and
 * publishes health events; it never stops feeding the IWDG, which resets only
 * if the whole system (incl. main's feed loop) is wedged.
 */

#ifndef HPI_PLATFORM_HEALTH_H
#define HPI_PLATFORM_HEALTH_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum hpi_subsys {
	HPI_SUBSYS_ACQ = 0,     /* onboard ECG/PPG acquisition (heartbeat) */
	HPI_SUBSYS_M4_IPC,      /* M4 vitals link (heartbeat once bound)   */
	HPI_SUBSYS_STREAM,      /* USB stream service (explicit)           */
	HPI_SUBSYS_RECORDING,   /* SD recording (explicit)                 */
	HPI_SUBSYS_HEALTHYLINK, /* module framework (explicit)             */
	HPI_SUBSYS_COUNT
};

enum hpi_health_state {
	HPI_HEALTH_UNKNOWN = 0, /* not registered / never reported */
	HPI_HEALTH_OK,          /* healthy                          */
	HPI_HEALTH_DEGRADED,    /* impaired but running             */
	HPI_HEALTH_FAILED,      /* not functioning                  */
};

struct hpi_health_entry {
	const char *name;
	uint8_t state;          /* enum hpi_health_state */
	uint8_t registered;
	uint32_t hb_timeout_ms; /* 0 = no heartbeat watchdog (explicit only) */
	int64_t last_seen_ms;   /* last heartbeat (k_uptime) */
	const char *reason;     /* last DEGRADED/FAILED reason (static string) */
};

struct hpi_health_report {
	struct hpi_health_entry e[HPI_SUBSYS_COUNT];
	uint8_t overall;        /* worst non-OK state across registered subsystems */
};

/* Register a subsystem. hb_timeout_ms > 0 arms the heartbeat watchdog (the
 * subsystem must call hpi_health_alive() at least that often once it is up). */
void hpi_health_register(enum hpi_subsys id, const char *name, uint32_t hb_timeout_ms);

/* Heartbeat check-in (cheap; safe from any thread). Records "now" and, if the
 * subsystem had degraded on a missed heartbeat, restores it to OK. */
void hpi_health_alive(enum hpi_subsys id);

/* Explicitly set a subsystem's state (reason must be a static string or NULL). */
void hpi_health_set(enum hpi_subsys id, enum hpi_health_state state, const char *reason);

enum hpi_health_state hpi_health_get(enum hpi_subsys id);

/* Copy a snapshot of all entries (for telemetry/diag/MCUmgr). */
void hpi_health_snapshot(struct hpi_health_report *out);

/* Worst state across all registered subsystems. */
enum hpi_health_state hpi_health_overall(void);

const char *hpi_health_state_str(enum hpi_health_state s);

/* Start the monitor thread (call once, after subsystems are registered). */
void hpi_health_start(void);

#ifdef __cplusplus
}
#endif

#endif /* HPI_PLATFORM_HEALTH_H */
