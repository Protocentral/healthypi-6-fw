/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * USB MSC "Transfer Mode" -- bulk SD download over USB mass storage.
 *
 * The mass-storage function is NOT enumerated by default; it is armed on demand
 * (group 64 / Studio / display). Arming unmounts the device FS and re-enumerates
 * with an MSC LUN exposing the SD card to the host; disarming removes the LUN
 * and remounts the FS. The device and host never share the FAT volume.
 */

#ifndef HPI_TRANSPORT_TRANSFER_MODE_H
#define HPI_TRANSPORT_TRANSFER_MODE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Arm Transfer Mode: pause recording (when present), unmount the FS, enumerate
 * the MSC LUN. Returns 0 on success; on failure the FS is left mounted. */
int  hpi_transfer_arm(void);

/* Disarm: remove the MSC LUN, remount the FS, resume recording. */
int  hpi_transfer_disarm(void);

/* Convenience: set armed state from a bool. */
int  hpi_transfer_set(bool on);

/* Current armed state. */
bool hpi_transfer_armed(void);

#ifdef __cplusplus
}
#endif

#endif /* HPI_TRANSPORT_TRANSFER_MODE_H */
