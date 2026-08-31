/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * M4 IPC feed (platform). Opens the OpenAMP endpoint to the M4, receives the
 * M4's vitals messages (HR/SpO2/HRV) and republishes them as HPI_CH_VITALS
 * frames on the sample bus. The M4 algorithms are unchanged.
 */

#ifndef HPI_PLATFORM_IPC_H
#define HPI_PLATFORM_IPC_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct hp6_vitals;   /* core/sample_formats.h */

/* Start the M4 IPC link. Non-blocking: binding (which waits for the M4's ~7 s
 * boot delay) runs on an internal thread, so bring-up is not stalled if the
 * M4 is absent. Returns 0 if the init thread was started. */
int hpi_ipc_init(void);

/* True once the OpenAMP endpoint to the M4 is bound (vitals can flow). */
bool hpi_ipc_ready(void);

/* Firmware version the M4 reported when the IPC endpoint bound, or "" if it has
 * not bound yet or is running a build that predates the version message.
 * Surfaced as m4fw in group-64 device_info / fw_versions -- which matters now
 * that the M4 image is host-updatable (the M4 update path). Never NULL. */
const char *hpi_ipc_m4_version(void);

/* Copy out the most recently published vitals sample (zeroed if none yet).
 *
 * For the control adapters, which answer a host on demand rather than
 * subscribing to the bus. `flags` is what makes this worth exposing: it says
 * which sensor `hr_bpm` came from (HP6_VIT_* in core/sample_formats.h), and a
 * heart rate without that is ambiguous. */
void hpi_ipc_last_vitals(struct hp6_vitals *out);

#ifdef __cplusplus
}
#endif

#endif /* HPI_PLATFORM_IPC_H */
