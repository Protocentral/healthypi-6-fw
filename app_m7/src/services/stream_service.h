/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Stream service (L4) -- representative service API.
 *
 * Subscribes to the sample bus and writes encoded frames to a pluggable
 * sink (USB CDC 0 today; ESP32/TCP or BLE later) using the .HP6 frame
 * framing (one parser for file + stream). This is the single
 * implementation of "streaming"; control surfaces are thin adapters:
 *   - control/mcumgr_hpi  -> hpi/stream_start|stop|status on CDC 1 (CLI/Studio)
 *   - control/shell_hpi   -> `hpi stream ...` (dev/factory)
 * Both call the functions below; neither reimplements stream logic.
 */

#ifndef HPI_SERVICES_STREAM_SERVICE_H
#define HPI_SERVICES_STREAM_SERVICE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Channel/annotation masks for the group-64 streaming commands. */
#define HPI_STREAM_CH_ECG   0x01u
#define HPI_STREAM_CH_PPG   0x02u
#define HPI_STREAM_CH_RESP  0x04u
#define HPI_STREAM_CH_EEG   0x08u

/* Pluggable output sink. */
struct hpi_stream_sink {
    const char *name;
    bool (*write)(const void *buf, uint32_t len);  /* all-or-nothing */
    bool (*is_connected)(void);
};

struct hpi_stream_status {
    bool     active;
    uint8_t  ch_mask;
    uint8_t  ann_mask;
    uint32_t frames_sent;
    uint32_t frames_dropped;
};

int  hpi_stream_service_init(const struct hpi_stream_sink *sink);

/* Enable/disable. Returns -ENOTSUP if a requested channel needs an absent
 * module (e.g. EEG without the module seated/powered). Stop is idempotent
 * and takes effect within 50 ms. */
int  hpi_stream_enable(uint8_t ch_mask, uint8_t ann_mask);
void hpi_stream_disable(void);
void hpi_stream_get_status(struct hpi_stream_status *out);

#ifdef __cplusplus
}
#endif

#endif /* HPI_SERVICES_STREAM_SERVICE_H */
