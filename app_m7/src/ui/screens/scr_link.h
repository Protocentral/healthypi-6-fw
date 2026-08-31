/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Link — connectivity control panel (More -> Link). Wi-Fi and BLE status +
 * enable are live from the connectivity service; the USB row reflects
 * capability. Pure L5.
 */
#ifndef HPI_UI_SCR_LINK_H
#define HPI_UI_SCR_LINK_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *hpi_scr_link_create(lv_obj_t *parent);
void hpi_scr_link_refresh(void);   /* poll Wi-Fi status; call ~2 Hz when active */

#ifdef __cplusplus
}
#endif

#endif /* HPI_UI_SCR_LINK_H */
