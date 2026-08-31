/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Group 64 HealthyLink module handlers -- thin adapters over the
 * HealthyLink framework service.
 *
 *   0x0050 hpi/module_list   -> per-slot { state, id, name, powered }
 *   0x0052 hpi/module_power  { slot, on } -> { ok }
 */

#include "hpi_mgmt_group.h"
#include "healthylink/healthylink_service.h"
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

static bool encode_slot(zcbor_state_t *zse, const char *key, hl_slot_t slot)
{
    struct hl_slot_status st;
    if (hl_get_slot_status(slot, &st) != 0) {
        memset(&st, 0, sizeof(st));
    }
    return zcbor_tstr_put_term(zse, key, 8) &&
           zcbor_map_start_encode(zse, 4) &&
           zcbor_tstr_put_lit(zse, "state") && zcbor_uint32_put(zse, st.state) &&
           zcbor_tstr_put_lit(zse, "id")    && zcbor_uint32_put(zse, st.module_id) &&
           zcbor_tstr_put_lit(zse, "pwr")   && zcbor_bool_put(zse, st.powered) &&
           zcbor_tstr_put_lit(zse, "name")  && zcbor_tstr_put_term(zse, st.name, sizeof(st.name)) &&
           zcbor_map_end_encode(zse, 4);
}

int hpi_module_list_read(struct smp_streamer *ctxt)
{
    zcbor_state_t *zse = ctxt->writer->zs;
    bool ok = encode_slot(zse, "a", HL_SLOT_A) &&
              encode_slot(zse, "b", HL_SLOT_B);
    return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

int hpi_module_power_write(struct smp_streamer *ctxt)
{
    zcbor_state_t *zsd = ctxt->reader->zs;
    zcbor_state_t *zse = ctxt->writer->zs;

    /* §14.3: switching module power rails is privileged -- gate on unlock. */
    int gate = hpi_security_require_unlocked();
    if (gate != MGMT_ERR_EOK) {
        return gate;
    }

    uint32_t slot = 0;
    bool on = false;
    size_t decoded = 0;
    struct zcbor_map_decode_key_val decoders[] = {
        ZCBOR_MAP_DECODE_KEY_DECODER("slot", zcbor_uint32_decode, &slot),
        ZCBOR_MAP_DECODE_KEY_DECODER("on",   zcbor_bool_decode,   &on),
    };
    if (zcbor_map_decode_bulk(zsd, decoders, ARRAY_SIZE(decoders), &decoded) != 0 ||
        slot >= HL_NUM_SLOTS) {
        return MGMT_ERR_EINVAL;
    }

    int rc = hl_set_slot_power((hl_slot_t)slot, on);
    bool ok = zcbor_tstr_put_lit(zse, "ok") && zcbor_bool_put(zse, rc == 0);
    return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}
