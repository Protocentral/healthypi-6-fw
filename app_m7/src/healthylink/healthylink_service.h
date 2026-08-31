/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * HealthyLink framework service (L4) -- boot enumeration + lifecycle + fault
 * isolation for expansion modules. At boot it detects each slot's module (via
 * the out-of-tree HealthyLink driver's EEPROM read), matches a registered
 * provider (hl_provider.h, iterable section), claims resources (hl_arbiter),
 * powers the slot, and runs probe()/start() under a supervisor that quarantines
 * a faulting module without disturbing core acquisition.
 *
 * The group-64 module commands (control/mcumgr_hpi/hpi_modules.c) read status
 * and toggle slot power through the accessors here.
 */

#ifndef HPI_HEALTHYLINK_SERVICE_H
#define HPI_HEALTHYLINK_SERVICE_H

#include <stdint.h>
#include <stdbool.h>
#include "hl_provider.h"

#ifdef __cplusplus
extern "C" {
#endif

enum hl_slot_state {
    HL_SLOT_EMPTY = 0,      /* no module detected */
    HL_SLOT_UNSUPPORTED,    /* module present, no matching provider */
    HL_SLOT_ACTIVE,         /* provider started, producing */
    HL_SLOT_ERROR,          /* probe/claim/start failed */
    HL_SLOT_QUARANTINED,    /* faulted at runtime, powered down */
};

struct hl_slot_status {
    uint8_t  state;         /* enum hl_slot_state */
    uint16_t module_id;
    uint32_t caps;
    bool     powered;
    char     name[32];
};

#define HL_NUM_SLOTS 2

int  hl_get_slot_status(hl_slot_t slot, struct hl_slot_status *out);
int  hl_set_slot_power(hl_slot_t slot, bool on);   /* host-driven (group 64) */

#ifdef __cplusplus
}
#endif

#endif /* HPI_HEALTHYLINK_SERVICE_H */
