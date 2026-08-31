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
#include <zephyr/sys/crc.h>
#include <zephyr/sys/timeutil.h>
#include <zephyr/logging/log.h>
#include <app_version.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

LOG_MODULE_REGISTER(hpi_rec, CONFIG_HPI_APP_LOG_LEVEL);

#define REC_ROOT       "/SD:/HPI6/REC"
#define REC_WRITE_BUF  4096

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
#define REC_SYNC_PERIOD_MS 5000u
#define HP6_SYNC_MAGIC 0xDEADBEEFu

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

/* ---- state ---- */
static struct hpi_bus_sub *g_sub;
static volatile bool       g_active;
static struct fs_file_t    g_file;
static struct recording_header g_hdr;
static uint8_t  g_wbuf[REC_WRITE_BUF];
static uint32_t g_wlen;
static uint32_t g_bytes;
static uint64_t g_start_ms;          /* device-monotonic ms at start */
static char     g_path[64];
static struct k_mutex g_lock;
static uint32_t g_undated_seq;       /* fallback filename counter */

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
    (void)fs_mkdir(path);
}

/* Best-effort mkdir -p, starting BELOW the "/SD:" mount point (you can't mkdir
 * the volume root -- that produced the -ENOENT noise in the log). */
static void mkdir_p(const char *dir)
{
    char tmp[64];
    strncpy(tmp, dir, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    /* Skip the mount point: advance past the '/' that follows "/SD:". */
    char *start = strchr(tmp + 1, '/');
    if (start == NULL) {
        return;   /* nothing below the volume to create */
    }
    for (char *p = start + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir_one(tmp);
            *p = '/';
        }
    }
    mkdir_one(tmp);
}

/* Build the dated path + create dirs. Uses the RTC if available. */
static void build_path(void)
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
        snprintf(file, sizeof(file), "%s/%02d%02d%02d.HP6",
                 daydir, t.tm_hour, t.tm_min, t.tm_sec);
    } else {
        snprintf(daydir, sizeof(daydir), REC_ROOT "/UNDATED");
        mkdir_p(daydir);
        snprintf(file, sizeof(file), "%s/UPT_%06u.HP6", daydir,
                 (unsigned)(++g_undated_seq));
    }
    strncpy(g_path, file, sizeof(g_path) - 1);
    g_path[sizeof(g_path) - 1] = '\0';
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
    g_hdr.channels = HPI_CH_BIT(HPI_CH_ECG) | HPI_CH_BIT(HPI_CH_PPG) |
                     HPI_CH_BIT(HPI_CH_VITALS);
    g_hdr.rate_hz[HP6_HDR_SLOT(HPI_CH_ECG)]    = sys_cpu_to_le16(HPI_ECG_RATE_HZ);
    g_hdr.rate_hz[HP6_HDR_SLOT(HPI_CH_PPG)]    = sys_cpu_to_le16(HPI_PPG_RATE_HZ);
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

static int flush_buf(void)
{
    if (g_wlen == 0) {
        return 0;
    }
    ssize_t w = fs_write(&g_file, g_wbuf, g_wlen);
    if (w < 0) {
        LOG_ERR("fs_write failed (%d)", (int)w);
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
    /* DBLK t_ms is milliseconds since DEVICE BOOT, of the first sample in the
     * block -- docs/HP6_DATA_FORMAT.md ss3, and the same value the live stream
     * writes, so one parser reads both. It used to be session-relative
     * (`- g_start_ms`), which was both off-spec and underflowed to 0xFFFFFFFD
     * on the first frame of a recording, that frame predating g_start_ms.
     * Wall-clock still comes from the header's timestamp_start and the sync
     * markers, exactly as the spec describes. */
    uint64_t t_ms = f->t_mono_us / 1000ULL;
    /* Where this block will land. Captured BEFORE the append because it can
     * flush mid-call; g_bytes + g_wlen is invariant across a flush. */
    uint64_t rec_off = g_bytes + g_wlen;

    append_dblk((uint8_t)f->channel, (uint8_t)f->flags, t_ms,
                f->sample_count, f->payload, f->len);

    switch (f->channel) {
    case HPI_CH_ECG:
    case HPI_CH_PPG:
    case HPI_CH_RESP:
    case HPI_CH_VITALS:
    case HPI_CH_EEG:
    case HPI_CH_INFER:
        g_hdr.sample_count[HP6_HDR_SLOT(f->channel)] += f->sample_count;
        g_hdr.channels |= HPI_CH_BIT(f->channel);
        break;
    case HPI_CH_EVENT:
        /* The first event fixes events_offset, so a reader can jump to the
         * first marker instead of walking the file. Both fields are patched
         * into the header at finalize. */
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
    strncpy(g_idx_path, g_path, sizeof(g_idx_path) - 1);
    g_idx_path[sizeof(g_idx_path) - 1] = '\0';
    char *dot = strrchr(g_idx_path, '.');
    if (dot) {
        strncpy(dot, ".IDX", 5);   /* replace ".HP6" */
    }
    fs_file_t_init(&g_idx);
    if (fs_open(&g_idx, g_idx_path, FS_O_CREATE | FS_O_WRITE) != 0) {
        LOG_WRN("idx open failed: %s", g_idx_path);
        return;
    }
    uint8_t h[12];
    memcpy(h, "HP6I", 4);
    sys_put_le16(0x0300, &h[4]);   /* version -- tracks the .HP6 container */
    sys_put_le16(0, &h[6]);        /* event_count (no events yet) */
    sys_put_le32(0, &h[8]);        /* sync_count (filled at finalize) */
    (void)fs_write(&g_idx, h, sizeof(h));
    g_idx_open = true;
}

static void idx_append_sync(uint32_t ts_ms, uint64_t file_off, uint32_t seq, uint32_t crc)
{
    if (!g_idx_open) {
        return;
    }
    uint8_t e[20];
    sys_put_le32(ts_ms, &e[0]);
    sys_put_le64(file_off, &e[4]);
    sys_put_le32(seq, &e[12]);
    sys_put_le32(crc, &e[16]);
    (void)fs_write(&g_idx, e, sizeof(e));
}

static void idx_finalize(void)
{
    if (!g_idx_open) {
        return;
    }
    /* Patch sync_count at offset 8, then append a whole-file CRC footer. */
    if (fs_seek(&g_idx, 8, FS_SEEK_SET) == 0) {
        uint8_t c[4];
        sys_put_le32(g_sync_count, c);
        (void)fs_write(&g_idx, c, 4);
    }
    (void)fs_seek(&g_idx, 0, FS_SEEK_END);
    uint8_t foot[4];
    sys_put_le32(0u, foot);   /* footer_crc32 placeholder (full-IDX CRC TODO) */
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
    char buf[512];   /* 320 truncated mid-line: `Bytes:` came out empty */
    int n = snprintf(buf, sizeof(buf),
        "HealthyPi 6 Recording\r\n"
        "Format:    HPI6 v3.0 (.HP6)\r\n"
        "File:      %s\r\n"
        "Firmware:  %s   Board: %s\r\n"
        "Serial:    %.16s\r\n"
        "Session:   %s\r\n"
        "Channels:  ECG %u Hz, PPG %u Hz, Vitals %u Hz\r\n"
        "Status:    %s\r\n"
        "Duration:  %u ms\r\n"
        "Samples:   ECG %u, PPG %u, Vitals %u\r\n"
        "Bytes:     %u\r\n",
        g_path, g_hdr.firmware_version, g_hdr.board_variant, g_hdr.serial_number,
        g_hdr.session_name[0] ? g_hdr.session_name : "(unnamed)",
        HPI_ECG_RATE_HZ, HPI_PPG_RATE_HZ, 1u,
        final ? "complete" : "recording...",
        final ? (uint32_t)((uint64_t)k_uptime_get() - g_start_ms) : 0u,
        g_hdr.sample_count[HP6_HDR_SLOT(HPI_CH_ECG)],
        g_hdr.sample_count[HP6_HDR_SLOT(HPI_CH_PPG)],
        g_hdr.sample_count[HP6_HDR_SLOT(HPI_CH_VITALS)], g_bytes);
    if (n > 0) {
        (void)fs_write(&f, buf, (size_t)MIN(n, (int)sizeof(buf)));
    }
    fs_sync(&f);
    fs_close(&f);
}

/* Write a sync marker into the .HP6 and mirror it in the .IDX. */
static void emit_sync(void)
{
    (void)flush_buf();                 /* so file_off points at the sync frame */
    uint64_t file_off = g_bytes;
    uint32_t ts_ms = (uint32_t)((uint64_t)k_uptime_get() - g_start_ms);

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
}

/* ---- public API ---- */

int hpi_recording_start(const char *session_name)
{
    k_mutex_lock(&g_lock, K_FOREVER);
    int rc = 0;

    if (g_active) { rc = -EBUSY; goto out; }
    /* Writer thread must have a live bus subscription, else we'd write only the
     * header and capture nothing (e.g. if the ring alloc failed at boot). */
    if (g_sub == NULL) {
        LOG_ERR("recording: no bus subscription (writer not ready); aborting start");
        rc = -ENODEV; goto out;
    }
    /* When USB Transfer Mode is armed the FS is unmounted, so this also blocks
     * recording while the host owns the SD. */
    if (!platform_fs_is_ready()) { rc = -ENODEV; goto out; }

    mkdir_p(REC_ROOT);
    build_path();
    g_wall_start_ms = rtc_unix_ms();
    header_init(session_name);

    fs_file_t_init(&g_file);
    rc = fs_open(&g_file, g_path, FS_O_CREATE | FS_O_WRITE);
    if (rc != 0) {
        LOG_ERR("fs_open %s failed (%d)", g_path, rc);
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
    g_running_crc = 0;
    g_last_sync_ms = k_uptime_get();
    idx_open();
    txt_write(false);
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
    (void)flush_buf();
    txt_write(true);        /* uses live counters before they're byte-swapped */
    idx_finalize();

    /* Finalize header: end time, duration, counters, fresh CRC. */
    uint32_t dur_ms = (uint32_t)((uint64_t)k_uptime_get() - g_start_ms);
    g_hdr.duration_ms = sys_cpu_to_le32(dur_ms);
    g_hdr.timestamp_end = sys_cpu_to_le64(
        g_wall_start_ms ? (g_wall_start_ms + dur_ms) : 0);
    for (int i = 0; i < HP6_HDR_CHANNEL_SLOTS; i++) {
        g_hdr.sample_count[i] = sys_cpu_to_le32(g_hdr.sample_count[i]);
    }
    g_hdr.channels = sys_cpu_to_le32(g_hdr.channels);
    g_hdr.event_count = sys_cpu_to_le32(g_hdr.event_count);
    g_hdr.events_offset = sys_cpu_to_le64(g_hdr.events_offset);
    g_hdr.header_crc32 = sys_cpu_to_le32(crc32_ieee((const uint8_t *)&g_hdr, 248));

    if (fs_seek(&g_file, 0, FS_SEEK_SET) == 0) {
        (void)fs_write(&g_file, &g_hdr, sizeof(g_hdr));
    }
    fs_sync(&g_file);
    fs_close(&g_file);
    LOG_INF("recording stopped: %s (%u bytes, %u syncs)", g_path, g_bytes,
            g_sync_count);

out:
    k_mutex_unlock(&g_lock);
    return rc;
}

int hpi_recording_mark(void)
{
    if (!g_active) {
        return -EACCES;
    }

    /* Sequence is per session and lives here so every producer of a mark -- the
     * on-screen button today, a hardware short-press later -- shares one
     * numbering. Published rather than written directly so the marker reaches
     * the stream and the ESP32 link too, and so it is ordered against the
     * samples by the same ring that carries them. */
    static atomic_t seq;
    uint16_t n = (uint16_t)atomic_inc(&seq) + 1;
    struct hp6_event ev = {
        .ts_ms = (uint32_t)((uint64_t)k_uptime_get() - g_start_ms),
        .type = HP6_EVENT_USER_MARK,
        .seq = n,
    };
    struct hpi_sample_frame f = {
        .channel = HPI_CH_EVENT,
        .sample_count = 1,
        .len = sizeof(ev),
        .t_mono_us = (uint64_t)k_uptime_get() * 1000ULL,
        .payload = &ev,
    };

    int rc = hpi_bus_publish(&f);

    if (rc != 0) {
        LOG_WRN("mark %u dropped at publish (%d)", n, rc);
        return rc;
    }
    LOG_INF("mark %u at %u ms", n, ev.ts_ms);
    return (int)n;
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
    out->bytes_written = g_bytes;
    out->duration_ms = g_active
        ? (uint32_t)((uint64_t)k_uptime_get() - g_start_ms) : 0;
    out->ecg_samples = g_hdr.sample_count[HP6_HDR_SLOT(HPI_CH_ECG)];
    out->ppg_samples = g_hdr.sample_count[HP6_HDR_SLOT(HPI_CH_PPG)];
    out->vitals_samples = g_hdr.sample_count[HP6_HDR_SLOT(HPI_CH_VITALS)];
    strncpy(out->path, g_path, sizeof(out->path) - 1);
    out->path[sizeof(out->path) - 1] = '\0';
}

/* ---- writer thread ---- */

static void recording_thread(void *a, void *b, void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    struct hpi_bus_sub_cfg cfg = {
        .name = "rec",
        .channel_mask = HPI_CH_BIT(HPI_CH_ECG) | HPI_CH_BIT(HPI_CH_PPG) |
                        HPI_CH_BIT(HPI_CH_VITALS) | HPI_CH_BIT(HPI_CH_EEG) |
                        HPI_CH_BIT(HPI_CH_EVENT) | HPI_CH_BIT(HPI_CH_INFER),
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
        if (!g_active) {
            continue;   /* drain + discard while idle */
        }
        k_mutex_lock(&g_lock, K_FOREVER);
        if (g_active) {
            append_frame(&f);
            if (k_uptime_get() - g_last_sync_ms >= (int64_t)REC_SYNC_PERIOD_MS) {
                emit_sync();   /* periodic 5 s marker for recovery + .IDX */
            }
        }
        k_mutex_unlock(&g_lock);
    }
}

K_THREAD_DEFINE(hpi_rec_tid, 4096, recording_thread, NULL, NULL, NULL,
                7 /* prio */, 0, 0);

int hpi_recording_service_init(void)
{
    k_mutex_init(&g_lock);
    LOG_INF("recording service ready (root %s)", REC_ROOT);
    return 0;
}
