/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Boot sequence: a full-screen overlay shown over the UI at start-up. Splash
 * auto-advances to the self-test, which reflects the per-subsystem health
 * snapshot, then the overlay tears down and the main UI appears. Pure L5:
 * reads hpi_health_snapshot() only.
 */
#ifndef HPI_UI_SCR_BOOT_H
#define HPI_UI_SCR_BOOT_H

#include <lvgl.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Create the boot overlay (splash view) as a top child of `screen`. */
lv_obj_t *hpi_scr_boot_create(lv_obj_t *screen);

/* Swap the splash for the self-test view. */
void hpi_scr_boot_selftest(void);

/* Refresh the self-test subsystem rows from the health snapshot. */
void hpi_scr_boot_refresh(void);

/* True once every subsystem is up (OK or degraded — none unknown/failed). Lets
 * the boot hold on the self-test until subsystems report (e.g. the M4 binds
 * ~7 s in) instead of advancing on a fixed timer. */
bool hpi_scr_boot_all_pass(void);

/* Delete the overlay. */
void hpi_scr_boot_destroy(void);

#ifdef __cplusplus
}
#endif

#endif /* HPI_UI_SCR_BOOT_H */
