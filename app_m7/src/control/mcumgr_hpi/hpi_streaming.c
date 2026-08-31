/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Group 64 streaming-control handlers -- thin adapters over the
 * stream service (services/stream_service.h). No stream logic lives here.
 *
 *   0x0020 hpi/stream_start  { ch, ann } -> enable CDC0 .HP6 stream
 *   0x0021 hpi/stream_stop                -> disable (idempotent)
 *   0x0022 hpi/stream_status -> { active, ch, ann, sent, dropped }
 */

#include "hpi_mgmt_group.h"
#include "services/stream_service.h"
#include "control/security/hpi_security.h"

#include <zephyr/logging/log.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zcbor_common.h>
#include <zcbor_decode.h>
#include <zcbor_encode.h>
#include <mgmt/mcumgr/util/zcbor_bulk.h>

LOG_MODULE_DECLARE(hpi_mgmt, LOG_LEVEL_INF);

int hpi_streaming_start(struct smp_streamer *ctxt)
{
    zcbor_state_t *zsd = ctxt->reader->zs;
    zcbor_state_t *zse = ctxt->writer->zs;

    /* §14.3: live stream carries PHI -- gate on unlock. */
    int gate = hpi_security_require_unlocked();
    if (gate != MGMT_ERR_EOK) {
        return gate;
    }

    uint32_t ch = 0;
    uint32_t ann = 0;
    size_t decoded = 0;

    struct zcbor_map_decode_key_val decoders[] = {
        ZCBOR_MAP_DECODE_KEY_DECODER("ch",  zcbor_uint32_decode, &ch),
        ZCBOR_MAP_DECODE_KEY_DECODER("ann", zcbor_uint32_decode, &ann),
    };

    if (zcbor_map_decode_bulk(zsd, decoders, ARRAY_SIZE(decoders), &decoded) != 0) {
        return MGMT_ERR_EINVAL;
    }

    /* Clamp to documented bit layouts; unknown bits dropped so a newer host
     * never bricks older firmware. */
    uint8_t ch_mask  = (uint8_t)(ch  & 0x0Fu);
    uint8_t ann_mask = (uint8_t)(ann & 0x1Fu);

    int rc = hpi_stream_enable(ch_mask, ann_mask);
    if (rc == -ENOTSUP) {
        bool ok = smp_add_cmd_err(zse, HPI_MGMT_GROUP_ID,
                      HPI_MGMT_ERR_CHANNEL_NOT_AVAILABLE);
        return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
    }
    if (rc != 0) {
        return MGMT_ERR_EUNKNOWN;
    }
    return MGMT_ERR_EOK;
}

int hpi_streaming_stop(struct smp_streamer *ctxt)
{
    ARG_UNUSED(ctxt);
    hpi_stream_disable();
    return MGMT_ERR_EOK;
}

int hpi_streaming_status(struct smp_streamer *ctxt)
{
    zcbor_state_t *zse = ctxt->writer->zs;
    struct hpi_stream_status st;

    hpi_stream_get_status(&st);

    bool ok =
        zcbor_tstr_put_lit(zse, "active")  && zcbor_bool_put(zse, st.active) &&
        zcbor_tstr_put_lit(zse, "ch")      && zcbor_uint32_put(zse, st.ch_mask) &&
        zcbor_tstr_put_lit(zse, "ann")     && zcbor_uint32_put(zse, st.ann_mask) &&
        zcbor_tstr_put_lit(zse, "sent")    && zcbor_uint32_put(zse, st.frames_sent) &&
        zcbor_tstr_put_lit(zse, "dropped") && zcbor_uint32_put(zse, st.frames_dropped);

    return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}
