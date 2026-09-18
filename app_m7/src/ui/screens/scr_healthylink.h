/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * HealthyLink — expansion module status (More -> HealthyLink). Slot state from
 * the HealthyLink framework service, module link state from the Compute
 * provider's cached HLink handshake. Read-only, and pure L5: it consumes two
 * snapshot accessors and adds no capability of its own.
 */
#ifndef HPI_UI_SCR_HEALTHYLINK_H
#define HPI_UI_SCR_HEALTHYLINK_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

struct hp6_infer_sample;

lv_obj_t *hpi_scr_healthylink_create(lv_obj_t *parent);
void hpi_scr_healthylink_refresh(void);   /* snapshot reads; ~2 Hz when active */
void hpi_scr_healthylink_set_infer(const struct hp6_infer_sample *s);

#ifdef __cplusplus
}
#endif

#endif /* HPI_UI_SCR_HEALTHYLINK_H */
