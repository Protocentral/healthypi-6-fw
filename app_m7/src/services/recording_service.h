/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Recording service (L4) -- writes a .HP6 file to the SD card from the sample
 * bus. Per RECORDING_MODULE_SRS.md §4: a 256-byte HPI6 v0x0300 header followed
 * by [type u8][len u16][ts_ms u32][payload] frames carrying the canonical
 * sample_formats.h payloads (same structs as the live stream).
 *
 * Writes the 256 B header + frame stream with in-band sync markers and the
 * .IDX/.TXT sidecars; start/stop/status; interlocked with USB Transfer Mode.
 */

#ifndef HPI_SERVICES_RECORDING_SERVICE_H
#define HPI_SERVICES_RECORDING_SERVICE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct hpi_recording_status {
    bool     active;
    uint32_t bytes_written;
    uint32_t duration_ms;
    uint32_t ecg_samples;
    uint32_t ppg_samples;
    uint32_t vitals_samples;
    char     path[64];
};

/* Start the writer thread + bus subscription. Call once at boot. */
int  hpi_recording_service_init(void);

/* Begin a recording. Creates /SD:/HPI6/REC/<date>/<time>.HP6 (or UNDATED/UPT_n
 * if the RTC is unset), writes the header, and starts capturing ECG/PPG/VITALS.
 * Returns -EBUSY if already recording or Transfer Mode is armed, -ENODEV if the
 * SD FS is not mounted. session_name may be NULL. */
int  hpi_recording_start(const char *session_name);

/* Stop + finalize (rewrite header with end time/duration/counters) and close. */
int  hpi_recording_stop(void);

bool hpi_recording_active(void);
void hpi_recording_get_status(struct hpi_recording_status *out);

/* Mark this instant. Publishes an HP6_EVENT_USER_MARK on HPI_CH_EVENT, so the
 * marker lands in the recording, the live stream and the ESP32 link together,
 * ordered against the samples it sits between (see struct hp6_event).
 * Implemented here, not in the UI: the UI is a pure consumer, and every way of
 * marking (on-screen button, hardware button) must produce the same record.
 *
 * Returns the 1-based sequence number of the mark, or -EACCES when nothing is
 * recording (the caller should not report success). Safe from any thread. */
int  hpi_recording_mark(void);

#ifdef __cplusplus
}
#endif

#endif /* HPI_SERVICES_RECORDING_SERVICE_H */
