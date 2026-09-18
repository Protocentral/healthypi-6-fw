/*
 * Copyright (c) 2026 ProtoCentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Canned TENSOR_LOAD / RUN / READ_RESULT on npu_wq. Call only from
 * healthylink/, after handshake UP, never from LVGL or MCUmgr.
 */

#ifndef HPI_HEALTHYLINK_NPU_INFER_H
#define HPI_HEALTHYLINK_NPU_INFER_H

#ifdef __cplusplus
extern "C" {
#endif

void npu_infer_on_link_up(void);
void npu_infer_cancel(void);

#ifdef __cplusplus
}
#endif

#endif /* HPI_HEALTHYLINK_NPU_INFER_H */
