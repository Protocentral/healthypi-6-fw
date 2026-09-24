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
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct hpi_recording_status {
    bool     active;
	bool     error;        /* true once an unrecoverable error stopped recording */
    int      error_code;   /* -errno reason; valid only when error == true */
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
/* Pause/resume recording: file remains open; paused time is excluded from
 * duration_ms. Returns 0 on success, -EACCES if not recording. */
int  hpi_recording_pause(bool pause);

/* Unrecoverable-error reporting (SRS §3.1): an I/O failure or card eject
 * during an active recording moves to ERROR and stops it; nothing
 * auto-restarts. These mirror hpi_recording_status's error/error_code
 * fields for callers that don't otherwise poll status. clear_error() lets
 * the UI dismiss the condition; a successful hpi_recording_start() also
 * clears it implicitly. */
bool hpi_recording_has_error(void);
int  hpi_recording_get_error(void);
void hpi_recording_clear_error(void);

//* New APIs: event and queries */
int  recording_add_event(uint16_t type);
/* Add a user event with a short text payload persisted to the .TXT sidecar. */
/* Returns the 1-based sequence number on success, or negative errno on failure. */
int  recording_add_event_text(const char *text);
int  recording_get_free_space(uint64_t *free_bytes, uint64_t *total_bytes);

/* One recording, as reported by recording_list_async(). duration_ms/channels
 * are read from the .HP6 header; size_bytes from fs_stat; year == 0 if the
 * file predates an RTC or was recorded UNDATED. */
struct recording_summary {
	char     path[64];      /* full path to the .HP6 file */
	uint32_t size_bytes;
	uint32_t duration_ms;    /* 0 if unknown / recording was never finalized */
	uint32_t event_count;    /* from the .IDX header */
	uint32_t channels;       /* bitmask, HPI_CH_BIT(id); 0 if unknown */
	uint16_t year;           /* 0 = undated (RTC was unset when recorded) */
	uint8_t  month, day, hour, min, sec;
};
/* Minimum free space required to start a recording. Below this, starting is
 * refused rather than letting a recording run out of room mid-session and
 * leave a truncated/unfinalized .HP6 file. */
#define REC_MIN_FREE_BYTES  (10ULL * 1024 * 1024)   /* 10 MB */

/* Called once per recording, newest-first, then ONE more time with
 * entry == NULL to signal the listing is complete. Runs on the system
 * workqueue thread -- never touch LVGL from inside cb(). */
typedef void (*recording_list_cb_t)(const struct recording_summary *entry,
				     void *user_data);
int  recording_list_async(recording_list_cb_t cb, void *user_data);
int recording_index_last_error(void);
/* ---- paginated listing (index build is I/O-free; page fetch reads only
 * the requested slice, cached). Preferred over recording_list_async() for
 * the Browse screen once the recording count grows. ---- */
struct recording_index_entry {
    char     path[64];
    uint16_t year;
    uint8_t  month, day, hour, min, sec;
};

typedef void (*recording_index_cb_t)(const struct recording_index_entry *entries,
                                     size_t n_entries, void *user_data);
typedef void (*recording_page_cb_t)(const struct recording_summary *entry, void *user_data);

#define RECORDING_PAGE_SIZE_MAX  20

int recording_index_build_async(recording_index_cb_t cb, void *user_data);
int recording_page_fetch_async(uint32_t start_idx, uint32_t count,
                               recording_page_cb_t cb, void *user_data);

int  recording_delete(const char *hp6_path);

int hpi_recording_set_channel_enabled(uint8_t channel, bool enabled);
uint32_t hpi_recording_get_channel_mask(void);

#ifdef __cplusplus
}
#endif

#endif /* HPI_SERVICES_RECORDING_SERVICE_H */
