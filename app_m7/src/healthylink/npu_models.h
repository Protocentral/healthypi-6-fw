/*
 * Copyright (c) 2026 ProtoCentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * MODEL_LIST / MODEL_ACTIVATE over SPI. Call only from healthylink/ on
 * npu_wq, after handshake STATUS, before the stream producer starts.
 */

#ifndef HPI_HEALTHYLINK_NPU_MODELS_H
#define HPI_HEALTHYLINK_NPU_MODELS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * List models, activate a default if none is active (prefer
 * beat_classifier), and wait for STATUS.active_name.
 *
 * On success copies the active name into @p active_out (NUL-terminated).
 * A list/activate failure is logged and returned; the caller still
 * publishes link UP -- the 2026-09-11 module already had a default.
 */
int npu_models_sync(char *active_out, size_t active_len);

#ifdef __cplusplus
}
#endif

#endif /* HPI_HEALTHYLINK_NPU_MODELS_H */
