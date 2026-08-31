/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Group 64 telemetry handler -- battery + charge state
 * from the power service. die temperature is a sentinel until the STM32 temp
 * sensor is wired. (fw_versions / 0x0031 lives in hpi_system.c.)
 */

#include "hpi_mgmt_group.h"
#include "services/power_service.h"

#include <zephyr/logging/log.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zcbor_common.h>
#include <zcbor_encode.h>

LOG_MODULE_DECLARE(hpi_mgmt, LOG_LEVEL_INF);

#define TEMP_UNAVAILABLE  (-32768)   /* INT16_MIN sentinel */

int hpi_telemetry_read(struct smp_streamer *ctxt)
{
    zcbor_state_t *zse = ctxt->writer->zs;
    struct hpi_power_status p;
    hpi_power_get(&p);

    bool usb = p.usb_present;
    bool ok =
        zcbor_tstr_put_lit(zse, "vbat_mv") && zcbor_uint32_put(zse, p.vbat_mv) &&
        zcbor_tstr_put_lit(zse, "ibat_ma") && zcbor_int32_put(zse, p.ibat_ma) &&
        zcbor_tstr_put_lit(zse, "soc")     && zcbor_uint32_put(zse, p.soc_pct) &&
        zcbor_tstr_put_lit(zse, "tc_x10")  && zcbor_int32_put(zse, TEMP_UNAVAILABLE) &&
        zcbor_tstr_put_lit(zse, "charge")  && zcbor_uint32_put(zse, p.charge_state) &&
        zcbor_tstr_put_lit(zse, "usb")     && zcbor_bool_put(zse, usb) &&
        zcbor_tstr_put_lit(zse, "batt")    && zcbor_bool_put(zse, !usb) &&
        zcbor_tstr_put_lit(zse, "ok")      && zcbor_bool_put(zse, p.valid);

    return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}
