/*
 * Copyright (c) 2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * DFU service (L4) -- device-side entry into MCUboot serial recovery.
 *
 * MCUboot here is overwrite-only (secondary slot is on QSPI, swap needs
 * same-device scratch), so there is no revert; serial recovery is the fallback
 * for a broken image. Two entry points, no recovery button wired:
 *
 *   1. No bootable image -- CONFIG_BOOT_SERIAL_NO_APPLICATION, entirely inside
 *      the bootloader.
 *   2. Application request -- this service: sets the retained boot-mode flag in
 *      backup SRAM and reboots; MCUboot's CONFIG_BOOT_SERIAL_BOOT_MODE sees it
 *      and stays in recovery.
 *
 * An image that boots and then hangs before honouring a request still needs
 * SWD -- see DFU_MASTER_PLAN §6.
 *
 * Adapter-parity: control/mcumgr_hpi/hpi_recovery.c and the UI are thin
 * clients of this API; neither touches the retention subsystem directly.
 */

#ifndef HPI_DFU_SERVICE_H
#define HPI_DFU_SERVICE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Is a reboot into MCUboot serial recovery possible on this build?
 *
 * False on the dev flavor (no bootloader to receive the flag).
 */
bool hpi_dfu_recovery_available(void);

/**
 * @brief Arm the retained boot-mode flag so the next boot stops in MCUboot
 *        serial recovery.
 *
 * Does NOT reboot -- the caller decides when, so a control session can be
 * closed cleanly first (an SMP reply cannot be sent after the reset).
 *
 * @retval 0        flag armed; the next reset lands in recovery
 * @retval -ENOTSUP this build has no bootloader / no retention backing
 * @retval -EIO     the retention write failed
 */
int hpi_dfu_recovery_arm(void);

/**
 * @brief Clear the retained boot-mode flag.
 *
 * MCUboot clears it itself once it has acted on it, so this is only for
 * abandoning an armed-but-not-yet-taken request.
 */
int hpi_dfu_recovery_disarm(void);

/**
 * @brief Is the flag currently armed?
 */
bool hpi_dfu_recovery_armed(void);

/**
 * @brief Reboot now.
 *
 * Split from arming so callers can flush a reply first. Never returns on
 * success.
 */
void hpi_dfu_reboot(void);

#ifdef __cplusplus
}
#endif

#endif /* HPI_DFU_SERVICE_H */
