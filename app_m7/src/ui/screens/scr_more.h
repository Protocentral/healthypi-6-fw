/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * "More" submenu + a generic placeholder screen. The 5th nav slot ("More")
 * opens a submenu that routes to Link / HRV / Settings / OTA. Pure L5, no
 * service/bus use.
 */
#ifndef HPI_UI_SCR_MORE_H
#define HPI_UI_SCR_MORE_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The "More" tab: a list of submenu entries (Link / HRV / Settings / OTA), each
 * routing to its screen via hpi_ui_show_screen(). */
lv_obj_t *hpi_scr_more_create(lv_obj_t *parent);

/* A labelled placeholder for a not-yet-built screen. `back_to` >= 0 adds a small
 * back chip that routes there (use HPI_UI_SCREEN_MORE for submenu screens); pass
 * -1 for a main tab that the nav bar already returns from. */
lv_obj_t *hpi_scr_placeholder_create(lv_obj_t *parent, const char *title,
				     const char *subtitle, int back_to);

#ifdef __cplusplus
}
#endif

#endif /* HPI_UI_SCR_MORE_H */
