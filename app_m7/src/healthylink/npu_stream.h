/*
 * Copyright (c) 2026 ProtoCentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * STREAM_PUSH live ECG (lead II) to Compute and RESULT_POLL onto HPI_CH_INFER.
 * Call only from healthylink/, after handshake UP, never from LVGL or MCUmgr.
 */

#ifndef HPI_HEALTHYLINK_NPU_STREAM_H
#define HPI_HEALTHYLINK_NPU_STREAM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void npu_stream_on_link_up(void);
void npu_stream_cancel(void);

/** Queue one QRS for STREAM_EVENT. Safe from the IPC receive callback
 *  (non-blocking). @p t_ms is M7 uptime, same clock as STREAM_PUSH. */
void npu_stream_on_beat(uint32_t t_ms);

#ifdef __cplusplus
}
#endif

#endif /* HPI_HEALTHYLINK_NPU_STREAM_H */
