/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 */
#ifndef HPI_SCREENS_SCR_RECORDING_H_
#define HPI_SCREENS_SCR_RECORDING_H_

#include <lvgl.h>

/* Browse/delete screen for recordings on the SD card. */
lv_obj_t *hpi_scr_recording_create(lv_obj_t *parent);

/* Kicks off a fresh recording_list_async() listing. Call once on create and
 * again any time the list may be stale (e.g. right before switching in from
 * the Rec screen's BROWSE button). */
void hpi_scr_recording_reload(void);

/* Polled from the UI thread (~2 Hz, like the other service-backed screens).
 * Cheap no-op until a pending reload has finished; rebuilds the row list at
 * most once per completed reload. */
void hpi_scr_recording_refresh(void);

#endif /* HPI_SCREENS_SCR_RECORDING_H_ */