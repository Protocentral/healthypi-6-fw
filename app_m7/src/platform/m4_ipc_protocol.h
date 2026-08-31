/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * M7<->M4 IPC transport framing (envelope + message ids + endpoint name).
 *
 * The PAYLOAD structs are the shared contract in `hpi_common_types.h` -- the
 * same header the M4 consumes. This file only adds the envelope/enum/ept-name,
 * mirrored from the M4's ipc_module.h; keep the two in sync.
 */

#ifndef HPI_PLATFORM_M4_IPC_PROTOCOL_H
#define HPI_PLATFORM_M4_IPC_PROTOCOL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* OpenAMP endpoint name -- must match the M4 side exactly. */
#define HPI_IPC_EPT_NAME "hpi_ipc"

/* Message envelope: type + length-prefixed payload. */
struct hpi_ipc_msg {
    uint8_t  type;
    uint8_t  reserved;
    uint16_t length;
    uint8_t  data[];
} __packed;

/* Message ids (subset; values fixed by the M4 contract). */
enum hpi_ipc_msg_type {
    HPI_IPC_MSG_TYPE_VERSION    = 0x05, /* M4->M7: firmware version */
    HPI_IPC_MSG_TYPE_PPG_RAW    = 0x10, /* M7->M4: raw PPG batch  */
    HPI_IPC_MSG_TYPE_PPG_VITALS = 0x11, /* M4->M7: SpO2 + HR       */
    HPI_IPC_MSG_TYPE_ECG_RAW    = 0x20, /* M7->M4: raw ECG batch  */
    HPI_IPC_MSG_TYPE_ECG_VITALS = 0x21, /* M4->M7: HR + HRV + QRS  */
};

#ifdef __cplusplus
}
#endif


/* M4 firmware version report (M4 -> M7), sent once when the endpoint binds;
 * surfaces as m4fw in group-64 device_info / fw_versions. Fixed 32 B,
 * NUL-terminated so a shorter string does not leak stale bytes across the IPC
 * boundary. */
#define HPI_IPC_VERSION_STR_MAX 32

struct hpi_ipc_version {
    char version[HPI_IPC_VERSION_STR_MAX];
} __packed;

#endif /* HPI_PLATFORM_M4_IPC_PROTOCOL_H */
