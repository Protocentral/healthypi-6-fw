/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Ambient clock — a minimum-backlight sleep state shown after the
 * display is idle. A full-screen overlay: big clock + date + HR + recording
 * status. ui_module manages the idle timeout / wake (any touch) and the backlight
 * dim/restore. Pure L5 (reads RTC + recording status).
 */
#ifndef HPI_UI_SCR_AMBIENT_H
#define HPI_UI_SCR_AMBIENT_H

#include <lvgl.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void hpi_scr_ambient_show(lv_obj_t *screen);   /* create overlay (top of `screen`) */
void hpi_scr_ambient_hide(void);               /* delete overlay */
bool hpi_scr_ambient_active(void);
void hpi_scr_ambient_refresh(void);            /* update clock/date/rec */
/* Latest HR for the ambient readout, with the HP6_VIT_* provenance flags from
 * the same vitals frame (the face marks a PPG-derived rate as such). */
void hpi_scr_ambient_set_hr(uint16_t hr_bpm, uint8_t flags);

#ifdef __cplusplus
}
#endif

#endif /* HPI_UI_SCR_AMBIENT_H */
