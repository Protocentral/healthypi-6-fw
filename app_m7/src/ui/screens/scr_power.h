/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Power menu -- the modal a 5 s button hold opens. It deliberately offers no
 * "power off": the board is switched by a slide switch and firmware cannot cut
 * its own rail, so the menu lists only actions the firmware can actually do.
 *
 * Entries appear only when they apply (recovery only on a bootloader build,
 * stop-recording only while recording); an option that cannot work is hidden,
 * never greyed out.
 *
 * Full-screen overlay, so it returns to whatever was underneath. Pure L5:
 * every entry is a call into a service.
 */
#ifndef HPI_UI_SCR_POWER_H
#define HPI_UI_SCR_POWER_H

#include <lvgl.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void hpi_scr_power_show(lv_obj_t *screen);  /* create overlay (top of `screen`) */
void hpi_scr_power_hide(void);              /* delete overlay */
bool hpi_scr_power_active(void);

#ifdef __cplusplus
}
#endif

#endif /* HPI_UI_SCR_POWER_H */
