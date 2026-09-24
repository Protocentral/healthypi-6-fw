/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Recording service (L4) -- .HP6 writer. See recording_service.h + the SRS.
 */

#include "recording_service.h"
#include "hp6_frame.h"
#include "stream_service.h"
#include "core/sample_bus.h"
#include "core/channel_registry.h"
#include "core/sample_formats.h"
#include "platform/fs_mount.h"

#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/timeutil.h>
#include <zephyr/logging/log.h>
#include <app_version.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <strings.h>
#include <stdlib.h>   /* qsort */

LOG_MODULE_REGISTER(hpi_rec, CONFIG_HPI_APP_LOG_LEVEL);

#define REC_ROOT       "/SD:/HPI6/REC"
#define REC_WRITE_BUF  4096
#define REC_LIST_MAX_ENTRIES  64   /* must match scr_recording.c's REC_LIST_MAX */
#define REC_INDEX_MAX_ENTRIES  256
#define REC_PAGE_SIZE_MAX      RECORDING_PAGE_SIZE_MAX
#define REC_PAGE_CACHE_SLOTS   3
#define TXT_HDR_SIZE  512   /* fixed, so the final rewrite never shifts event lines */

/* `board_variant` in the .HP6 header -- how a host tells which hardware
 * produced a file. Add a case when a board is added; the #error makes a
 * missing one a build failure rather than a silent wrong label. */
#if defined(CONFIG_BOARD_HEALTHYPI6_V5)
#define HPI_BOARD_VARIANT "v5"
#elif defined(CONFIG_BOARD_HEALTHYPI6_V4)
#define HPI_BOARD_VARIANT "v4"
#elif defined(CONFIG_BOARD_HEALTHYPI6_V3)
#define HPI_BOARD_VARIANT "v3"
#elif defined(CONFIG_BOARD_HEALTHYPI6_V2)
#define HPI_BOARD_VARIANT "v2"
#else
#error "Unknown board: add its board_variant string for the .HP6 header"
#endif
#define REC_RING       32
#define REC_ECG_SCRATCH  640   /* >= 16 samples x sizeof(struct hp6_ecg_sample) */
#define REC_SYNC_PERIOD_MS 5000u
#define REC_ROLLOVER_MS    (24ULL * 3600ULL * 1000ULL)   /* SRS §3.1/§5 */
#define HP6_SYNC_MAGIC 0xDEADBEEFu
/* v0x0300: magic4 + version2 + event_count2 + sync_count4 = 12 B.
 * v0x0301 (this change) appends duration_ms4 + channels4 = 20 B, so
 * page_work_fn() can list without ever opening the .HP6 (SRS §3.4/§4.2).
 * Readers MUST gate on the version field, not on read length -- an old
 * 12-byte-header file still fills a 20-byte read buffer, because its event
 * table starts immediately at the old offset 12. */
#define HP6_IDX_HDR_SIZE        20
#define HP6_IDX_EVENT_REC_SIZE  69U   /* ts_ms(4) + type(1) + desc[64] */
#define HP6_IDX_SYNC_REC_SIZE   20U   /* ts_ms(4) + file_off(8) + seq(4) + crc(4) */
#define HP6_IDX_MAX_EVENTS      64    /* reserved slots -- see idx_append_event() */
#define HP6_IDX_EVENTS_OFFSET   HP6_IDX_HDR_SIZE
#define HP6_IDX_SYNCS_OFFSET    (HP6_IDX_EVENTS_OFFSET + \
                                  (HP6_IDX_MAX_EVENTS * HP6_IDX_EVENT_REC_SIZE))
BUILD_ASSERT(HP6_IDX_EVENT_REC_SIZE == 4 + 1 + 64, "idx event record must be 69 B");

/* .IDX event category (SRS §3.3: USER/AUTO/ARTIFACT/SYSTEM). This is the
 * on-disk classification byte only -- independent of hp6_event.type, which
 * today only ever carries HP6_EVENT_USER_MARK on the wire. */
enum hp6_idx_event_type {
    HP6_IDX_EVT_USER     = 0,
    HP6_IDX_EVT_AUTO     = 1,
    HP6_IDX_EVT_ARTIFACT = 2,
    HP6_IDX_EVT_SYSTEM   = 3,
};
/* In-band sync marker payload (SRS §4.1) -- written as an HPI_CH_SYNC frame
 * every 5 s; the .IDX mirrors it. Enables crash recovery + fast listing. */
struct hp6_sync_payload {
    uint32_t magic;        /* HP6_SYNC_MAGIC */
    uint32_t seq;
    uint64_t wall_ms;
    uint32_t ecg_count, ppg_count, eeg_count, vitals_count;
    uint32_t events_since_last_sync;
    uint32_t running_crc32;   /* CRC-32 of frame bytes since the previous sync */
} __packed;
BUILD_ASSERT(sizeof(struct hp6_sync_payload) == 40, "sync payload must be 40 B");

/* Number of per-channel slots in the header's rate/count arrays. Indexed by
 * (channel id - 1), so slot 0 is HPI_CH_ECG. Sized past the channels that exist
 * so the next one does not cost a format version. */
#define HP6_HDR_CHANNEL_SLOTS  8

/* 256-byte .HP6 v0x0300 header.
 *
 * 0x0300 replaced 0x0200's five NAMED rate/sample fields (ecg_rate, ppg_rate,
 * ... eeg_samples) with arrays indexed by channel id. The named form was full
 * at five entries and every new channel needed both a format break and a
 * hand-edit in three codebases; the array form grows to HP6_HDR_CHANNEL_SLOTS
 * without one. Done together with widening the HRV fields in hp6_vitals and
 * adding HPI_CH_INFER, so the format broke once rather than three times --
 * affordable only because nothing had shipped yet. */
struct recording_header {
    char     magic[4];          /* "HPI6" */
    uint16_t version;           /* 0x0300 */
    uint16_t header_size;       /* 256 */
    uint64_t timestamp_start;   /* unix ms, 0 if RTC unset */
    uint64_t timestamp_end;     /* 0xFFFF.. while open */
    uint32_t duration_ms;
    char     patient_id[32];
    char     session_name[64];
    uint32_t channels;          /* bit = HPI_CH_BIT(channel id) */
    uint16_t rate_hz[HP6_HDR_CHANNEL_SLOTS];      /* 0 = event-rate/absent */
    uint32_t sample_count[HP6_HDR_CHANNEL_SLOTS];
    uint32_t event_count;
    uint64_t events_offset;
    char     firmware_version[16];
    char     board_variant[8];
    char     serial_number[16];
    uint8_t  reserved[20];
    uint32_t header_crc32;      /* over [0..248) */
    char     header_magic_end[4]; /* "HP6E" */
} __packed;

/* Slot for a channel id, or -1 if it has no per-channel summary. */
#define HP6_HDR_SLOT(ch)  ((int)(ch) - 1)

BUILD_ASSERT(sizeof(struct recording_header) == 256, "HP6 header must be 256 B");

/* Dedicated workqueue for recording_list_async(). Listing does synchronous
 * SD I/O (fs_open/fs_read/fs_close per entry) that can run long with many
 * recordings on the card. Running that on the SYSTEM workqueue was starving
 * the input subsystem's own work items on the same queue, so a Browse
 * Recordings listing could make touch/button input unresponsive for the
 * whole scan ("Event dropped, queue full, not blocking in syswq"). */
#define REC_WQ_STACK_SIZE  4096
#define REC_WQ_PRIORITY    K_PRIO_PREEMPT(10)

K_THREAD_STACK_DEFINE(s_rec_wq_stack, REC_WQ_STACK_SIZE);
static struct k_work_q s_rec_wq;

/* ---- state ---- */
static struct hpi_bus_sub *g_sub;
static volatile bool       g_active;
static volatile bool       g_paused;
static volatile bool       g_file_open;   /* true only while g_file has a valid, open handle */
static volatile bool       g_error;       /* true once an unrecoverable error has stopped recording */
static volatile int        g_error_code;  /* -errno reason; valid only when g_error == true */
static uint64_t            g_pause_start_ms;
static uint64_t            g_paused_accum_ms;
static struct fs_file_t    g_file;
static struct recording_header g_hdr;
static uint8_t  g_wbuf[REC_WRITE_BUF];
static uint32_t g_wlen;
static uint32_t g_bytes;
static uint64_t g_start_ms;          /* device-monotonic ms at start */
static char     g_path[64];
static struct k_mutex g_lock;
static uint32_t g_undated_seq;       /* fallback filename counter */
static atomic_t g_mark_seq = ATOMIC_INIT(0);
static char     g_session_base_path[64];  /* g_path minus ".HP6", for _NNN naming */
static char     g_session_name[64];       /* reused across rollover continuations */
static uint32_t g_rollover_seq;           /* 0 = original file; increments per continuation */
/* Channels the recorder should persist (bitset of HPI_CH_BIT(id)).
 * Default: record ECG, PPG, RESP and VITALS to match the UI toggles. */
static uint32_t g_channel_mask = (HPI_CH_BIT(HPI_CH_ECG) | HPI_CH_BIT(HPI_CH_PPG) |
                                  HPI_CH_BIT(HPI_CH_RESP) | HPI_CH_BIT(HPI_CH_VITALS));
struct list_work_ctx {
	struct k_work work;
	recording_list_cb_t cb;
	void *user_data;
	struct recording_summary *entries;
	size_t n_entries;
	size_t max_entries;
};

/* 5b: sidecars + sync markers */
static struct fs_file_t g_idx;       /* .IDX sync/event TOC */
static bool     g_idx_open;
static char     g_idx_path[64];
static uint32_t g_sync_seq;
static uint32_t g_sync_count;
static int64_t  g_last_sync_ms;      /* device-monotonic ms of last sync */
static uint32_t g_running_crc;       /* CRC of frame bytes since last sync */
static uint32_t g_seq;               /* DBLK sequence, per file (gap = loss) */
static uint64_t g_wall_start_ms;     /* unix ms at start (0 if RTC unset) */
static uint32_t g_idx_event_count;      /* events written into the reserved table */
static uint64_t g_idx_sync_write_off;   /* explicit tail; event writes seek elsewhere */
static bool     g_idx_events_full_warned;
/* ---- helpers ---- */

static void hex16(char out[17], const uint8_t *id, size_t n)
{
    static const char hx[] = "0123456789ABCDEF";
    size_t have = n < 8 ? n : 8;
    size_t start = n >= 8 ? n - 8 : 0;
    for (size_t i = 0; i < have; i++) {
        out[i * 2] = hx[(id[start + i] >> 4) & 0xF];
        out[i * 2 + 1] = hx[id[start + i] & 0xF];
    }
    out[have * 2] = '\0';
}

/* Create one dir only if it doesn't already exist (avoids noisy -EEXIST errors
 * the FS driver logs at err level on every call). */
static void mkdir_one(const char *path)
{
    struct fs_dirent ent;
    if (fs_stat(path, &ent) == 0) {
        return;   /* already exists */
    }
    int rc = fs_mkdir(path);
    if (rc != 0 && rc != -EEXIST) {
        LOG_WRN("mkdir %s failed (%d)", path, rc);
    }
}

/* Best-effort mkdir -p, starting BELOW the "/SD:" mount point (you can't mkdir
 * the volume root -- that produced the -ENOENT noise in the log). */
static void mkdir_p(const char *dir)
{
    /* Allow reasonably long paths under the mount point. */
    char tmp[128];
    size_t n = strlen(dir);
    if (n >= sizeof(tmp)) {
        LOG_WRN("mkdir_p: path too long (%zu)", n);
        return;
    }
    strncpy(tmp, dir, sizeof(tmp));
    tmp[sizeof(tmp) - 1] = '\0';

    /* Skip the mount point: advance past the '/' that follows "/SD:". */
    char *start = strchr(tmp + 1, '/');
    if (start == NULL) {
        return;   /* nothing below the volume to create */
    }

    /* Create each directory component below the mount point. */
    for (char *p = start + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir_one(tmp);
            *p = '/';
        }
    }
    mkdir_one(tmp);
}

/* Diagnostic helper: log stat results for the path, its parent, and the SD mount. */
static void log_path_status(const char *path)
{
    struct fs_dirent ent;
    int rc;

    rc = fs_stat(path, &ent);
    LOG_WRN("diag: fs_stat %s -> %d", path, rc);

    char tmp[128];
    strncpy(tmp, path, sizeof(tmp));
    tmp[sizeof(tmp) - 1] = '\0';
    char *slash = strrchr(tmp, '/');
    if (slash != NULL && slash != tmp) {
        *slash = '\0';
        rc = fs_stat(tmp, &ent);
        LOG_WRN("diag: fs_stat parent %s -> %d", tmp, rc);
    }

    struct fs_statvfs sv;
    rc = fs_statvfs("/SD:", &sv);
    LOG_WRN("diag: fs_statvfs /SD: -> %d", rc);
}

/* Build the dated path + create dirs. Uses the RTC if available.
 * Returns 0 on success, or -EAGAIN if 26 dated recordings already exist for
 * this exact second (SRS §5: same-second collisions get an HHMMSS_A..Z.HP6
 * suffix; running out means refuse rather than silently overwrite). */
static int build_path(void)
{
    const struct device *rtc = DEVICE_DT_GET_OR_NULL(DT_ALIAS(rtc));
    struct rtc_time t;
    char daydir[40];
    char file[64];

    if (rtc && device_is_ready(rtc) && rtc_get_time(rtc, &t) == 0 &&
        t.tm_year > 0) {
        snprintf(daydir, sizeof(daydir), REC_ROOT "/%04d%02d%02d",
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
        mkdir_p(daydir);

        /* Try the bare HHMMSS.HP6 name first; only add a suffix if that
         * exact second already has a recording (e.g. a quick stop/start).
         * Without this check, fs_open(FS_O_CREATE) on the second attempt
         * would silently truncate and overwrite the first file. */
        struct fs_dirent probe;
        snprintf(file, sizeof(file), "%s/%02d%02d%02d.HP6",
                 daydir, t.tm_hour, t.tm_min, t.tm_sec);
        if (fs_stat(file, &probe) == 0) {
            bool found = false;
            for (char suffix = 'A'; suffix <= 'Z'; suffix++) {
                snprintf(file, sizeof(file), "%s/%02d%02d%02d_%c.HP6",
                         daydir, t.tm_hour, t.tm_min, t.tm_sec, suffix);
                if (fs_stat(file, &probe) != 0) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                LOG_ERR("build_path: 26 collisions for %02d%02d%02d, refusing",
                        t.tm_hour, t.tm_min, t.tm_sec);
                return -EAGAIN;
            }
        }
    } else {
        snprintf(daydir, sizeof(daydir), REC_ROOT "/UNDATED");
        mkdir_p(daydir);
        /* g_undated_seq resets to 0 on every boot (plain RAM, not
         * persisted), so trusting it alone produces UP000001.HP6 after
         * every reboot -- fs_open() then reuses and silently overwrites
         * whatever was already at that name. Probe the card and advance
         * past any name that already exists. */
        struct fs_dirent probe;
        do {
            snprintf(file, sizeof(file), "%s/UP%06u.HP6", daydir,
                     (unsigned)(++g_undated_seq));
        } while (fs_stat(file, &probe) == 0 && g_undated_seq < 999999U);
    }
    strncpy(g_path, file, sizeof(g_path) - 1);
    g_path[sizeof(g_path) - 1] = '\0';
    return 0;
}

static void header_init(const char *session_name)
{
    memset(&g_hdr, 0, sizeof(g_hdr));
    memcpy(g_hdr.magic, "HPI6", 4);
    g_hdr.version = sys_cpu_to_le16(0x0300);
    g_hdr.header_size = sys_cpu_to_le16(256);
    g_hdr.timestamp_start = sys_cpu_to_le64(g_wall_start_ms);  /* unix ms, 0 if RTC unset */
    g_hdr.timestamp_end = 0xFFFFFFFFFFFFFFFFULL;
    /* Host-endian while open; byte-swapped once in finalize, like sample_count.
     * Channels actually seen are OR'd in as frames arrive. */
    /* Record which channels are intended to be written. Keep header in host
     * endianness while open; finalize() will byte-swap. */
    g_hdr.channels = g_channel_mask;
    g_hdr.rate_hz[HP6_HDR_SLOT(HPI_CH_ECG)]    = sys_cpu_to_le16(HPI_ECG_RATE_HZ);
    g_hdr.rate_hz[HP6_HDR_SLOT(HPI_CH_PPG)]    = sys_cpu_to_le16(HPI_PPG_RATE_HZ);
    g_hdr.rate_hz[HP6_HDR_SLOT(HPI_CH_RESP)]   = sys_cpu_to_le16(HPI_ECG_RATE_HZ);
    g_hdr.rate_hz[HP6_HDR_SLOT(HPI_CH_VITALS)] = sys_cpu_to_le16(1);
    g_hdr.rate_hz[HP6_HDR_SLOT(HPI_CH_EEG)]    = sys_cpu_to_le16(HPI_EEG_RATE_HZ);
    /* HPI_CH_INFER is event-rate: its slot stays 0, like EVENT and SYNC. */
    if (session_name) {
        strncpy(g_hdr.session_name, session_name, sizeof(g_hdr.session_name) - 1);
    }
    strncpy(g_hdr.firmware_version, APP_VERSION_STRING,
            sizeof(g_hdr.firmware_version) - 1);
    strncpy(g_hdr.board_variant, HPI_BOARD_VARIANT, sizeof(g_hdr.board_variant) - 1);
    uint8_t uid[16] = {0};
    /* serial: low 8 bytes of UID, hex (matches device_info). */
    ssize_t n = hwinfo_get_device_id(uid, sizeof(uid));
    if (n > 0) {
        char sn[17];
        hex16(sn, uid, (size_t)n);
        /* memcpy, not strncpy: the serial is exactly 16 hex digits and the
         * field is char[16], so strncpy(..., sizeof - 1) wrote 15 and dropped
         * the last digit from every recording ever made. The field is not
         * required to be NUL-terminated -- readers take it as a fixed-width
         * char[16] -- so a full 16 is correct and in spec. */
        BUILD_ASSERT(sizeof(g_hdr.serial_number) == 16, "serial field is char[16]");
        memcpy(g_hdr.serial_number, sn, sizeof(g_hdr.serial_number));
    }
    g_hdr.header_crc32 = sys_cpu_to_le32(
        crc32_ieee((const uint8_t *)&g_hdr, 248));
    memcpy(g_hdr.header_magic_end, "HP6E", 4);
}

/* Move to an unrecoverable-error state: stop accepting frames and close
 * whatever is open, without touching header finalize math (the file may be
 * mid-block) -- SRS §3.1: unrecoverable errors -> ERROR, attempt close,
 * notify. Surfaced to callers via hpi_recording_get_status()/_has_error(),
 * matching the polling pattern the UI already uses for everything else in
 * this module. Caller must hold g_lock. */
static void enter_error_state(int reason)
{
    if (g_error) {
        return;   /* already in error -- keep the first reason */
    }
    g_error = true;
    g_error_code = reason;
    g_active = false;

    if (g_idx_open) {
        fs_sync(&g_idx);
        fs_close(&g_idx);
        g_idx_open = false;
    }
    if (g_file_open) {
        fs_sync(&g_file);
        fs_close(&g_file);
        g_file_open = false;
    }

    LOG_ERR("recording: entering ERROR state (%d) on %s", reason, g_path);
}

static int flush_buf(void)
{
    if (g_wlen == 0) {
        return 0;
    }
    ssize_t w = fs_write(&g_file, g_wbuf, g_wlen);
    if (w < 0) {
        LOG_ERR("fs_write failed (%d)", (int)w);
        enter_error_state((int)w);
        return (int)w;
    }
    g_bytes += g_wlen;
    g_wlen = 0;
    return 0;
}

/* Current unix-epoch ms from the RTC, or 0 if unset/unavailable. */
static uint64_t rtc_unix_ms(void)
{
    const struct device *rtc = DEVICE_DT_GET_OR_NULL(DT_ALIAS(rtc));
    struct rtc_time t;
    if (rtc && device_is_ready(rtc) && rtc_get_time(rtc, &t) == 0 &&
        t.tm_year > 0) {
        /* rtc_time is layout-compatible with struct tm for the date fields. */
        int64_t secs = timeutil_timegm64((struct tm *)&t);
        if (secs > 0) {
            return (uint64_t)secs * 1000ULL;
        }
    }
    return 0;
}

/* Append one [type u8][len u16][ts_ms u32][payload] frame to the write buffer,
 * accumulating the running CRC over the emitted bytes. */
/* Append one DBLK block -- the SAME frame the live stream emits (hp6_frame.h,
 * and docs/HP6_DATA_FORMAT.md ss3), built straight into the write buffer.
 *
 * This used to write an ad-hoc 7-byte record -- type(1), len(2), ts(4) -- and
 * nothing else. That container was internally consistent but it was not the
 * .HP6 format: no magic, no per-block CRC, no sequence number. The published
 * spec says a file is this header followed by DBLK blocks, so no third party
 * could read a recording, and neither could our own `hp6 verify`, `to-csv`,
 * `repair` or `events` -- the host toolchain only ever worked on live streams.
 * Found on hardware 2026-08-31: `hp6 verify` skipped all 389767 body bytes
 * resyncing and decoded zero blocks.
 *
 * What the old container gave up, and this restores: a reader can resynchronise
 * on the next magic and lose one block instead of the rest of the file, each
 * block carries its own CRC, and a gap in `seq` is detectable. That matters
 * most in exactly the case recordings exist for -- a card pulled mid-write --
 * and it is what makes `hp6 repair` possible at all. */
static void append_dblk(uint8_t channel, uint8_t flags, uint64_t t_ms,
                        uint16_t sample_count, const void *payload, uint16_t len)
{
    const uint32_t block_len = HP6_DBLK_OVERHEAD + len;

    if (block_len > sizeof(g_wbuf)) {
        return;   /* impossible with canonical batch sizes: 32 + 16*20 = 352 */
    }
    if (g_wlen + block_len > sizeof(g_wbuf)) {
        if (flush_buf() != 0) {
            return;
        }
    }

    uint8_t *b = &g_wbuf[g_wlen];

    b[HP6_DBLK_OFF_MAGIC + 0] = HP6_DBLK_MAGIC0;
    b[HP6_DBLK_OFF_MAGIC + 1] = HP6_DBLK_MAGIC1;
    b[HP6_DBLK_OFF_MAGIC + 2] = HP6_DBLK_MAGIC2;
    b[HP6_DBLK_OFF_MAGIC + 3] = HP6_DBLK_MAGIC3;
    sys_put_le32(block_len,   &b[HP6_DBLK_OFF_BLOCK_LEN]);
    sys_put_le32(g_seq++,     &b[HP6_DBLK_OFF_SEQ]);
    sys_put_le64(t_ms,        &b[HP6_DBLK_OFF_T_MS]);
    b[HP6_DBLK_OFF_CHANNEL] = channel;
    b[HP6_DBLK_OFF_FLAGS]   = flags;
    sys_put_le16(sample_count, &b[HP6_DBLK_OFF_SAMPLE_COUNT]);
    sys_put_le32(0,            &b[HP6_DBLK_OFF_RESERVED]);

    if (len > 0 && payload != NULL) {
        memcpy(&b[HP6_DBLK_OFF_PAYLOAD], payload, len);
    }
    sys_put_le32(crc32_ieee(b, HP6_DBLK_HDR_LEN + len),
                 &b[HP6_DBLK_HDR_LEN + len]);

    /* The sync marker's running CRC covers whole blocks now, which is what a
     * reader can actually re-compute from the file. */
    g_running_crc = crc32_ieee_update(g_running_crc, b, block_len);
    g_wlen += block_len;
}

static void append_frame(const struct hpi_sample_frame *f)
{
    /* DBLK t_ms is ms since device boot (docs/HP6_DATA_FORMAT.md ss3). */
    uint64_t t_ms = f->t_mono_us / 1000ULL;
    /* Captured BEFORE the append because it can flush mid-call. */
    uint64_t rec_off = g_bytes + g_wlen;

    const bool ecg_sel  = (g_channel_mask & HPI_CH_BIT(HPI_CH_ECG))  != 0;
    const bool resp_sel = (g_channel_mask & HPI_CH_BIT(HPI_CH_RESP)) != 0;
    const void *payload = f->payload;

    /* RESP (ADS1294R CH1) travels inside the ECG frame. Keep the wire layout,
     * but zero whichever half was not selected so the file only holds what
     * the user chose. */
    static uint8_t ecg_scratch[REC_ECG_SCRATCH] __aligned(4);

    if (f->channel == HPI_CH_ECG && (!ecg_sel || !resp_sel) &&
        f->len <= sizeof(ecg_scratch) &&
        (f->len % sizeof(struct hp6_ecg_sample)) == 0) {
        struct hp6_ecg_sample *s = (struct hp6_ecg_sample *)ecg_scratch;
        size_t n = f->len / sizeof(*s);

        memcpy(ecg_scratch, f->payload, f->len);
        for (size_t i = 0; i < n; i++) {
            if (!resp_sel) {
                s[i].resp = 0;
            }
            if (!ecg_sel) {
                s[i].lead_i = 0;
                s[i].lead_ii = 0;
                s[i].v1 = 0;
                s[i].lead_off = 0;
            }
        }
        payload = ecg_scratch;
    }

    append_dblk((uint8_t)f->channel, (uint8_t)f->flags, t_ms,
                f->sample_count, payload, f->len);

    switch (f->channel) {
    case HPI_CH_ECG:
        /* One frame feeds two logical channels; count each only if selected. */
        if (ecg_sel) {
            g_hdr.sample_count[HP6_HDR_SLOT(HPI_CH_ECG)] += f->sample_count;
            g_hdr.channels |= HPI_CH_BIT(HPI_CH_ECG);
        }
        if (resp_sel) {
            g_hdr.sample_count[HP6_HDR_SLOT(HPI_CH_RESP)] += f->sample_count;
            g_hdr.channels |= HPI_CH_BIT(HPI_CH_RESP);
        }
        break;
    case HPI_CH_PPG:
    case HPI_CH_RESP:      /* only if a separate RESP frame is ever published */
    case HPI_CH_VITALS:
    case HPI_CH_EEG:
    case HPI_CH_INFER:
        g_hdr.sample_count[HP6_HDR_SLOT(f->channel)] += f->sample_count;
        g_hdr.channels |= HPI_CH_BIT(f->channel);
        break;
    case HPI_CH_EVENT:
        if (g_hdr.event_count == 0) {
            g_hdr.events_offset = rec_off;
        }
        g_hdr.event_count += f->sample_count;
        break;
    default: break;
    }
}

/* ---- .IDX sidecar (sync/event TOC, SRS §4.2) ---- */

static void idx_open(void)
{
    g_idx_open = false;
    g_sync_count = 0;
    g_idx_event_count = 0;
    g_idx_events_full_warned = false;
    strncpy(g_idx_path, g_path, sizeof(g_idx_path) - 1);
    g_idx_path[sizeof(g_idx_path) - 1] = '\0';
    char *dot = strrchr(g_idx_path, '.');
    if (dot) {
        strncpy(dot, ".IDX", 5);   /* replace ".HP6" */
    }
    fs_file_t_init(&g_idx);
    if (fs_open(&g_idx, g_idx_path, FS_O_CREATE | FS_O_RDWR) != 0) {
        LOG_WRN("idx open failed: %s", g_idx_path);
        return;
    }
    uint8_t h[HP6_IDX_HDR_SIZE] = {0};
    memcpy(h, "HP6I", 4);
    /* IDX header format revision, independent of the .HP6 container version
     * (still 0x0300) -- 0x0301 adds duration_ms/channels below. */
    sys_put_le16(0x0301, &h[4]);
    sys_put_le16(0, &h[6]);        /* event_count -- patched as events land + at finalize */
    sys_put_le32(0, &h[8]);        /* sync_count -- patched at finalize */
    sys_put_le32(0, &h[12]);       /* duration_ms -- patched at finalize */
    sys_put_le32(0, &h[16]);       /* channels    -- patched at finalize */
    if (fs_write(&g_idx, h, sizeof(h)) != (ssize_t)sizeof(h)) {
        LOG_WRN("idx header write failed: %s", g_idx_path);
        fs_close(&g_idx);
        return;
    }

    /* Reserve the events table, zero-filled, so a later event write lands
     * in-place and never disturbs the syncs tail below it. */
    uint8_t zero[64] = {0};
    size_t remaining = (size_t)HP6_IDX_MAX_EVENTS * HP6_IDX_EVENT_REC_SIZE;
    while (remaining > 0) {
        size_t chunk = MIN(remaining, sizeof(zero));
        if (fs_write(&g_idx, zero, chunk) != (ssize_t)chunk) {
            LOG_WRN("idx event-table reserve failed: %s", g_idx_path);
            break;
        }
        remaining -= chunk;
    }

    g_idx_sync_write_off = HP6_IDX_SYNCS_OFFSET;
    g_idx_open = true;
}

static void idx_append_sync(uint32_t ts_ms, uint64_t file_off, uint32_t seq, uint32_t crc)
{
    if (!g_idx_open) {
        return;
    }
    uint8_t e[HP6_IDX_SYNC_REC_SIZE];
    sys_put_le32(ts_ms, &e[0]);
    sys_put_le64(file_off, &e[4]);
    sys_put_le32(seq, &e[12]);
    sys_put_le32(crc, &e[16]);

    /* Event writes seek elsewhere in the file between sync calls, so the
     * sync tail must be re-seeked explicitly rather than assumed current. */
    if (fs_seek(&g_idx, g_idx_sync_write_off, FS_SEEK_SET) != 0) {
        return;
    }
    if (fs_write(&g_idx, e, sizeof(e)) == (ssize_t)sizeof(e)) {
        g_idx_sync_write_off += sizeof(e);
    }
}

/* Write one event into its reserved .IDX slot (SRS §3.3: ts_ms u32, type u8,
 * desc[64]). Takes g_lock itself -- called from multiple contexts (UI
 * thread via hpi_recording_mark, recording_add_event_text/_event). */
static void idx_append_event(uint32_t ts_ms, enum hp6_idx_event_type type,
                             const char *desc)
{
    k_mutex_lock(&g_lock, K_FOREVER);
    if (!g_idx_open) {
        k_mutex_unlock(&g_lock);
        return;
    }
    if (g_idx_event_count >= HP6_IDX_MAX_EVENTS) {
        k_mutex_unlock(&g_lock);
        if (!g_idx_events_full_warned) {
            LOG_WRN("idx: event table full (%d) -- further events still land "
                    "in .HP6/.TXT, just omitted from .IDX", HP6_IDX_MAX_EVENTS);
            g_idx_events_full_warned = true;
        }
        return;
    }
    uint8_t rec[HP6_IDX_EVENT_REC_SIZE] = {0};
    sys_put_le32(ts_ms, &rec[0]);
    rec[4] = (uint8_t)type;
    if (desc && desc[0]) {
        strncpy((char *)&rec[5], desc, HP6_IDX_EVENT_REC_SIZE - 5 - 1);
    }

    uint64_t off = HP6_IDX_EVENTS_OFFSET +
                  (uint64_t)g_idx_event_count * HP6_IDX_EVENT_REC_SIZE;
    
        if (fs_seek(&g_idx, off, FS_SEEK_SET) == 0 &&
        fs_write(&g_idx, rec, sizeof(rec)) == (ssize_t)sizeof(rec)) {
        g_idx_event_count++;
        uint8_t c[2];
        sys_put_le16((uint16_t)g_idx_event_count, c);
        if (fs_seek(&g_idx, 6, FS_SEEK_SET) == 0) {
            (void)fs_write(&g_idx, c, sizeof(c));
        }
    }
    k_mutex_unlock(&g_lock);
}
static void idx_finalize(uint32_t duration_ms, uint32_t channels)
{
    if (!g_idx_open) {
        return;
    }
    /* Patch event_count at offset 6 and sync_count at offset 8, then append
     * a whole-file CRC footer. */
    if (fs_seek(&g_idx, 6, FS_SEEK_SET) == 0) {
        uint8_t c[2];
        sys_put_le16((uint16_t)MIN(g_idx_event_count, 0xFFFFu), c);
        (void)fs_write(&g_idx, c, sizeof(c));
    }
    if (fs_seek(&g_idx, 8, FS_SEEK_SET) == 0) {
        uint8_t c[4];
        sys_put_le32(g_sync_count, c);
        (void)fs_write(&g_idx, c, 4);
    }
    /* duration_ms/channels -- lets listing show both without ever opening
     * the .HP6 (SRS §3.4/§4.2, IDX header v0x0301). */
    if (fs_seek(&g_idx, 12, FS_SEEK_SET) == 0) {
        uint8_t c[4];
        sys_put_le32(duration_ms, c);
        (void)fs_write(&g_idx, c, 4);
    }
    if (fs_seek(&g_idx, 16, FS_SEEK_SET) == 0) {
        uint8_t c[4];
        sys_put_le32(channels, c);
        (void)fs_write(&g_idx, c, 4);
    }
        uint32_t footer_crc = 0;
    if (fs_seek(&g_idx, 0, FS_SEEK_SET) == 0) {
        uint8_t buf[128];
        ssize_t r;
        while ((r = fs_read(&g_idx, buf, sizeof(buf))) > 0) {
            footer_crc = crc32_ieee_update(footer_crc, buf, (size_t)r);
        }
    } else {
        LOG_WRN("idx: footer CRC seek failed, writing 0: %s", g_idx_path);
    }
    (void)fs_seek(&g_idx, 0, FS_SEEK_END);
    uint8_t foot[4];
    sys_put_le32(footer_crc, foot);
    (void)fs_write(&g_idx, foot, 4);
    fs_sync(&g_idx);
    fs_close(&g_idx);
    g_idx_open = false;
}

/* ---- .TXT sidecar (human-readable, SRS §4.3) ---- */

static void txt_write(bool final)
{
    char path[64];
    strncpy(path, g_path, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    char *dot = strrchr(path, '.');
    if (dot) {
        strncpy(dot, ".TXT", 5);
    }
    struct fs_file_t f;
    fs_file_t_init(&f);
    if (fs_open(&f, path, FS_O_CREATE | FS_O_WRITE) != 0) {
        return;
    }
    char buf[TXT_HDR_SIZE];  
    /* compute duration excluding paused time when finalizing */
    uint32_t dur_ms = 0;
    if (final) {
        uint64_t now = k_uptime_get();
        uint64_t paused_total = g_paused_accum_ms + (g_paused ? (now - g_pause_start_ms) : 0);
        uint64_t dur = now - g_start_ms;
        if (dur > paused_total) dur -= paused_total; else dur = 0;
        dur_ms = (uint32_t)dur;
    }

    /* Channels + sample counts for the recorded set (g_hdr.channels). Add a
     * row to list a new channel. */
    static const struct {
        uint8_t     id;
        const char *name;
        unsigned    rate_hz;
    } chans[] = {
        { HPI_CH_ECG,    "ECG",    HPI_ECG_RATE_HZ  },
        { HPI_CH_PPG,    "PPG",    HPI_PPG_RATE_HZ  },
        { HPI_CH_RESP,   "RESP",   HPI_ECG_RATE_HZ },
        { HPI_CH_VITALS, "Vitals", 1                },
    };
    char chl[96] = "";
    char smp[96] = "";
    size_t co = 0, so = 0;

    for (size_t i = 0; i < ARRAY_SIZE(chans); i++) {
        if (!(g_hdr.channels & HPI_CH_BIT(chans[i].id))) {
            continue;
        }
        if (co >= sizeof(chl) || so >= sizeof(smp)) {
            break;
        }
        co += snprintf(chl + co, sizeof(chl) - co, "%s%s %u Hz",
                       co ? ", " : "", chans[i].name, chans[i].rate_hz);
        so += snprintf(smp + so, sizeof(smp) - so, "%s%s %u",
                       so ? ", " : "", chans[i].name,
                       (unsigned)g_hdr.sample_count[HP6_HDR_SLOT(chans[i].id)]);
    }
    if (co == 0) {
        strcpy(chl, "(none)");
        strcpy(smp, "(none)");
    }

    int n = snprintf(buf, sizeof(buf),
        "HealthyPi 6 Recording\r\n"
        "Format:    HPI6 v3.0 (.HP6)\r\n"
        "File:      %s\r\n"
        "Firmware:  %s   Board: %s\r\n"
        "Serial:    %.16s\r\n"
        "Session:   %s\r\n"
        "Channels:  %s\r\n"
        "Status:    %s\r\n"
        "Duration:  %u ms\r\n"
        "Samples:   %s\r\n"
        "Bytes:     %u\r\n",
        g_path, g_hdr.firmware_version, g_hdr.board_variant, g_hdr.serial_number,
        g_hdr.session_name[0] ? g_hdr.session_name : "(unnamed)",
        chl,
        final ? "complete" : "recording...",
        final ? dur_ms : 0u,
        smp, g_bytes);
    
    if (n < 0) n = 0;
    if (n > (int)sizeof(buf) - 2) n = (int)sizeof(buf) - 2;
    memset(buf + n, ' ', sizeof(buf) - 2 - n);   /* pad with spaces to 512 bytes */
    buf[sizeof(buf) - 2] = '\r';
    buf[sizeof(buf) - 1] = '\n';
    (void)fs_write(&f, buf, sizeof(buf));

    fs_sync(&f);
    fs_close(&f);
}

/* Append a single textual event line to the .TXT sidecar. Format: ts_ms\tseq\ttext\r\n
 * Best-effort: failure to write is ignored. Caller must hold no locks. */
static void txt_append_event_text(uint32_t ts_ms, uint32_t seq, const char *text)
{
    char path[64];
    strncpy(path, g_path, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    char *dot = strrchr(path, '.');
    if (dot) {
        strncpy(dot, ".TXT", 5);
    }
    struct fs_file_t f;
    fs_file_t_init(&f);
    int flags = FS_O_CREATE | FS_O_WRITE | FS_O_APPEND;
    if (fs_open(&f, path, flags) != 0) return;
    char buf[512];
    int n = snprintf(buf, sizeof(buf), "%u\t%u\t%s\r\n", ts_ms, seq, text ? text : "");
    if (n > 0) {
        (void)fs_write(&f, buf, (size_t)MIN(n, (int)sizeof(buf)));
        fs_sync(&f);
    }
    fs_close(&f);
}

/* Single path for all recorded events: computes the pause-adjusted
 * timestamp and shared sequence number, publishes the in-band frame
 * (unchanged .HP6/hp6_event wire shape), and mirrors it into .IDX with the
 * SRS §3.3 category + description, and into .TXT when there's text to show.
 * Returns the 1-based sequence number, or a negative errno. */
static int emit_event(uint16_t wire_type, enum hp6_idx_event_type idx_type,
                      const char *desc)
{
    if (!g_active) {
        return -EACCES;
    }
    uint64_t now = k_uptime_get();
    uint64_t paused_total;
    k_mutex_lock(&g_lock, K_FOREVER);
    paused_total = g_paused_accum_ms + (g_paused ? (now - g_pause_start_ms) : 0);
    k_mutex_unlock(&g_lock);
    uint32_t ts_ms = (uint32_t)((now - g_start_ms) - paused_total);
    uint16_t seq = (uint16_t)atomic_inc(&g_mark_seq) + 1;

    struct hp6_event ev = {
        .ts_ms = ts_ms,
        .type  = wire_type,
        .seq   = seq,
    };
    struct hpi_sample_frame f = {
        .channel = HPI_CH_EVENT,
        .sample_count = 1,
        .len = sizeof(ev),
        .t_mono_us = (uint64_t)k_uptime_get() * 1000ULL,
        .payload = &ev,
    };
    int rc = hpi_bus_publish(&f);
    if (rc <= 0) {
        LOG_WRN("event %u dropped at publish (%d)", seq, rc);
        return rc ? rc : -ENOSPC;
    }

    idx_append_event(ts_ms, idx_type, desc);
    if (desc && desc[0]) {
        txt_append_event_text(ts_ms, seq, desc);
    }
    return (int)seq;
}

int recording_add_event_text(const char *text)
{
    return emit_event(HP6_EVENT_USER_MARK, HP6_IDX_EVT_USER, text);
}

int recording_add_event(uint16_t type)
{
    enum hp6_idx_event_type idx_type =
        (type == HP6_EVENT_USER_MARK) ? HP6_IDX_EVT_USER : HP6_IDX_EVT_SYSTEM;
    int rc = emit_event(type, idx_type, NULL);
    return rc >= 0 ? 0 : rc;   /* preserve the header's documented 0/-errno contract */
}


/* Write a sync marker into the .HP6 and mirror it in the .IDX. */
static void emit_sync(void)
{
    (void)flush_buf();                 /* so file_off points at the sync frame */
    uint64_t file_off = g_bytes;
    uint64_t now = k_uptime_get();
    uint64_t paused_total = g_paused_accum_ms + (g_paused ? (now - g_pause_start_ms) : 0);
    uint32_t ts_ms = (uint32_t)((now - g_start_ms) - paused_total);

    struct hp6_sync_payload s = {
        .magic        = HP6_SYNC_MAGIC,
        .seq          = g_sync_seq,
        .wall_ms      = g_wall_start_ms ? (g_wall_start_ms + ts_ms) : 0,
        .ecg_count    = g_hdr.sample_count[HP6_HDR_SLOT(HPI_CH_ECG)],
        .ppg_count    = g_hdr.sample_count[HP6_HDR_SLOT(HPI_CH_PPG)],
        .eeg_count    = g_hdr.sample_count[HP6_HDR_SLOT(HPI_CH_EEG)],
        .vitals_count = g_hdr.sample_count[HP6_HDR_SLOT(HPI_CH_VITALS)],
        .events_since_last_sync = 0,
        .running_crc32 = g_running_crc,   /* CRC of the window just ended */
    };
    /* One sample of the 40-byte sync payload, on its own channel. ts_ms stays
     * session-relative INSIDE the payload (wall_ms is derived from it); the
     * block's own t_ms is uptime like every other block. */
    append_dblk(HPI_CH_SYNC, 0, (uint64_t)k_uptime_get(), 1, &s, sizeof(s));
    idx_append_sync(ts_ms, file_off, g_sync_seq, g_running_crc);

    g_sync_seq++;
    g_sync_count++;
    g_running_crc = 0;                  /* start a fresh window */
    g_last_sync_ms = k_uptime_get();

    /* Push the sync frame to the card and commit FAT metadata, so the
     * marker survives a pulled card or power loss. */
    if (flush_buf() == 0 && g_file_open) {
        fs_sync(&g_file);
        if (g_idx_open) {
            fs_sync(&g_idx);
        }
    }
}

/* Auto-close the current file at REC_ROLLOVER_MS and continue the same
 * session into "<base>_NNN.HP6" in the same day dir (SRS §3.1, §5).
 * Runs on the recording thread, under g_lock. Mirrors hpi_recording_stop()'s
 * finalize sequence but leaves g_active/g_paused/g_mark_seq alone -- this is
 * a file boundary, not a session boundary. */
static void rollover_file(void)
{
    emit_sync();
    if (flush_buf() != 0 || g_error) {
        return;             /* error state already entered; don't open a new file */
    }
    txt_write(true);

    uint64_t now = k_uptime_get();
    uint64_t paused_total = g_paused_accum_ms + (g_paused ? (now - g_pause_start_ms) : 0);
    uint64_t dur = now - g_start_ms;
    dur = (dur > paused_total) ? dur - paused_total : 0;
    uint32_t dur_ms = (uint32_t)dur;
    uint32_t channels_host = g_hdr.channels;

    idx_finalize(dur_ms, channels_host);

    g_hdr.duration_ms = sys_cpu_to_le32(dur_ms);
    g_hdr.timestamp_end = sys_cpu_to_le64(
        g_wall_start_ms ? (g_wall_start_ms + dur_ms) : 0);
    for (int i = 0; i < HP6_HDR_CHANNEL_SLOTS; i++) {
        g_hdr.sample_count[i] = sys_cpu_to_le32(g_hdr.sample_count[i]);
    }
    g_hdr.channels = sys_cpu_to_le32(channels_host);
    g_hdr.event_count = sys_cpu_to_le32(g_hdr.event_count);
    g_hdr.events_offset = sys_cpu_to_le64(g_hdr.events_offset);
    g_hdr.header_crc32 = sys_cpu_to_le32(crc32_ieee((const uint8_t *)&g_hdr, 248));
    if (fs_seek(&g_file, 0, FS_SEEK_SET) == 0) {
        (void)fs_write(&g_file, &g_hdr, sizeof(g_hdr));
    }
    fs_sync(&g_file);
    fs_close(&g_file);
    g_file_open = false;
    LOG_INF("rollover: closed %s (%u bytes, %u syncs)", g_path, g_bytes, g_sync_count);

    snprintf(g_path, sizeof(g_path), "%s_%03u.HP6", g_session_base_path,
             (unsigned)(++g_rollover_seq));
    g_wall_start_ms = rtc_unix_ms();
    header_init(g_session_name);

    fs_file_t_init(&g_file);
    int rc = fs_open(&g_file, g_path, FS_O_CREATE | FS_O_WRITE);
    if (rc != 0) {
        LOG_ERR("rollover: fs_open %s failed (%d) -- stopping", g_path, rc);
        enter_error_state(rc);
        return;
    }
    ssize_t w = fs_write(&g_file, &g_hdr, sizeof(g_hdr));
    if (w != (ssize_t)sizeof(g_hdr)) {
        LOG_ERR("rollover: header write failed (%d) -- stopping", (int)w);
        fs_close(&g_file);
        enter_error_state((int)w);
        return;
    }
    g_file_open = true;
    g_bytes = sizeof(g_hdr);
    g_wlen = 0;
    g_start_ms = (uint64_t)k_uptime_get();
    g_paused_accum_ms = 0;   /* pre-rollover pause time belongs to the old file */
    g_seq = 0;
    g_sync_seq = 0;
    g_sync_count = 0;
    g_running_crc = 0;
    g_last_sync_ms = k_uptime_get();
    idx_open();
    txt_write(false);

    LOG_INF("rollover: continuation started: %s (wall=%llu ms)", g_path,
            (unsigned long long)g_wall_start_ms);
}

/* Periodic tick from the recording thread: sync marker, or rollover if the
 * current file has hit REC_ROLLOVER_MS. Do-nothing if called before
 * REC_SYNC_PERIOD_MS has elapsed, so callers can invoke it unconditionally. */
static void check_periodic(void)
{
    if (k_uptime_get() - g_last_sync_ms < (int64_t)REC_SYNC_PERIOD_MS) {
        return;
    }
    if (!platform_fs_is_ready()) {
        /* Card pulled mid-recording. Caught here (once per sync interval)
         * rather than waiting for the next fs_write() to fail, since a
         * missing card can otherwise surface as a generic write error. */
        enter_error_state(-ENODEV);
        return;
    }
    uint64_t now = k_uptime_get();
    uint64_t paused_total = g_paused_accum_ms + (g_paused ? (now - g_pause_start_ms) : 0);
    uint64_t elapsed = now - g_start_ms;
    elapsed = (elapsed > paused_total) ? elapsed - paused_total : 0;

    if (elapsed >= REC_ROLLOVER_MS) {
        rollover_file();
    } else {
        emit_sync();
    }
}
/* ---- public API ---- */

int hpi_recording_start(const char *session_name)
{
    k_mutex_lock(&g_lock, K_FOREVER);
    int rc = 0;

    if (g_active) { rc = -EBUSY; goto out; }

    g_error = false;
    g_error_code = 0;

    /* Writer thread must have a live bus subscription, else we'd write only the
     * header and capture nothing (e.g. if the ring alloc failed at boot). */
    if (g_sub == NULL) {
        LOG_ERR("recording: no bus subscription (writer not ready); aborting start");
        rc = -ENODEV; goto out;
    }

    /* recording while the host owns the SD. */
    if (!platform_fs_is_ready()) { rc = -ENODEV; goto out; }

    {
        uint64_t free_bytes = 0, total_bytes = 0;
        if (recording_get_free_space(&free_bytes, &total_bytes) == 0 &&
            free_bytes < REC_MIN_FREE_BYTES) {
            LOG_WRN("recording: refusing start, %llu bytes free (< %llu min)",
                    (unsigned long long)free_bytes,
                    (unsigned long long)REC_MIN_FREE_BYTES);
            rc = -ENOSPC;
            goto out;
        }
    }

    mkdir_p(REC_ROOT);
    {
        struct fs_dirent probe;
        if (fs_stat(REC_ROOT, &probe) != 0) {
            LOG_WRN("recording: %s not reachable, card missing?", REC_ROOT);
            rc = -ENODEV;
            goto out;
        }
    }
    rc = build_path();
    if (rc != 0) {
        goto out;
    }

    strncpy(g_session_base_path, g_path, sizeof(g_session_base_path) - 1);
    g_session_base_path[sizeof(g_session_base_path) - 1] = '\0';
    {
        char *dot = strrchr(g_session_base_path, '.');
        if (dot) {
            *dot = '\0';
        }
    }
    g_rollover_seq = 0;
    strncpy(g_session_name, session_name ? session_name : "", sizeof(g_session_name) - 1);
    g_session_name[sizeof(g_session_name) - 1] = '\0';

    g_wall_start_ms = rtc_unix_ms();
    header_init(session_name);

    fs_file_t_init(&g_file);
    rc = fs_open(&g_file, g_path, FS_O_CREATE | FS_O_WRITE);
    if (rc != 0) {
        LOG_ERR("fs_open %s failed (%d)", g_path, rc);
        log_path_status(g_path);
        /* The folder was just verified, so an I/O or not-found error
         * here means the card went away. */
        if (rc == -EIO || rc == -ENOENT) {
            rc = -ENODEV;
        }
        goto out;
    }
    ssize_t w = fs_write(&g_file, &g_hdr, sizeof(g_hdr));
    if (w != (ssize_t)sizeof(g_hdr)) {
        LOG_ERR("header write failed (%d)", (int)w);
        fs_close(&g_file);
        rc = -EIO;
        goto out;
    }
    g_bytes = sizeof(g_hdr);
    g_wlen = 0;
    g_start_ms = (uint64_t)k_uptime_get();
    g_sync_seq = 0;
    g_sync_count = 0;
    g_seq = 0;
    (void)atomic_set(&g_mark_seq, 0);
    g_running_crc = 0;
    g_last_sync_ms = k_uptime_get();
    g_paused = false;           
    g_pause_start_ms = 0;       
    g_paused_accum_ms = 0;        
    idx_open();
    txt_write(false);
    g_file_open = true;
    g_active = true;
    LOG_INF("recording started: %s (wall=%llu ms)", g_path,
            (unsigned long long)g_wall_start_ms);

out:
    k_mutex_unlock(&g_lock);
    return rc;
}

int hpi_recording_stop(void)
{
    k_mutex_lock(&g_lock, K_FOREVER);
    int rc = 0;

        if (!g_active) { rc = -EALREADY; goto out; }
    g_active = false;

    emit_sync();            /* final sync marker (covers the last window) */
    if (flush_buf() != 0 || g_error) {
        rc = g_error_code;  /* files were already closed by enter_error_state() */
        goto out;
    }
    txt_write(true);        /* uses live counters before they're byte-swapped */

    /* Compute duration + channels (host-order) BEFORE idx_finalize() -- the
     * .IDX and the .HP6 header both need them, and g_hdr.channels is about
     * to be byte-swapped in place below, so capture it here first. */
    uint64_t now = k_uptime_get();
    uint64_t paused_total = g_paused_accum_ms + (g_paused ? (now - g_pause_start_ms) : 0);
    uint64_t dur = now - g_start_ms;
    if (dur > paused_total) dur -= paused_total; else dur = 0;
    uint32_t dur_ms = (uint32_t)dur;
    uint32_t channels_host = g_hdr.channels;

    idx_finalize(dur_ms, channels_host);

    /* Finalize header: end time, duration, counters, fresh CRC. */
    g_hdr.duration_ms = sys_cpu_to_le32(dur_ms);
    g_hdr.timestamp_end = sys_cpu_to_le64(
        g_wall_start_ms ? (g_wall_start_ms + dur_ms) : 0);
    for (int i = 0; i < HP6_HDR_CHANNEL_SLOTS; i++) {
        g_hdr.sample_count[i] = sys_cpu_to_le32(g_hdr.sample_count[i]);
    }
    g_hdr.channels = sys_cpu_to_le32(channels_host);
    g_hdr.event_count = sys_cpu_to_le32(g_hdr.event_count);
    g_hdr.events_offset = sys_cpu_to_le64(g_hdr.events_offset);
    g_hdr.header_crc32 = sys_cpu_to_le32(crc32_ieee((const uint8_t *)&g_hdr, 248));

    if (fs_seek(&g_file, 0, FS_SEEK_SET) == 0) {
        (void)fs_write(&g_file, &g_hdr, sizeof(g_hdr));
    }
    fs_sync(&g_file);
    fs_close(&g_file);
    g_file_open = false;
    LOG_INF("recording stopped: %s (%u bytes, %u syncs)", g_path, g_bytes,
            g_sync_count);

out:
    k_mutex_unlock(&g_lock);
    return rc;
}

int hpi_recording_mark(void)
{
    /* Previously published to the bus but never called idx_append_event() --
     * plain marks (the MARK EVENT button, no note) never reached .IDX. */
    int rc = emit_event(HP6_EVENT_USER_MARK, HP6_IDX_EVT_USER, NULL);
    if (rc < 0) {
        return rc;
    }
    LOG_INF("mark %d recorded", rc);
    return rc;
}

bool hpi_recording_active(void)
{
    return g_active;
}

void hpi_recording_get_status(struct hpi_recording_status *out)
{
    if (!out) {
        return;
    }
    out->active = g_active;
    out->error = g_error;
    out->error_code = g_error_code;
    out->bytes_written = g_bytes;
    if (g_active) {
        uint64_t now = k_uptime_get();
        uint64_t paused_total;
        k_mutex_lock(&g_lock, K_FOREVER);
        paused_total = g_paused_accum_ms + (g_paused ? (now - g_pause_start_ms) : 0);
        k_mutex_unlock(&g_lock);
        uint64_t dur = now - g_start_ms;
        if (dur > paused_total) dur -= paused_total; else dur = 0;
        out->duration_ms = (uint32_t)dur;
    } else {
        out->duration_ms = 0;
    }
    out->ecg_samples = g_hdr.sample_count[HP6_HDR_SLOT(HPI_CH_ECG)];
    out->ppg_samples = g_hdr.sample_count[HP6_HDR_SLOT(HPI_CH_PPG)];
    out->vitals_samples = g_hdr.sample_count[HP6_HDR_SLOT(HPI_CH_VITALS)];
    strncpy(out->path, g_path, sizeof(out->path) - 1);
    out->path[sizeof(out->path) - 1] = '\0';
}

int hpi_recording_pause(bool pause)
{
    if (!g_active) return -EACCES;
    k_mutex_lock(&g_lock, K_FOREVER);
    if (pause == g_paused) {
        k_mutex_unlock(&g_lock);
        return 0;
    }
    if (pause) {
        /* entering pause: record pause start */
        g_pause_start_ms = k_uptime_get();
        g_paused = true;
    } else {
        /* leaving pause: accumulate paused duration */
        if (g_pause_start_ms) {
            g_paused_accum_ms += (k_uptime_get() - g_pause_start_ms);
            g_pause_start_ms = 0;
        }
        g_paused = false;
    }
    k_mutex_unlock(&g_lock);
    return 0;
}

bool hpi_recording_has_error(void)
{
    return g_error;
}

int hpi_recording_get_error(void)
{
    return g_error_code;
}

void hpi_recording_clear_error(void)
{
    k_mutex_lock(&g_lock, K_FOREVER);
    g_error = false;
    g_error_code = 0;
    k_mutex_unlock(&g_lock);
}

int hpi_recording_set_channel_enabled(uint8_t channel, bool enabled)
{
    if (channel == 0) return -EINVAL;
    k_mutex_lock(&g_lock, K_FOREVER);
    uint32_t bit = HPI_CH_BIT(channel);
    if (enabled) g_channel_mask |= bit;
    else g_channel_mask &= ~bit;
    /* Update header live so the finalized header reflects the chosen set. */
   // g_hdr.channels = g_channel_mask;
    k_mutex_unlock(&g_lock);
    return 0;
}

uint32_t hpi_recording_get_channel_mask(void)
{
    return g_channel_mask;
}

int recording_get_free_space(uint64_t *free_bytes, uint64_t *total_bytes)
{
    if (free_bytes) *free_bytes = 0;
    if (total_bytes) *total_bytes = 0;
    struct fs_statvfs st;
    if (!platform_fs_is_ready()) {
        return -ENODEV;
    }
    if (fs_statvfs("/SD:", &st) != 0) {
        return -EIO;
    }
    uint64_t total = (uint64_t)st.f_blocks * st.f_frsize;
    uint64_t freeb = (uint64_t)st.f_bfree * st.f_frsize;
    if (free_bytes) *free_bytes = freeb;
    if (total_bytes) *total_bytes = total;
    return 0;
}

struct rec_datetime { uint16_t year; uint8_t month, day, hour, min, sec; };

static void parse_path_datetime_dt(const char *hp6_path, struct rec_datetime *out)
{
	out->year = 0;
	out->month = out->day = out->hour = out->min = out->sec = 0;

	const char *fname = strrchr(hp6_path, '/');
	if (!fname) {
		return;
	}
	fname++;

	const char *daydir_end = fname - 1;
	const char *daydir_start = daydir_end;
	while (daydir_start > hp6_path && *(daydir_start - 1) != '/') {
		daydir_start--;
	}
	size_t daylen = (size_t)(daydir_end - daydir_start);
	char daybuf[16] = {0};
	if (daylen == 0 || daylen >= sizeof(daybuf)) {
		return;
	}
	memcpy(daybuf, daydir_start, daylen);

	unsigned y = 0, mo = 0, d = 0, hh = 0, mm = 0, ss = 0;
	if (sscanf(daybuf, "%4u%2u%2u", &y, &mo, &d) == 3 &&
	    sscanf(fname, "%2u%2u%2u", &hh, &mm, &ss) == 3) {
		out->year = (uint16_t)y;
		out->month = (uint8_t)mo;
		out->day = (uint8_t)d;
		out->hour = (uint8_t)hh;
		out->min = (uint8_t)mm;
		out->sec = (uint8_t)ss;
	}
}

static void parse_path_datetime(const char *hp6_path, struct recording_summary *out)
{
	struct rec_datetime dt;
	parse_path_datetime_dt(hp6_path, &dt);
	out->year = dt.year; out->month = dt.month; out->day = dt.day;
	out->hour = dt.hour; out->min = dt.min; out->sec = dt.sec;
}

/* Best-effort peek at a finalized header for duration + channels. */
static int read_header_summary(const char *hp6_path, uint32_t *duration_ms,
				uint32_t *channels)
{
	struct fs_file_t f;
	struct recording_header h;

	*duration_ms = 0;
	*channels = 0;

	fs_file_t_init(&f);
	if (fs_open(&f, hp6_path, FS_O_READ) != 0) {
		return -ENOENT;
	}
	ssize_t r = fs_read(&f, &h, sizeof(h));
	fs_close(&f);

	if (r != (ssize_t)sizeof(h) || memcmp(h.magic, "HPI6", 4) != 0) {
		return -EIO;
	}
	
    *channels = sys_le32_to_cpu(h.channels);   /* intended mask is valid even if open */
	if (h.timestamp_end == 0xFFFFFFFFFFFFFFFFULL) {
		return -EAGAIN;   /* never finalized */
	}
	*duration_ms = sys_le32_to_cpu(h.duration_ms);
	return 0;
}

/* Newest-first by the YYYYMMDDHHMMSS packed into each entry. */
static uint64_t sort_key(const struct recording_summary *e)
{
	return (((uint64_t)e->year * 10000ULL + e->month * 100U + e->day) * 1000000ULL) +
	       (e->hour * 10000U + e->min * 100U + e->sec);
}

static void sort_entries_desc(struct recording_summary *arr, size_t n)
{
	for (size_t i = 1; i < n; i++) {
		struct recording_summary tmp = arr[i];
		uint64_t key = sort_key(&tmp);
		size_t j = i;
		while (j > 0 && sort_key(&arr[j - 1]) < key) {
			arr[j] = arr[j - 1];
			j--;
		}
		arr[j] = tmp;
	}
}

// /* Async listing work: enumerate /SD:/HPI6/REC/*/*.HP6 and call cb for each
//  * entry found. Runs in a worker so it won't block the UI. */
// */
static int read_idx_counts(const char *idx_path, uint32_t *out_event_count,
                            uint32_t *out_sync_count)
{
    struct fs_file_t f;
    fs_file_t_init(&f);
    if (fs_open(&f, idx_path, FS_O_READ) != 0) return -ENOENT;
    uint8_t h[HP6_IDX_HDR_SIZE];
    if (fs_read(&f, h, sizeof(h)) != sizeof(h)) { fs_close(&f); return -EIO; }
    if (out_event_count) *out_event_count = sys_get_le16(&h[6]);
    if (out_sync_count)  *out_sync_count  = sys_get_le32(&h[8]);
    fs_close(&f);
    return 0;
}

/* Like read_idx_counts(), but also returns duration_ms/channels when the
 * .IDX was written with header v0x0301+ -- letting page_work_fn() list
 * without ever opening the .HP6 (SRS §3.4/§4.2).
 *
 * CRITICAL: gate on the version field, never on how many bytes fs_read()
 * returned. An older, pre-0x0301 .IDX still fills the full HP6_IDX_HDR_SIZE
 * read -- its reserved event table starts right where the old, shorter
 * header ended, so bytes [12..20) belong to that first event slot, not to
 * unused padding. Reading them as duration/channels would show garbage
 * instead of "unknown". */

static int read_idx_summary(const char *idx_path, uint32_t *out_event_count,
                            uint32_t *out_sync_count, uint32_t *out_duration_ms,
                            uint32_t *out_channels)
{
    struct fs_file_t f;
    fs_file_t_init(&f);
    if (fs_open(&f, idx_path, FS_O_READ) != 0) return -ENOENT;

    uint8_t h[HP6_IDX_HDR_SIZE];
    ssize_t r = fs_read(&f, h, sizeof(h));
    if (r < 12) { fs_close(&f); return -EIO; }

    uint16_t version = sys_get_le16(&h[4]);
    uint32_t ev = sys_get_le16(&h[6]);
    uint32_t sc = sys_get_le32(&h[8]);
    uint32_t dur = 0, ch = 0;

    if (version >= 0x0301 && r >= (ssize_t)HP6_IDX_HDR_SIZE) {
        dur = sys_get_le32(&h[12]);
        ch  = sys_get_le32(&h[16]);

        /* Never finalized (card pulled / power loss): recover the duration
         * from the last complete sync record. */
        if (dur == 0 && sc == 0) {
            struct fs_dirent st;
            if (fs_stat(idx_path, &st) == 0 && st.size > HP6_IDX_SYNCS_OFFSET) {
                uint32_t n = (uint32_t)(st.size - HP6_IDX_SYNCS_OFFSET) /
                             HP6_IDX_SYNC_REC_SIZE;
                if (n > 0) {
                    uint8_t rec[HP6_IDX_SYNC_REC_SIZE];
                    off_t off = HP6_IDX_SYNCS_OFFSET +
                                (off_t)(n - 1) * HP6_IDX_SYNC_REC_SIZE;
                    if (fs_seek(&f, off, FS_SEEK_SET) == 0 &&
                        fs_read(&f, rec, sizeof(rec)) == (ssize_t)sizeof(rec)) {
                        dur = sys_get_le32(&rec[0]);
                        sc  = n;
                    }
                }
            }
        }
    }
    fs_close(&f);

    if (out_event_count) *out_event_count = ev;
    if (out_sync_count)  *out_sync_count  = sc;
    if (out_duration_ms) *out_duration_ms = dur;
    if (out_channels)    *out_channels    = ch;
    return 0;
}
static void listing_work_fn(struct k_work *work)
{
	struct list_work_ctx *ctx = CONTAINER_OF(work, struct list_work_ctx, work);
	struct fs_dir_t dayd;
	struct fs_dirent ent;
	const char *root = REC_ROOT;

    fs_dir_t_init(&dayd);   

    int drc = fs_opendir(&dayd, root);
	LOG_INF("REC-LIST: opendir(%s)=%d fs_ready=%d", root, drc, (int)platform_fs_is_ready());
	if (drc != 0) {
		goto done;
	}

	while (fs_readdir(&dayd, &ent) == 0 && ent.name[0]) {
		if (ent.type != FS_DIR_ENTRY_DIR) continue;
		char daypath[128];
		snprintf(daypath, sizeof(daypath), "%s/%s", root, ent.name);
		struct fs_dir_t fd;
        fs_dir_t_init(&fd);   
		if (fs_opendir(&fd, daypath) != 0) continue;

		while (fs_readdir(&fd, &ent) == 0 && ent.name[0]) {
			if (ent.type != FS_DIR_ENTRY_FILE) continue;
			const char *name = ent.name;
			size_t ln = strlen(name);
			if (ln < 5 || strcasecmp(name + ln - 4, ".HP6") != 0) continue;

			char hp6_path[256];
			snprintf(hp6_path, sizeof(hp6_path), "%s/%s", daypath, name);

			char idx_path[256];
			strncpy(idx_path, hp6_path, sizeof(idx_path) - 1);
			idx_path[sizeof(idx_path) - 1] = '\0';
			char *dot = strrchr(idx_path, '.');
			if (dot) strncpy(dot, ".IDX", 5);

			if (ctx->n_entries >= ctx->max_entries) continue;

			struct recording_summary *e = &ctx->entries[ctx->n_entries];
			memset(e, 0, sizeof(*e));
			strncpy(e->path, hp6_path, sizeof(e->path) - 1);
			e->size_bytes = (uint32_t)ent.size;

			uint32_t event_count = 0, sync_count = 0;
			(void)read_idx_counts(idx_path, &event_count, &sync_count);
			e->event_count = event_count;

			uint32_t dur = 0, chans = 0;
			(void)read_header_summary(hp6_path, &dur, &chans);
			e->duration_ms = dur;
			e->channels = chans;

			parse_path_datetime(hp6_path, e);
			ctx->n_entries++;
		}
		fs_closedir(&fd);
	}
	fs_closedir(&dayd);

	sort_entries_desc(ctx->entries, ctx->n_entries);

	for (size_t i = 0; i < ctx->n_entries; i++) {
		ctx->cb(&ctx->entries[i], ctx->user_data);
	}
	ctx->cb(NULL, ctx->user_data);   /* completion sentinel */

	LOG_INF("REC-LIST: total entries found = %zu", ctx->n_entries);
done:
	k_free(ctx->entries);
	k_free(ctx);
}

int recording_list_async(recording_list_cb_t cb, void *user_data)
{
	if (!cb) return -EINVAL;
	struct list_work_ctx *ctx = k_malloc(sizeof(*ctx));
	if (!ctx) return -ENOMEM;
	ctx->entries = k_malloc(sizeof(struct recording_summary) * REC_LIST_MAX_ENTRIES);
	if (!ctx->entries) {
		LOG_ERR("recording_list_async: k_malloc(%u entries, %u B) failed",
			REC_LIST_MAX_ENTRIES,
			(unsigned)(sizeof(struct recording_summary) * REC_LIST_MAX_ENTRIES));
		k_free(ctx);
		return -ENOMEM;
	}
	ctx->n_entries = 0;
	ctx->max_entries = REC_LIST_MAX_ENTRIES;
	ctx->cb = cb;
	ctx->user_data = user_data;
	k_work_init(&ctx->work, listing_work_fn);
	//k_work_submit(&ctx->work);
    k_work_submit_to_queue(&s_rec_wq, &ctx->work);
	return 0;
}
static struct recording_index_entry *s_index;
static uint32_t s_index_n;
static uint32_t s_index_cap;
static bool     s_index_cap_warned;
static volatile int s_index_rc;

struct rec_page_cache_slot {
	bool     valid;
	uint32_t start_idx;
	uint32_t count;
	uint32_t lru_stamp;
	struct recording_summary entries[REC_PAGE_SIZE_MAX];
};
static struct rec_page_cache_slot s_page_cache[REC_PAGE_CACHE_SLOTS];
static uint32_t s_page_cache_clock;

struct index_work_ctx {
	struct k_work work;
	recording_index_cb_t cb;
	void *user_data;
};

struct page_work_ctx {
	struct k_work work;
	uint32_t start_idx;
	uint32_t count;
	recording_page_cb_t cb;
	void *user_data;
};

static uint64_t index_sort_key(const struct recording_index_entry *e)
{
	return (((uint64_t)e->year * 10000ULL + e->month * 100U + e->day) * 1000000ULL) +
	       (e->hour * 10000U + e->min * 100U + e->sec);
}

static void index_sort_desc(struct recording_index_entry *arr, size_t n)
{
	for (size_t i = 1; i < n; i++) {
		struct recording_index_entry tmp = arr[i];
		uint64_t key = index_sort_key(&tmp);
		size_t j = i;
		while (j > 0 && index_sort_key(&arr[j - 1]) < key) {
			arr[j] = arr[j - 1];
			j--;
		}
		arr[j] = tmp;
	}
}

static void rec_page_cache_invalidate_all(void)
{
	for (int i = 0; i < REC_PAGE_CACHE_SLOTS; i++) {
		s_page_cache[i].valid = false;
	}
}

static void index_work_fn(struct k_work *work)
{
	struct index_work_ctx *ctx = CONTAINER_OF(work, struct index_work_ctx, work);
	struct fs_dir_t dayd;
	struct fs_dirent ent;
	const char *root = REC_ROOT;

	s_index_n = 0;
	s_index_cap_warned = false;
	rec_page_cache_invalidate_all();

	fs_dir_t_init(&dayd);
	int orc = fs_opendir(&dayd, root);
	s_index_rc = orc;
	if (orc != 0) {
		goto done;
	}

	while (fs_readdir(&dayd, &ent) == 0 && ent.name[0]) {
		if (ent.type != FS_DIR_ENTRY_DIR) continue;
		char daypath[128];
		snprintf(daypath, sizeof(daypath), "%s/%s", root, ent.name);
		struct fs_dir_t fd;
		fs_dir_t_init(&fd);
		if (fs_opendir(&fd, daypath) != 0) continue;

		while (fs_readdir(&fd, &ent) == 0 && ent.name[0]) {
			if (ent.type != FS_DIR_ENTRY_FILE) continue;
			const char *name = ent.name;
			size_t ln = strlen(name);
			if (ln < 5 || strcasecmp(name + ln - 4, ".HP6") != 0) continue;

			if (s_index_n >= s_index_cap) {
				if (!s_index_cap_warned) {
					LOG_WRN("recording index full at %u entries -- "
						"recordings past this won't list",
						s_index_cap);
					s_index_cap_warned = true;
				}
				continue;
			}

			char hp6_path[256];
			snprintf(hp6_path, sizeof(hp6_path), "%s/%s", daypath, name);

			struct recording_index_entry *e = &s_index[s_index_n];
			memset(e, 0, sizeof(*e));
			strncpy(e->path, hp6_path, sizeof(e->path) - 1);

			struct rec_datetime dt;
			parse_path_datetime_dt(hp6_path, &dt);
			e->year = dt.year; e->month = dt.month; e->day = dt.day;
			e->hour = dt.hour; e->min = dt.min; e->sec = dt.sec;
			s_index_n++;
		}
		fs_closedir(&fd);
	}
	fs_closedir(&dayd);

	index_sort_desc(s_index, s_index_n);

done:
	if (ctx->cb) {
		ctx->cb(s_index, s_index_n, ctx->user_data);
	}
	k_free(ctx);
}

int recording_index_last_error(void)
{
	return s_index_rc;
}

int recording_index_build_async(recording_index_cb_t cb, void *user_data)
{
	if (!cb) return -EINVAL;
	// if (!s_index) {
	// 	s_index = k_malloc(sizeof(*s_index) * REC_INDEX_MAX_ENTRIES);
	// 	if (!s_index) return -ENOMEM;
	// 	s_index_cap = REC_INDEX_MAX_ENTRIES;
	// }
    static struct recording_index_entry s_index_storage[REC_INDEX_MAX_ENTRIES];
	if (!s_index) {
		s_index = s_index_storage;
		s_index_cap = REC_INDEX_MAX_ENTRIES;
	}
	struct index_work_ctx *ctx = k_malloc(sizeof(*ctx));
	if (!ctx) return -ENOMEM;
	ctx->cb = cb;
	ctx->user_data = user_data;
	k_work_init(&ctx->work, index_work_fn);
	k_work_submit_to_queue(&s_rec_wq, &ctx->work);
	return 0;
}

static struct rec_page_cache_slot *page_cache_find(uint32_t start_idx, uint32_t count)
{
	for (int i = 0; i < REC_PAGE_CACHE_SLOTS; i++) {
		if (s_page_cache[i].valid && s_page_cache[i].start_idx == start_idx &&
		    s_page_cache[i].count == count) {
			return &s_page_cache[i];
		}
	}
	return NULL;
}

static struct rec_page_cache_slot *page_cache_slot_for_write(void)
{
	struct rec_page_cache_slot *victim = &s_page_cache[0];
	for (int i = 0; i < REC_PAGE_CACHE_SLOTS; i++) {
		if (!s_page_cache[i].valid) return &s_page_cache[i];
		if (s_page_cache[i].lru_stamp < victim->lru_stamp) victim = &s_page_cache[i];
	}
	return victim;
}

static void page_work_fn(struct k_work *work)
{
	struct page_work_ctx *ctx = CONTAINER_OF(work, struct page_work_ctx, work);
	uint32_t start = ctx->start_idx;
	uint32_t count = ctx->count;

	if (start >= s_index_n) {
		count = 0;
	} else if (start + count > s_index_n) {
		count = s_index_n - start;
	}

	struct rec_page_cache_slot *hit = page_cache_find(start, count);
	if (hit) {
		hit->lru_stamp = ++s_page_cache_clock;
		for (uint32_t i = 0; i < hit->count; i++) {
			ctx->cb(&hit->entries[i], ctx->user_data);
		}
		ctx->cb(NULL, ctx->user_data);
		k_free(ctx);
		return;
	}

	struct rec_page_cache_slot *slot = page_cache_slot_for_write();
	slot->valid = false;
	slot->start_idx = start;
	slot->count = count;

	for (uint32_t i = 0; i < count; i++) {
		const struct recording_index_entry *ie = &s_index[start + i];
		struct recording_summary *sm = &slot->entries[i];

		memset(sm, 0, sizeof(*sm));
		strncpy(sm->path, ie->path, sizeof(sm->path) - 1);
		sm->year = ie->year; sm->month = ie->month; sm->day = ie->day;
		sm->hour = ie->hour; sm->min = ie->min; sm->sec = ie->sec;

		struct fs_dirent ent;
		if (fs_stat(ie->path, &ent) == 0) {
			sm->size_bytes = (uint32_t)ent.size;
		}

		char idx_path[256];
		strncpy(idx_path, ie->path, sizeof(idx_path) - 1);
		idx_path[sizeof(idx_path) - 1] = '\0';
		char *dot = strrchr(idx_path, '.');
		if (dot) strncpy(dot, ".IDX", 5);
		uint32_t event_count = 0, sync_count = 0, dur = 0, chans = 0;
		(void)read_idx_summary(idx_path, &event_count, &sync_count, &dur, &chans);
        if (chans == 0) {
			uint32_t d2 = 0, c2 = 0;
			(void)read_header_summary(ie->path, &d2, &c2);
			chans = c2;
		}
		sm->event_count = event_count;
		sm->duration_ms = dur;
		sm->channels = chans;
	}

	slot->valid = true;
	slot->lru_stamp = ++s_page_cache_clock;

	for (uint32_t i = 0; i < count; i++) {
		ctx->cb(&slot->entries[i], ctx->user_data);
	}
	ctx->cb(NULL, ctx->user_data);
	k_free(ctx);
}

int recording_page_fetch_async(uint32_t start_idx, uint32_t count,
                               recording_page_cb_t cb, void *user_data)
{
	if (!cb || count == 0) return -EINVAL;
	if (count > REC_PAGE_SIZE_MAX) count = REC_PAGE_SIZE_MAX;

	struct page_work_ctx *ctx = k_malloc(sizeof(*ctx));
	if (!ctx) return -ENOMEM;
	ctx->start_idx = start_idx;
	ctx->count = count;
	ctx->cb = cb;
	ctx->user_data = user_data;
	k_work_init(&ctx->work, page_work_fn);
	k_work_submit_to_queue(&s_rec_wq, &ctx->work);
	return 0;
}

int recording_delete(const char *hp6_path)
{
	if (!hp6_path) return -EINVAL;

	k_mutex_lock(&g_lock, K_FOREVER);

	if (g_active && strcmp(hp6_path, g_path) == 0) {
		k_mutex_unlock(&g_lock);
		return -EBUSY;
	}

	char idx_path[128], txt_path[128];
	strncpy(idx_path, hp6_path, sizeof(idx_path) - 1);
	idx_path[sizeof(idx_path) - 1] = '\0';
	char *dot = strrchr(idx_path, '.');
	if (dot) strncpy(dot, ".IDX", 5);

	strncpy(txt_path, hp6_path, sizeof(txt_path) - 1);
	txt_path[sizeof(txt_path) - 1] = '\0';
	dot = strrchr(txt_path, '.');
	if (dot) strncpy(dot, ".TXT", 5);

	int rc = fs_unlink(hp6_path);
	if (rc != 0 && rc != -ENOENT) goto out;
	rc = fs_unlink(txt_path);
	if (rc != 0 && rc != -ENOENT) goto out;
	rc = fs_unlink(idx_path);
	if (rc != 0 && rc != -ENOENT) goto out;
	rc = 0;
    rec_page_cache_invalidate_all();

out:
	k_mutex_unlock(&g_lock);
	return rc;
}

/* ---- writer thread ---- */

static void recording_thread(void *a, void *b, void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    struct hpi_bus_sub_cfg cfg = {
        .name = "rec",
        .channel_mask = HPI_CH_BIT(HPI_CH_ECG) | HPI_CH_BIT(HPI_CH_PPG) |
                        HPI_CH_BIT(HPI_CH_RESP) | HPI_CH_BIT(HPI_CH_VITALS) |
                        HPI_CH_BIT(HPI_CH_EEG) | HPI_CH_BIT(HPI_CH_EVENT) |
                        HPI_CH_BIT(HPI_CH_INFER),
        .ring_frames = REC_RING,
    };
    g_sub = hpi_bus_subscribe(&cfg);
    if (g_sub == NULL) {
        LOG_ERR("recording: bus subscribe failed");
        return;
    }

    struct hpi_sample_frame f;
    while (1) {
        if (hpi_bus_pull_wait(g_sub, &f, 100) != 0) {
            continue;
        }
        /* While paused, ignore data channels (ECG/PPG/RESP/VITALS/EEG/INFER)
         * but still accept EVENT and SYNC frames so markers and recovery work. */
        if (!g_active) {
            continue;   /* drain + discard while idle */
        }
        k_mutex_lock(&g_lock, K_FOREVER);
        if (g_active) {
            if (g_paused && f.channel != HPI_CH_EVENT && f.channel != HPI_CH_SYNC) {
                /* drop data while paused */
            } else {
                /* If this is a data channel, respect the dynamic channel mask
                 * configured by the UI/service. EVENT and SYNC frames are
                 * always accepted. */
                // if (f.channel != HPI_CH_EVENT && f.channel != HPI_CH_SYNC) {
                //     if (!(g_channel_mask & HPI_CH_BIT(f.channel))) {
                //         /* configured to drop this channel */
                //     } else {
                if (f.channel != HPI_CH_EVENT && f.channel != HPI_CH_SYNC) {
                    uint32_t want = HPI_CH_BIT(f.channel);
                    if (f.channel == HPI_CH_ECG) {
                        want |= HPI_CH_BIT(HPI_CH_RESP);   /* RESP rides in the ECG frame */
                    }
                    if (!(g_channel_mask & want)) {
                        /* configured to drop this channel */
                    } else {
                        append_frame(&f);
                        // if (k_uptime_get() - g_last_sync_ms >= (int64_t)REC_SYNC_PERIOD_MS) {
                        //     emit_sync();   /* periodic 5 s marker for recovery + .IDX */
                        // }
                        check_periodic();
                    }
                } else {
                    /* EVENT/SYNC */
                    append_frame(&f);
                    // if (k_uptime_get() - g_last_sync_ms >= (int64_t)REC_SYNC_PERIOD_MS) {
                    //     emit_sync();
                    // }
                    check_periodic();
                }
            }
        }
        k_mutex_unlock(&g_lock);
    }
}

K_THREAD_DEFINE(hpi_rec_tid, 4096, recording_thread, NULL, NULL, NULL,
                7 /* prio */, 0, 0);

// int hpi_recording_service_init(void)
// {
//     k_mutex_init(&g_lock);
//     LOG_INF("recording service ready (root %s)", REC_ROOT);
//     return 0;
// }
int hpi_recording_service_init(void)
{
    k_mutex_init(&g_lock);

    k_work_queue_init(&s_rec_wq);
    k_work_queue_start(&s_rec_wq, s_rec_wq_stack,
                       K_THREAD_STACK_SIZEOF(s_rec_wq_stack),
                       REC_WQ_PRIORITY, NULL);
    k_thread_name_set(&s_rec_wq.thread, "hpi_rec_wq");

    LOG_INF("recording service ready (root %s)", REC_ROOT);
    return 0;
}
