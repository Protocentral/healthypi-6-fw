/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Filesystem bring-up with an FS-ready handshake.
 *
 * Touching the filesystem before it is mounted faults (BUS/USAGE), so
 * fs_mount runs early, posts a semaphore on success, and every consumer
 * (UI, recorder) waits on it. A mount failure degrades to a clean
 * "SD not ready" state instead of faulting.
 */

#ifndef HPI_PLATFORM_FS_MOUNT_H
#define HPI_PLATFORM_FS_MOUNT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Attempt to mount the SD FATFS at "/SD:". Posts the FS-ready semaphore on
 * success. Returns 0 on success, negative errno otherwise (non-fatal). */
int  platform_fs_mount_init(void);

/* Block up to timeout_ms for the filesystem to become ready.
 * Returns true if ready, false on timeout. */
bool platform_fs_wait_ready(uint32_t timeout_ms);

/* Latched ready state (non-blocking). */
bool platform_fs_is_ready(void);

/* Unmount the SD FATFS so an external owner (USB MSC Transfer Mode) can take
 * exclusive block-device access. After this, platform_fs_is_ready() is false.
 * Returns 0 on success (or if already unmounted). MUST be called before MSC is
 * armed -- device + host on the same FAT volume corrupts it. */
int platform_fs_unmount(void);

/* Re-mount the SD FATFS after Transfer Mode is disarmed. Returns 0 on success
 * and re-latches the ready state. */
int platform_fs_remount(void);

#ifdef __cplusplus
}
#endif

#endif /* HPI_PLATFORM_FS_MOUNT_H */
