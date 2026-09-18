/*
 * Copyright (c) 2026 ProtoCentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * STREAM_PUSH live ECG (lead II) to Compute and RESULT_POLL onto HPI_CH_INFER.
 * Call only from healthylink/, after handshake UP, never from LVGL or MCUmgr.
 */

#ifndef HPI_HEALTHYLINK_NPU_STREAM_H
#define HPI_HEALTHYLINK_NPU_STREAM_H

#ifdef __cplusplus
extern "C" {
#endif

void npu_stream_on_link_up(void);
void npu_stream_cancel(void);

#ifdef __cplusplus
}
#endif

#endif /* HPI_HEALTHYLINK_NPU_STREAM_H */
