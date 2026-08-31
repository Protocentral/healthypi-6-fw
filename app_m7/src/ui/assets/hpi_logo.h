/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * HealthyPi 6 brand lockup for the on-device UI.
 *
 * The artwork is two colours over an alpha channel, so it ships as two
 * LV_COLOR_FORMAT_A8 masks on a shared canvas rather than one RGB565A8 image:
 * a third of the flash, and the brand colours stay in hpi_m3_theme.h instead of
 * being baked into the asset. Both layers are full-canvas so they scale about
 * the same origin and cannot drift apart.
 *
 * Do not use these directly -- hpi_ui_logo_create() stacks and recolours them.
 * Regenerate with ui/assets/convert_logo.py.
 */
#ifndef HPI_LOGO_H_
#define HPI_LOGO_H_

#include <lvgl.h>

/* Native canvas of the masters below, in device pixels (= the boot splash size
 * from redesign 2a). Smaller placements scale this down. */
#define HPI_LOGO_LOCKUP_W  312
#define HPI_LOGO_LOCKUP_H  119

/** "HealthyPi" wordmark layer -- painted HPI_M3_ON_SURFACE. */
extern const lv_image_dsc_t hpi_logo_lockup_light;

/** 6 / hexagon / ECG trace layer -- painted HPI_M3_SIG_ECG. */
extern const lv_image_dsc_t hpi_logo_lockup_accent;

#endif /* HPI_LOGO_H_ */
