/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Record screen — a recording_service client with a setup and an active view.
 * Exposes a refresh hook the UI thread calls ~2 Hz when active (UI-thread
 * context, so the LVGL calls are safe).
 */
#ifndef HPI_UI_SCR_SECONDARY_H
#define HPI_UI_SCR_SECONDARY_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *hpi_scr_record_create(lv_obj_t *parent);

/* Poll the recording service and update the labels. Call from the UI thread only
 * (typically when the screen is active). No-op if the screen wasn't built. */
void hpi_scr_record_refresh(void);

#ifdef __cplusplus
}
#endif

#endif /* HPI_UI_SCR_SECONDARY_H */
