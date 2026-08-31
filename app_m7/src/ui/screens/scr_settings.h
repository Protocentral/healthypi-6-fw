/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Settings + Alert limits + OTA — the More-submenu cluster. Brightness and
 * display sleep are live (config service + backlight); units/alert thresholds
 * are local UI state (visual — no real alarm system, per the research-use
 * caution); OTA shows the real firmware version. Pure L5.
 */
#ifndef HPI_UI_SCR_SETTINGS_H
#define HPI_UI_SCR_SETTINGS_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *hpi_scr_settings_create(lv_obj_t *parent);
lv_obj_t *hpi_scr_alert_create(lv_obj_t *parent);
lv_obj_t *hpi_scr_ota_create(lv_obj_t *parent);

#ifdef __cplusplus
}
#endif

#endif /* HPI_UI_SCR_SETTINGS_H */
