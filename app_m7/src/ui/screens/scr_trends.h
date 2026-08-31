/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Trends + HRV screens. Trends plots a rolling history of the vitals the UI
 * already receives (HR/SpO2/Resp/Temp) with per-vital tabs + min/avg/max; the
 * HRV tab routes to the HRV screen (SDNN/RMSSD). There is no on-device history
 * producer, so the window is what the UI has accumulated this session. Pure L5.
 */
#ifndef HPI_UI_SCR_TRENDS_H
#define HPI_UI_SCR_TRENDS_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

struct hp6_vitals;   /* core/sample_formats.h */

lv_obj_t *hpi_scr_trends_create(lv_obj_t *parent);
lv_obj_t *hpi_scr_hrv_create(lv_obj_t *parent);

/* Push a vitals frame into the rolling history (call on every VITALS frame,
 * regardless of the active screen, so switching to Trends shows accumulated
 * history). Also updates the HRV screen numerics. */
void hpi_scr_trends_push_vitals(const struct hp6_vitals *v);

/* Redraw the chart/stats (Trends) from the history; call ~2 Hz when active. */
void hpi_scr_trends_refresh(void);

/* Which series Trends plots. Mirrors the tab order on the screen. */
enum hpi_trend_vital {
	HPI_TREND_HR = 0,
	HPI_TREND_SPO2,
	HPI_TREND_RESP,
	HPI_TREND_TEMP,
};

/* Select a series without a tab tap, so another screen can deep-link into
 * Trends (a Home vital chip routes here for its own vital). Out-of-range
 * values are ignored. Does NOT change the active screen; the caller does that
 * with hpi_ui_show_screen(). */
void hpi_scr_trends_select(enum hpi_trend_vital v);

#ifdef __cplusplus
}
#endif

#endif /* HPI_UI_SCR_TRENDS_H */
