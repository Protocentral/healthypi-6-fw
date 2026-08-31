/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Group 64 diagnostics handlers:
 *   0x0080 run_selftest  -- a fixed set of on-demand readiness checks, returned
 *                           as one CBOR report.
 *   0x0081 lead_off      -- ECG electrode state + heart-rate provenance, the
 *                           host-side twin of the panel's lead-off banner.
 */

#include "hpi_mgmt_group.h"
#include "core/acquisition.h"       /* debounced ECG lead-off mask */
#include "core/sample_formats.h"    /* HP6_LEAD_OFF_* / HP6_VIT_* */
#include "services/power_service.h"
#include "platform/fs_mount.h"
#include "platform/ipc.h"
#include "platform/health.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zcbor_common.h>
#include <zcbor_encode.h>

LOG_MODULE_DECLARE(hpi_mgmt, LOG_LEVEL_INF);

/* EOL suite version must be a deliberate, non-zero value (factory pins it). */
BUILD_ASSERT(CONFIG_HPI_EOL_SUITE_VER > 0, "CONFIG_HPI_EOL_SUITE_VER must be > 0");

static bool node_ready(const struct device *d)
{
    return d != NULL && device_is_ready(d);
}

int hpi_diag_run_selftest(struct smp_streamer *ctxt)
{
    zcbor_state_t *zse = ctxt->writer->zs;

    struct hpi_power_status p;
    hpi_power_get(&p);

    bool sd   = platform_fs_is_ready();
    bool batt = p.valid;
    bool ecg  = node_ready(DEVICE_DT_GET(DT_NODELABEL(ads129xx)));
    bool ppg  = node_ready(DEVICE_DT_GET(DT_NODELABEL(afe4400)));
    bool m4   = hpi_ipc_ready();
    /* QSPI NOR readiness: the st,stm32-qspi-nor driver reads the JEDEC ID at
     * init and fails to become ready if the flash is absent/mis-IDed, so this
     * is a safe runtime proxy for the manufacturing "QSPI flash ID" check. */
    bool qspi = node_ready(DEVICE_DT_GET(DT_NODELABEL(w25q01jv)));

    uint32_t pass = (uint32_t)sd + batt + ecg + ppg + m4 + qspi;
    uint32_t fail = 6u - pass;

    LOG_INF("selftest[v%d]: sd=%d batt=%d ecg=%d ppg=%d m4=%d qspi=%d (%u/6 pass)",
            CONFIG_HPI_EOL_SUITE_VER, sd, batt, ecg, ppg, m4, qspi, pass);

    /* per-subsystem health states (0=unknown,1=ok,2=degraded,3=failed). */
    struct hpi_health_report hr;
    hpi_health_snapshot(&hr);

    bool ok =
        zcbor_tstr_put_lit(zse, "suite_ver") && zcbor_uint32_put(zse, CONFIG_HPI_EOL_SUITE_VER) &&
        zcbor_tstr_put_lit(zse, "sd")   && zcbor_bool_put(zse, sd) &&
        zcbor_tstr_put_lit(zse, "batt") && zcbor_bool_put(zse, batt) &&
        zcbor_tstr_put_lit(zse, "ecg")  && zcbor_bool_put(zse, ecg) &&
        zcbor_tstr_put_lit(zse, "ppg")  && zcbor_bool_put(zse, ppg) &&
        zcbor_tstr_put_lit(zse, "m4")   && zcbor_bool_put(zse, m4) &&
        zcbor_tstr_put_lit(zse, "qspi") && zcbor_bool_put(zse, qspi) &&
        zcbor_tstr_put_lit(zse, "pass") && zcbor_uint32_put(zse, pass) &&
        zcbor_tstr_put_lit(zse, "fail") && zcbor_uint32_put(zse, fail) &&
        zcbor_tstr_put_lit(zse, "health_overall") && zcbor_uint32_put(zse, hr.overall) &&
        zcbor_tstr_put_lit(zse, "health") && zcbor_map_start_encode(zse, HPI_SUBSYS_COUNT) &&
        zcbor_tstr_put_lit(zse, "acq")  && zcbor_uint32_put(zse, hr.e[HPI_SUBSYS_ACQ].state) &&
        zcbor_tstr_put_lit(zse, "m4")   && zcbor_uint32_put(zse, hr.e[HPI_SUBSYS_M4_IPC].state) &&
        zcbor_tstr_put_lit(zse, "stream") && zcbor_uint32_put(zse, hr.e[HPI_SUBSYS_STREAM].state) &&
        zcbor_tstr_put_lit(zse, "rec")  && zcbor_uint32_put(zse, hr.e[HPI_SUBSYS_RECORDING].state) &&
        zcbor_tstr_put_lit(zse, "hl")   && zcbor_uint32_put(zse, hr.e[HPI_SUBSYS_HEALTHYLINK].state) &&
        zcbor_map_end_encode(zse, HPI_SUBSYS_COUNT);

    return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

/* 0x0081 diag_lead_off -- ECG electrode state + which sensor the reported
 * heart rate came from (the same debounced mask and provenance flags the
 * panel shows; without them a host cannot tell an ECG rate from a PPG pulse
 * rate). `age_ms` is how long ago acquisition last updated the mask: a large
 * value means the ECG front end has gone quiet, which is NOT the same as
 * "leads on"; `ok` says whether the state is fresh enough to act on.
 */
#define LEAD_OFF_FRESH_MS 5000

int hpi_diag_lead_off_read(struct smp_streamer *ctxt)
{
    zcbor_state_t *zse = ctxt->writer->zs;
    uint32_t age = UINT32_MAX;
    uint8_t mask = hpi_acquisition_lead_off(&age);
    bool fresh = (age <= LEAD_OFF_FRESH_MS);
    struct hp6_vitals v;

    hpi_ipc_last_vitals(&v);

    bool ok =
        zcbor_tstr_put_lit(zse, "mask")   && zcbor_uint32_put(zse, mask) &&
        zcbor_tstr_put_lit(zse, "ra")     && zcbor_bool_put(zse, mask & HP6_LEAD_OFF_RA) &&
        zcbor_tstr_put_lit(zse, "la")     && zcbor_bool_put(zse, mask & HP6_LEAD_OFF_LA) &&
        zcbor_tstr_put_lit(zse, "ll")     && zcbor_bool_put(zse, mask & HP6_LEAD_OFF_LL) &&
        zcbor_tstr_put_lit(zse, "v1")     && zcbor_bool_put(zse, mask & HP6_LEAD_OFF_V1) &&
        zcbor_tstr_put_lit(zse, "age_ms") && zcbor_uint32_put(zse, age) &&
        zcbor_tstr_put_lit(zse, "ok")     && zcbor_bool_put(zse, fresh) &&
        zcbor_tstr_put_lit(zse, "hr")     && zcbor_uint32_put(zse, v.hr_bpm) &&
        zcbor_tstr_put_lit(zse, "hr_src") &&
            zcbor_uint32_put(zse, (v.flags & HP6_VIT_HR_FROM_PPG) ? 1u : 0u) &&
        zcbor_tstr_put_lit(zse, "vflags") && zcbor_uint32_put(zse, v.flags);

    return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}
