/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * HealthyLink resource arbiter -- prevents two slots from claiming the same
 * shared interface (SPI4/SPI6/USART2/FDCAN/ADC/...). A provider's `caps`
 * (HEALTHYLINK_CAP_REQUIRES_* bits) are the resources it needs; the arbiter
 * grants them atomically per slot or refuses with -EBUSY so a misconfigured
 * second module can never corrupt the first.
 */

#ifndef HPI_HEALTHYLINK_ARBITER_H
#define HPI_HEALTHYLINK_ARBITER_H

#include <stdint.h>
#include "hl_provider.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Claim the required-interface bits in `caps` for `slot`. Returns 0 on success,
 * -EBUSY if any required interface is already held by another slot. */
int  hl_arbiter_claim(hl_slot_t slot, uint32_t caps);

/* Release everything held by `slot`. */
void hl_arbiter_release(hl_slot_t slot);

/* Bitmap of currently-claimed required interfaces (diagnostics). */
uint32_t hl_arbiter_claimed(void);

#ifdef __cplusplus
}
#endif

#endif /* HPI_HEALTHYLINK_ARBITER_H */
