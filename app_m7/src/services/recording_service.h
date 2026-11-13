/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Recording service (L4) -- writes a .HP6 file to the SD card from the sample
 * bus: the 256-byte header, then DBLK blocks carrying the canonical
 * sample_formats.h payloads (the same bytes as the live stream), with in-band
 * sync markers and the .IDX/.TXT sidecars. docs/HP6_DATA_FORMAT.md is the
 * format; this service must match it.
 *
 * Start/stop/pause/status, channel selection, event marks with notes, and a
 * paginated listing + delete for the on-device browser. Interlocked with USB
 * Transfer Mode.
 */

#ifndef HPI_SERVICES_RECORDING_SERVICE_H
#define HPI_SERVICES_RECORDING_SERVICE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct hpi_recording_status {
    bool     active;
    bool     paused;
    bool     error;        /* true once an unrecoverable error stopped recording */
    int      error_code;   /* -errno reason; valid only when error == true */
    uint32_t bytes_written;
    uint32_t duration_ms;  /* recorded time, excluding pauses */
    uint32_t ecg_samples;
    uint32_t ppg_samples;
    uint32_t vitals_samples;
    uint32_t events;       /* EVENT blocks written: marks, pause and resume */
    char     path[64];
};

/* Announce the service. The writer thread and its work queue start on their
 * own before main(), so the UI can use this API from the moment it exists. */
int  hpi_recording_service_init(void);

/* Begin a recording. Creates /SD:/HPI6/REC/<date>/<time>.HP6 (or
 * UNDATED/UPnnnnnn.HP6 if the RTC is unset), writes the header, and records
 * the selected channels (hpi_recording_set_channel_enabled()) plus EEG and
 * INFER whenever an expansion module produces them.
 *
 * max_duration_s stops the recording on its own after that much recorded
 * (pause-excluded) time; 0 records until stopped. Any stop -- UI, host, power,
 * Transfer Mode -- cancels it.
 *
 * Returns -EBUSY if already recording, -ENODEV if the SD card is not mounted
 * (or the host owns it), -ENOSPC below REC_MIN_FREE_BYTES free. session_name
 * may be NULL. */
int  hpi_recording_start_ex(const char *session_name, uint32_t max_duration_s);

/* hpi_recording_start_ex() with no auto-stop. */
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
 * recording (the caller should not report success). Safe from any thread;
 * never waits on the card. */
int  hpi_recording_mark(void);

/* As hpi_recording_mark(), with a short note (up to 63 bytes) written to the
 * .TXT sidecar next to the event's ts_ms and sequence number. */
int  recording_add_event_text(const char *text);

/* Pause/resume: the file stays open, data channels are dropped while paused,
 * and events are still recorded. Each transition writes an
 * HP6_EVENT_SYSTEM_PAUSE / _RESUME event. duration_ms excludes paused time;
 * every other timestamp in the file stays real time. Returns 0, or -EACCES if
 * not recording. */
int  hpi_recording_pause(bool pause);

/* Unrecoverable-error reporting: an I/O failure, a full card or a card eject
 * during an active recording moves to ERROR and stops it; nothing
 * auto-restarts. These mirror hpi_recording_status's error/error_code fields
 * for callers that don't otherwise poll status. clear_error() lets the UI
 * dismiss the condition; a successful start also clears it. */
bool hpi_recording_has_error(void);
int  hpi_recording_get_error(void);
void hpi_recording_clear_error(void);

/* Select which of HPI_CH_ECG (ECG + respiration), HPI_CH_PPG and
 * HPI_CH_VITALS the next recording captures. Returns -EINVAL for any other
 * channel, -EBUSY while recording. */
int      hpi_recording_set_channel_enabled(uint8_t channel, bool enabled);
uint32_t hpi_recording_get_channel_mask(void);

int  recording_get_free_space(uint64_t *free_bytes, uint64_t *total_bytes);

/* Minimum free space required to start a recording. Below this, starting is
 * refused rather than letting a recording run out of room mid-session. */
#define REC_MIN_FREE_BYTES  (10ULL * 1024 * 1024)   /* 10 MB */

/* ---- listing and delete, for the on-device browser ----
 *
 * Every callback below runs on the service's own work queue thread -- never
 * touch LVGL from one; stage the result and pick it up on the UI thread. */

/* One recording. duration_ms, channels and event_count come from the .HP6
 * header; for a file that was never closed, duration_ms is recovered from the
 * .IDX and event_count is 0. size_bytes is from fs_stat; year == 0 if the file
 * was recorded UNDATED. */
struct recording_summary {
    char     path[64];      /* full path to the .HP6 file */
    uint32_t size_bytes;
    uint32_t duration_ms;   /* 0 if unknown */
    uint32_t event_count;
    uint32_t channels;      /* bitmask, HPI_CH_BIT(id); 0 if unknown */
    uint16_t year;          /* 0 = undated (RTC was unset when recorded) */
    uint8_t  month, day, hour, min, sec;
};

struct recording_index_entry {
    char     path[64];
    uint16_t year;
    uint8_t  month, day, hour, min, sec;
};

typedef void (*recording_index_cb_t)(const struct recording_index_entry *entries,
                                     size_t n_entries, void *user_data);
/* Called once per entry, then once more with entry == NULL when the page is
 * complete. */
typedef void (*recording_page_cb_t)(const struct recording_summary *entry, void *user_data);
typedef void (*recording_delete_cb_t)(int rc, void *user_data);

#define RECORDING_PAGE_SIZE_MAX  20

/* Walk the card and build the newest-first index (no per-file reads). */
int recording_index_build_async(recording_index_cb_t cb, void *user_data);
/* The opendir() result of the last index build: 0, or -errno. */
int recording_index_last_error(void);
/* Summaries for [start_idx, start_idx + count) of the last index; cached. */
int recording_page_fetch_async(uint32_t start_idx, uint32_t count,
                               recording_page_cb_t cb, void *user_data);
/* Delete a recording and its .IDX/.TXT. cb gets 0, -EBUSY if it is the file
 * being recorded, or the fs_unlink() error. */
int recording_delete_async(const char *hp6_path, recording_delete_cb_t cb, void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* HPI_SERVICES_RECORDING_SERVICE_H */
