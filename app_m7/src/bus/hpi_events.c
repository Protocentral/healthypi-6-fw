/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>

#include "hpi_events.h"

ZBUS_CHAN_DEFINE(hpi_sys_evt_chan,          /* channel name */
                 struct hpi_sys_evt,        /* message type */
                 NULL,                      /* validator */
                 NULL,                      /* user data */
                 ZBUS_OBSERVERS_EMPTY,      /* observers added in later steps */
                 ZBUS_MSG_INIT(0));         /* initial value */

void hpi_events_publish(uint16_t type, int32_t arg)
{
    struct hpi_sys_evt e = {
        .type = type,
        .t_ms = k_uptime_get_32(),
        .arg = arg,
    };
    /* Best-effort: a short timeout so a producer never stalls on the bus. */
    (void)zbus_chan_pub(&hpi_sys_evt_chan, &e, K_MSEC(5));
}
