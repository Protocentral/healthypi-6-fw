/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Group 64 HealthyLink module handlers -- thin adapters over the
 * HealthyLink framework service.
 *
 *   0x0050 hpi/module_list         -> per-slot { state, id, name, powered }
 *   0x0052 hpi/module_power        { slot, on } -> { ok }
 *   0x0053 hpi/module_eeprom_read  { slot, off, len } -> { off, data }
 *   0x0054 hpi/module_eeprom_write { slot, off, data } -> { off, len }
 *   0x0055 hpi/module_i2c_scan     { slot, pwr } -> { addrs }
 *
 * The EEPROM pair moves raw bytes and nothing else -- no magic check, no CRC,
 * no notion of what a module ID means. That is the host tool's job
 * (`healthypi.hw.eeprom`), and keeping the device out of it is what lets a
 * module type that did not exist when this firmware shipped be programmed
 * without new firmware. The device's own check happens where it has to:
 * detect verifies the CRC and refuses to power an image it cannot trust.
 */

#include "hpi_mgmt_group.h"
#include "healthylink/healthylink_service.h"
#include "control/security/hpi_security.h"

#include <errno.h>
#include <stdint.h>
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

/* One command moves at most this many bytes, which keeps a request and its
 * reply inside the SMP MTU with room for the CBOR map around them. A 256-byte
 * image is 4 writes. */
#define HPI_EEPROM_CHUNK_MAX 64

/* Map the service's errno onto an SMP error.
 *
 * The two namespaces are NOT interchangeable. A bare return value is the
 * protocol-wide MGMT_ERR set and goes on the wire as {"rc": n}; a group-64
 * code (>= 256) means nothing there and must be encoded as an err map with
 * its group id. Returning one directly produces a reply no client can parse:
 * {"rc": 257} is not a protocol error code, so the response matches neither
 * the success model nor either error model. */
static int eeprom_err(zcbor_state_t *zse, int rc)
{
    switch (rc) {
    case 0:        return MGMT_ERR_EOK;
    case -EINVAL:  return MGMT_ERR_EINVAL;
    case -ENOTSUP: return MGMT_ERR_ENOTSUP;
    default:       break;
    }

    uint16_t code;

    switch (rc) {
    /* Nothing acknowledged at the slot's I2C address: an empty slot, a module
     * that is not seated, or one with no ID EEPROM. Not a fault, and the
     * distinction is not knowable from here -- NOT_READY says "there is
     * nothing to talk to" without inventing a reason. */
    case -ENODEV:
    case -ENXIO:
    case -EIO:
    case -ETIMEDOUT:
        code = HPI_MGMT_ERR_NOT_READY;
        break;
    default:
        code = HPI_MGMT_ERR_HW_FAULT;
        break;
    }

    bool ok = smp_add_cmd_err(zse, HPI_MGMT_GROUP_ID, code);
    return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

int hpi_module_eeprom_read(struct smp_streamer *ctxt)
{
    zcbor_state_t *zsd = ctxt->reader->zs;
    zcbor_state_t *zse = ctxt->writer->zs;

    /* hl_eeprom_read retries with the slot rail on if the unpowered probe
     * NAKs -- that pulses EN_MOD_x, the same privilege as module_i2c_scan. */
    int gate = hpi_security_require_unlocked();
    if (gate != MGMT_ERR_EOK) {
        return gate;
    }

    uint32_t slot = 0, off = 0, len = 0;
    size_t decoded = 0;
    struct zcbor_map_decode_key_val decoders[] = {
        ZCBOR_MAP_DECODE_KEY_DECODER("slot", zcbor_uint32_decode, &slot),
        ZCBOR_MAP_DECODE_KEY_DECODER("off",  zcbor_uint32_decode, &off),
        ZCBOR_MAP_DECODE_KEY_DECODER("len",  zcbor_uint32_decode, &len),
    };
    if (zcbor_map_decode_bulk(zsd, decoders, ARRAY_SIZE(decoders), &decoded) != 0 ||
        slot >= HL_NUM_SLOTS || len == 0 || len > HPI_EEPROM_CHUNK_MAX ||
        off > UINT8_MAX) {
        return MGMT_ERR_EINVAL;
    }

    uint8_t buf[HPI_EEPROM_CHUNK_MAX];
    int rc = hl_eeprom_read((hl_slot_t)slot, (uint8_t)off, buf, len);

    if (rc != 0) {
        return eeprom_err(zse, rc);
    }

    bool ok = zcbor_tstr_put_lit(zse, "off") && zcbor_uint32_put(zse, off) &&
              zcbor_tstr_put_lit(zse, "data") && zcbor_bstr_encode_ptr(zse, (const char *)buf, len);
    return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

int hpi_module_eeprom_write(struct smp_streamer *ctxt)
{
    zcbor_state_t *zsd = ctxt->reader->zs;
    zcbor_state_t *zse = ctxt->writer->zs;

    /* §14.3: this rewrites a module's identity, which the arbiter then acts
     * on -- privileged, like module_power. */
    int gate = hpi_security_require_unlocked();
    if (gate != MGMT_ERR_EOK) {
        return gate;
    }

    uint32_t slot = 0, off = 0;
    struct zcbor_string data = {0};
    size_t decoded = 0;
    struct zcbor_map_decode_key_val decoders[] = {
        ZCBOR_MAP_DECODE_KEY_DECODER("slot", zcbor_uint32_decode, &slot),
        ZCBOR_MAP_DECODE_KEY_DECODER("off",  zcbor_uint32_decode, &off),
        ZCBOR_MAP_DECODE_KEY_DECODER("data", zcbor_bstr_decode,   &data),
    };
    if (zcbor_map_decode_bulk(zsd, decoders, ARRAY_SIZE(decoders), &decoded) != 0 ||
        slot >= HL_NUM_SLOTS || data.len == 0 || data.len > HPI_EEPROM_CHUNK_MAX ||
        off > UINT8_MAX) {
        return MGMT_ERR_EINVAL;
    }

    int rc = hl_eeprom_write((hl_slot_t)slot, (uint8_t)off, data.value, data.len);

    if (rc != 0) {
        return eeprom_err(zse, rc);
    }

    /* Echo what was written so the host can drive the next chunk from the
     * reply rather than from its own bookkeeping. */
    bool ok = zcbor_tstr_put_lit(zse, "off") && zcbor_uint32_put(zse, off) &&
              zcbor_tstr_put_lit(zse, "len") &&
              zcbor_uint32_put(zse, (uint32_t)data.len);
    return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

int hpi_module_i2c_scan(struct smp_streamer *ctxt)
{
    zcbor_state_t *zsd = ctxt->reader->zs;
    zcbor_state_t *zse = ctxt->writer->zs;

    /* Scanning is harmless -- a zero-length write that reads and writes
     * nothing -- but `pwr` energises a module whose identity is unknown, which
     * is the same privilege module_power carries. */
    int gate = hpi_security_require_unlocked();
    if (gate != MGMT_ERR_EOK) {
        return gate;
    }

    uint32_t slot = 0;
    bool pwr = false;
    size_t decoded = 0;
    struct zcbor_map_decode_key_val decoders[] = {
        ZCBOR_MAP_DECODE_KEY_DECODER("slot", zcbor_uint32_decode, &slot),
        ZCBOR_MAP_DECODE_KEY_DECODER("pwr",  zcbor_bool_decode,   &pwr),
    };
    if (zcbor_map_decode_bulk(zsd, decoders, ARRAY_SIZE(decoders), &decoded) != 0 ||
        slot >= HL_NUM_SLOTS) {
        return MGMT_ERR_EINVAL;
    }

    uint8_t addrs[HPI_EEPROM_CHUNK_MAX];
    int n = hl_bus_scan((hl_slot_t)slot, pwr, addrs, sizeof(addrs));

    if (n < 0) {
        return eeprom_err(zse, n);
    }

    bool ok = zcbor_tstr_put_lit(zse, "addrs") &&
              zcbor_bstr_encode_ptr(zse, (const char *)addrs, (size_t)n);
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
