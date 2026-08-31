/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * UI engine — public surface. The UI is an L5 bus consumer + service
 * client; nothing in core/services depends on it. Call hpi_ui_init() once at
 * boot (gated by CONFIG_HPI_DISPLAY_ENABLED; a no-op stub otherwise).
 */
#ifndef HPI_UI_MODULE_H
#define HPI_UI_MODULE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Screens. The first HPI_UI_MAIN_TAB_COUNT entries are the bottom-nav tabs (in
 * bar order); the rest are the "More" submenu, reached from the More tab, not the
 * nav bar. Keep the two groups contiguous — the nav bar and the swipe logic rely
 * on it (main tabs are < HPI_UI_MAIN_TAB_COUNT, submenu screens are >= it). */
enum hpi_ui_screen {
	/* 5 bottom-nav tabs */
	HPI_UI_SCREEN_HOME = 0,
	HPI_UI_SCREEN_LIVE,
	HPI_UI_SCREEN_REC,
	HPI_UI_SCREEN_TRENDS,
	HPI_UI_SCREEN_MORE,
	/* "More" submenu destinations */
	HPI_UI_SCREEN_LINK,
	HPI_UI_SCREEN_HRV,
	HPI_UI_SCREEN_SETTINGS,
	HPI_UI_SCREEN_ALERT,
	HPI_UI_SCREEN_OTA,
	HPI_UI_SCREEN_COUNT,
};

/* The bottom navigation bar shows the first N screens as tabs. */
#define HPI_UI_MAIN_TAB_COUNT  HPI_UI_SCREEN_LINK   /* HOME..MORE = 5 */
/* The last main tab that participates in left/right swipe (More is tap-only). */
#define HPI_UI_LAST_SWIPE_TAB  HPI_UI_SCREEN_TRENDS

/* Build the display UI and start the LVGL/bus thread. No-op when the display
 * is not compiled in. */
void hpi_ui_init(void);

/* Switch the active screen (called by the nav bar; safe from the UI thread). */
void hpi_ui_show_screen(enum hpi_ui_screen scr);

/* Set the display backlight (0..100 %). No-op if the display is off / no panel.
 * Used by Settings (brightness) and the ambient-clock sleep state. */
void hpi_ui_set_brightness(uint8_t pct);

/* Drop to the ambient clock now, as if the idle timeout had expired. Used by
 * the power menu's Sleep entry. The state is latched, so the idle logic does
 * not wake the display again on the next loop. */
void hpi_ui_request_sleep(void);

#ifdef __cplusplus
}
#endif

#endif /* HPI_UI_MODULE_H */
