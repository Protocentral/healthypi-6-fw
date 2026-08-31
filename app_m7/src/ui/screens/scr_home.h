/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Home screen: a focal heart-rate readout + a small live ECG sparkline + a
 * 2x2 vital-chip grid (SpO2 / Resp / Temp / HRV), under the shared status bar.
 * Pure L5: a sample-bus consumer + service client; every hook is called from
 * the UI thread only.
 */
#ifndef HPI_UI_SCR_HOME_H
#define HPI_UI_SCR_HOME_H

#include <lvgl.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct hp6_vitals;   /* core/sample_formats.h */

/* Build the home content into `parent` (the content area). */
lv_obj_t *hpi_scr_home_create(lv_obj_t *parent);

/* One decimated ECG sample: the sparkline point, plus the electrode lead-off
 * mask (HP6_LEAD_OFF_*) that rides on the same sample and drives the lead-off
 * banner. Taken from the ECG channel rather than from vitals so the warning
 * works with no M4 running. */
void hpi_scr_home_push_ecg(int32_t lead_ii_uv, uint8_t lead_off);
void hpi_scr_home_push_ppg(int32_t ir);

/* Update the focal HR + the 2x2 vital chips from a bus vitals frame. Fields that
 * are not yet produced (RR/temp/HRV = 0) render an em-dash (truthful-data rule). */
void hpi_scr_home_set_vitals(const struct hp6_vitals *v);

#ifdef __cplusplus
}
#endif

#endif /* HPI_UI_SCR_HOME_H */
