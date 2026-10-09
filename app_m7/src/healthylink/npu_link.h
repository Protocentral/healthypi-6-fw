/*
 * Copyright (c) 2026 ProtoCentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Internal SPI helper for HealthyLink Compute. Includable only from
 * healthylink/. All callers run on npu_wq -- not the system workqueue,
 * not LVGL, not MCUmgr.
 *
 * mod_npu.h is the public handshake snapshot. This is the transaction API
 * the data-plane producers (npu_infer.c, npu_stream.c) will share.
 */

#ifndef HPI_HEALTHYLINK_NPU_LINK_H
#define HPI_HEALTHYLINK_NPU_LINK_H

#include "hlink_proto.h"

#include <zephyr/kernel.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum npu_wait {
	NPU_WAIT_ACK   = 0, /**< STREAM_PUSH / TENSOR_LOAD ACK: HPI_NPU_ACK_WAIT_MS */
	NPU_WAIT_REPLY = 1, /**< handshake, RUN, READ_RESULT: IRQ edge or 50 ms */
};

/**
 * One command, one reply, on the NPU SPI4 link.
 *
 * Must run on npu_wq. reply->payload points into the driver's RX buffer
 * and is valid until the next npu_cmd().
 *
 * @return 0, -ECANCELED if npu_stop() raced us, or a negative errno.
 */
int npu_cmd(uint8_t cmd, const void *payload, uint16_t len,
	    struct hlink_frame *reply, enum npu_wait wait);

bool npu_link_stale(void);
int npu_link_submit(struct k_work *work);
void npu_link_cancel(struct k_work *work);

#ifdef __cplusplus
}
#endif

#endif /* HPI_HEALTHYLINK_NPU_LINK_H */
