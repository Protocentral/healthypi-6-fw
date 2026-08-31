/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * HealthyLink module-provider contract -- the isolation boundary.
 *
 * A module is one self-contained file that registers a provider; the core
 * enumerates providers via the iterable section (no central table). A module
 * produces samples via hpi_bus_publish() exactly like the onboard sensors.
 * LLEXT-loaded modules implement the same vtable (CONFIG_LLEXT, dev only --
 * customer enablement gated on extension code-signing).
 *
 * Fault isolation: each active module runs under a supervisor thread with its
 * own watchdog channel and call timeouts. A fault/hang/missed-watchdog ->
 * stop(), power down the slot LDO, quarantine, emit an event -- core
 * acquisition and display keep running.
 */

#ifndef HPI_HEALTHYLINK_PROVIDER_H
#define HPI_HEALTHYLINK_PROVIDER_H

#include <stdint.h>
#include <stdbool.h>
#include <zephyr/sys/iterable_sections.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Slot identifiers (DF9 dual-connector). */
typedef enum { HL_SLOT_A = 0, HL_SLOT_B = 1 } hl_slot_t;

/* Per-activation context handed to provider ops. */
struct hl_ctx {
    hl_slot_t slot;
    uint16_t  module_id;     /* from EEPROM */
    void     *priv;          /* provider-owned */
};

struct hl_test_result {
    uint8_t status;          /* 0=PASS 1=FAIL 2=SKIP 3=MANUAL */
    char    detail[32];
};

/*
 * Provider vtable. All ops must honor their supervisor timeout and must
 * not block the acquisition or UI threads.
 *   probe   -- claim resources via the arbiter; NO side effects on core.
 *   start   -- begin producing into the sample bus.
 *   stop    -- cease production, release for power-down.
 *   selftest- optional on-demand check.
 *   ctrl    -- optional group-64 sub-namespace command handler.
 */
struct hl_module_provider {
    uint16_t    module_id;       /* matches EEPROM Module ID */
    const char *name;
    uint32_t    caps;            /* HLNK_CAP_* bitmap (resource needs) */
    int (*probe)(struct hl_ctx *ctx);
    int (*start)(struct hl_ctx *ctx);
    int (*stop)(struct hl_ctx *ctx);
    int (*selftest)(struct hl_ctx *ctx, struct hl_test_result *out);
    int (*ctrl)(struct hl_ctx *ctx, uint16_t cmd, const void *in, void *out);
};

/* Register a static provider. Place one per module .c file. */
#define HL_MODULE_REGISTER(_p) \
    STRUCT_SECTION_ITERABLE(hl_module_provider, _p)

/* Framework entry points (implemented in registry.c / supervisor.c). */
int  hl_framework_init(void);                 /* scan EEPROMs, lazy probe/start */
int  hl_slot_power(hl_slot_t slot, bool on);  /* arbiter/PMIC-gated LDO */
int  hl_slot_quarantine(hl_slot_t slot);      /* supervisor fault path */

#ifdef __cplusplus
}
#endif

#endif /* HPI_HEALTHYLINK_PROVIDER_H */
