/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Group 64 SD-recording handlers -- thin adapters over the recording
 * service. No recording logic here.
 *
 *   0x0060 hpi/sd_status        -> { active, bytes, dur, ecg, ppg, path }
 *   0x0061 hpi/sd_record_start  { name? } -> { path }
 *   0x0062 hpi/sd_record_stop   -> { }
 */

#include "hpi_mgmt_group.h"
#include "services/recording_service.h"
#include "control/security/hpi_security.h"

#include <string.h>
#include <zephyr/logging/log.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zcbor_common.h>
#include <zcbor_decode.h>
#include <zcbor_encode.h>
#include <mgmt/mcumgr/util/zcbor_bulk.h>

LOG_MODULE_DECLARE(hpi_mgmt, LOG_LEVEL_INF);

int hpi_recording_status_read(struct smp_streamer *ctxt)
{
    zcbor_state_t *zse = ctxt->writer->zs;
    struct hpi_recording_status st;
    hpi_recording_get_status(&st);

    bool ok =
        zcbor_tstr_put_lit(zse, "active") && zcbor_bool_put(zse, st.active) &&
        zcbor_tstr_put_lit(zse, "bytes")  && zcbor_uint32_put(zse, st.bytes_written) &&
        zcbor_tstr_put_lit(zse, "dur")    && zcbor_uint32_put(zse, st.duration_ms) &&
        zcbor_tstr_put_lit(zse, "ecg")    && zcbor_uint32_put(zse, st.ecg_samples) &&
        zcbor_tstr_put_lit(zse, "ppg")    && zcbor_uint32_put(zse, st.ppg_samples) &&
        zcbor_tstr_put_lit(zse, "path")   && zcbor_tstr_put_term(zse, st.path, sizeof(st.path));
    return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

int hpi_recording_start_write(struct smp_streamer *ctxt)
{
    zcbor_state_t *zsd = ctxt->reader->zs;
    zcbor_state_t *zse = ctxt->writer->zs;

    /* §14.3: recording captures PHI to SD -- gate on unlock. */
    int gate = hpi_security_require_unlocked();
    if (gate != MGMT_ERR_EOK) {
        return gate;
    }

    struct zcbor_string name = {0};
    size_t decoded = 0;
    struct zcbor_map_decode_key_val decoders[] = {
        ZCBOR_MAP_DECODE_KEY_DECODER("name", zcbor_tstr_decode, &name),
    };
    (void)zcbor_map_decode_bulk(zsd, decoders, ARRAY_SIZE(decoders), &decoded);

    char namebuf[64] = {0};
    if (name.len > 0 && name.len < sizeof(namebuf)) {
        memcpy(namebuf, name.value, name.len);
    }

    int rc = hpi_recording_start(namebuf[0] ? namebuf : NULL);
    if (rc == -EBUSY) {
        bool ok = smp_add_cmd_err(zse, HPI_MGMT_GROUP_ID, HPI_MGMT_ERR_NOT_READY);
        return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
    }
    if (rc == -ENODEV) {
        bool ok = smp_add_cmd_err(zse, HPI_MGMT_GROUP_ID, HPI_MGMT_ERR_NOT_READY);
        return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
    }
    if (rc != 0) {
        return MGMT_ERR_EUNKNOWN;
    }

    struct hpi_recording_status st;
    hpi_recording_get_status(&st);
    bool ok = zcbor_tstr_put_lit(zse, "path") &&
              zcbor_tstr_put_term(zse, st.path, sizeof(st.path));
    return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

int hpi_recording_stop_write(struct smp_streamer *ctxt)
{
    ARG_UNUSED(ctxt);
    (void)hpi_recording_stop();
    return MGMT_ERR_EOK;
}
