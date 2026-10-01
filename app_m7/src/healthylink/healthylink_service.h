/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * HealthyLink framework service (L4) -- boot enumeration + lifecycle + fault
 * isolation for expansion modules. At boot it detects each slot's module (via
 * the out-of-tree HealthyLink driver's per-slot EEPROM read), matches a
 * registered provider (hl_provider.h, iterable section), claims resources
 * (hl_arbiter), powers the slot, and runs probe()/start() under a supervisor
 * that quarantines a faulting module without disturbing core acquisition.
 *
 * Any module may sit in any slot; providers are told which slot they are in.
 * The group-64 module commands and the HealthyLink screen use the accessors
 * here.
 */

#ifndef HPI_HEALTHYLINK_SERVICE_H
#define HPI_HEALTHYLINK_SERVICE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "hl_provider.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The numeric values are on the wire (group-64 module_list `state`,
 * docs/MCUMGR_COMMANDS.md §7.5): append only. */
enum hl_slot_state {
    HL_SLOT_EMPTY = 0,      /* no module detected */
    HL_SLOT_UNSUPPORTED,    /* module present, no matching provider */
    HL_SLOT_ACTIVE,         /* provider started, producing */
    HL_SLOT_ERROR,          /* probe/claim/start failed */
    HL_SLOT_QUARANTINED,    /* faulted at runtime, powered down */
    HL_SLOT_OFF,            /* module present, powered down on request */
};

struct hl_slot_status {
    uint8_t  state;         /* enum hl_slot_state */
    uint16_t module_id;
    uint32_t caps;
    bool     powered;       /* load switch driven on (the command, not the rail) */
    bool     detectable;    /* false: no ID EEPROM path, state is a default */
    bool     busy;          /* a power change is being applied right now */
    char     name[32];
};

#define HL_NUM_SLOTS 2

/* A mutex and a struct copy, no I/O: safe from the LVGL thread. */
int  hl_get_slot_status(hl_slot_t slot, struct hl_slot_status *out);

/*
 * Switch a slot, synchronously. ON re-detects the slot and brings up what it
 * finds (so it also retries a failed slot and rescans an empty one); OFF stops
 * the provider, releases its interfaces and cuts the rail.
 *
 * Returns 0 if ON leaves the slot ACTIVE, or after OFF; -ENODEV if ON found no
 * module; -EIO if ON found one but could not bring it up.
 */
int  hl_set_slot_power(hl_slot_t slot, bool on);

/* The same, queued to the system work queue (for the LVGL thread). Status
 * reports `busy` until it has been applied. */
int  hl_request_slot_power(hl_slot_t slot, bool on);

/*
 * Raw access to a slot's ID EEPROM (the 256-byte image documented in
 * include/healthylink/healthylink_eeprom.h), so a module can be identified in
 * the field with nothing but a USB cable -- the alternative is an external
 * I2C programmer and the module off the board.
 *
 * Deliberately dumb: bytes in, bytes out, no interpretation. The image format,
 * its CRC and what a module ID means are the host tool's business
 * (`healthypi.hw.eeprom`), and keeping the device out of that argument is what
 * lets a new module type be programmed without new firmware. The device's own
 * validation happens where it matters -- at detect, which rejects a bad CRC.
 *
 * A module's ID EEPROM is meant to sit on the host's always-on rail, so
 * neither call normally touches slot power. When nothing acknowledges, both
 * retry once with the rail on -- a module that feeds its EEPROM from its own
 * regulator, behind the load switch, is otherwise indistinguishable from an
 * empty slot -- and put the rail back as they found it. Neither disturbs a
 * running module. A written identity takes effect at the next detect:
 * hl_set_slot_power(slot, true).
 *
 * Returns 0, -EINVAL for a bad slot or a range outside the 256-byte image,
 * -ENODEV if the slot has no EEPROM path, or the I2C errno.
 */
int  hl_eeprom_read(hl_slot_t slot, uint8_t off, uint8_t *buf, size_t len);
int  hl_eeprom_write(hl_slot_t slot, uint8_t off, const uint8_t *data, size_t len);

/*
 * Probe every address on the slot's I2C bus, optionally with the rail on, and
 * put the rail back as it was found. Returns the count, or a negative errno.
 *
 * For bring-up: on v5 the two slot EEPROMs are the only devices on that bus,
 * so a slot that answers nothing cannot be told apart from a bus that is not
 * working -- and "the module is at the wrong address" looks the same as "there
 * is no module". A scan separates all three.
 */
int  hl_bus_scan(hl_slot_t slot, bool powered, uint8_t *addrs, size_t max);

#ifdef __cplusplus
}
#endif

#endif /* HPI_HEALTHYLINK_SERVICE_H */
