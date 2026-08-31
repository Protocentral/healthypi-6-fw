/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Live screen: three stacked waveform lanes — ECG (lead II) · PPG (IR) · Resp
 * (bioZ) — each with an inline numeric (HR / SpO2 / RR), plus a control row.
 * Pure L5 bus consumer; every hook is called from the UI thread only. Resp
 * rides in hp6_ecg_sample.resp on HPI_CH_ECG, pushed alongside the ECG lead.
 */
#ifndef HPI_UI_SCR_LIVE_H
#define HPI_UI_SCR_LIVE_H

#include <lvgl.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct hp6_vitals;   /* core/sample_formats.h */

lv_obj_t *hpi_scr_live_create(lv_obj_t *parent);

/* One ECG sample feeds both the ECG lane (lead II) and the Resp lane (resp);
 * lead_off drives the leads-OK indicator. */
void hpi_scr_live_push_ecg(int32_t lead_ii_uv, int32_t resp_uv, uint8_t lead_off);
void hpi_scr_live_push_ppg(int32_t ir);

/* Inline lane numerics (HR / SpO2 / RR); "--" until produced (truthful-data). */
void hpi_scr_live_set_vitals(const struct hp6_vitals *v);

#ifdef __cplusplus
}
#endif

#endif /* HPI_UI_SCR_LIVE_H */
