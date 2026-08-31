/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Hardware watchdog (STM32 IWDG) -- the reset-of-last-resort backstop.
 * Per-subsystem task watchdogs are layered on top of this.
 */

#ifndef HPI_PLATFORM_WATCHDOG_H
#define HPI_PLATFORM_WATCHDOG_H

#ifdef __cplusplus
extern "C" {
#endif

/* Install + start the IWDG with CONFIG_HPI_WATCHDOG_TIMEOUT_MS.
 * Returns 0 on success, -ENODEV if no watchdog node is enabled (bring-up
 * continues without it), or a negative errno on driver error. */
int platform_watchdog_init(void);

/* Feed the watchdog. Safe to call when the watchdog is absent (no-op). */
void platform_watchdog_feed(void);

/* True if a hardware watchdog is active. */
bool platform_watchdog_active(void);

#ifdef __cplusplus
}
#endif

#endif /* HPI_PLATFORM_WATCHDOG_H */
