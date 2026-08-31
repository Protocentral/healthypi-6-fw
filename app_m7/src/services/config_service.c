/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Config service -- persistent settings on /lfs (QSPI LittleFS). See
 * config_service.h. Keys: hpi/name, hpi/stream_ch, hpi/disp_bright.
 */

#include "config_service.h"

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdio.h>

LOG_MODULE_REGISTER(hpi_cfg, CONFIG_HPI_APP_LOG_LEVEL);

/* Cached config with defaults. */
static struct {
    char     device_name[32];
    uint32_t stream_channels;
    uint8_t  display_brightness;
    uint16_t display_sleep_s;  /* 0 = never */
} g_cfg = {
    .device_name = "HealthyPi 6",
    .stream_channels = 0x03,   /* ECG | PPG */
    .display_brightness = 80,
    .display_sleep_s = 60,
};

/* settings 'set' -- called for each stored "hpi/..." key during load + on host
 * writes via the MCUmgr settings group. */
static int cfg_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
    const char *next;

    if (settings_name_steq(name, "name", &next) && !next) {
        ssize_t rl = read_cb(cb_arg, g_cfg.device_name, sizeof(g_cfg.device_name) - 1);
        if (rl < 0) {
            return (int)rl;
        }
        g_cfg.device_name[rl] = '\0';
        return 0;
    }
    if (settings_name_steq(name, "stream_ch", &next) && !next) {
        if (len != sizeof(uint32_t)) {
            return -EINVAL;
        }
        return (read_cb(cb_arg, &g_cfg.stream_channels, sizeof(uint32_t)) < 0) ? -EIO : 0;
    }
    if (settings_name_steq(name, "disp_bright", &next) && !next) {
        if (len != sizeof(uint8_t)) {
            return -EINVAL;
        }
        return (read_cb(cb_arg, &g_cfg.display_brightness, sizeof(uint8_t)) < 0) ? -EIO : 0;
    }
    if (settings_name_steq(name, "disp_sleep", &next) && !next) {
        if (len != sizeof(uint16_t)) {
            return -EINVAL;
        }
        return (read_cb(cb_arg, &g_cfg.display_sleep_s, sizeof(uint16_t)) < 0) ? -EIO : 0;
    }
    return -ENOENT;
}

/* settings 'get' -- lets the host read current values (runtime get). */
static int cfg_get(const char *name, char *val, int val_len_max)
{
    const char *next;

    if (settings_name_steq(name, "name", &next) && !next) {
        size_t n = strlen(g_cfg.device_name) + 1;
        if ((size_t)val_len_max < n) {
            return -ENOMEM;
        }
        memcpy(val, g_cfg.device_name, n);
        return (int)n;
    }
    if (settings_name_steq(name, "stream_ch", &next) && !next) {
        if (val_len_max < (int)sizeof(uint32_t)) {
            return -ENOMEM;
        }
        memcpy(val, &g_cfg.stream_channels, sizeof(uint32_t));
        return sizeof(uint32_t);
    }
    if (settings_name_steq(name, "disp_bright", &next) && !next) {
        if (val_len_max < (int)sizeof(uint8_t)) {
            return -ENOMEM;
        }
        memcpy(val, &g_cfg.display_brightness, sizeof(uint8_t));
        return sizeof(uint8_t);
    }
    if (settings_name_steq(name, "disp_sleep", &next) && !next) {
        if (val_len_max < (int)sizeof(uint16_t)) {
            return -ENOMEM;
        }
        memcpy(val, &g_cfg.display_sleep_s, sizeof(uint16_t));
        return sizeof(uint16_t);
    }
    return -ENOENT;
}

/* settings 'export' -- used by settings_save() to persist the whole subtree. */
static int cfg_export(int (*cb)(const char *name, const void *value, size_t val_len))
{
    cb("hpi/name", g_cfg.device_name, strlen(g_cfg.device_name) + 1);
    cb("hpi/stream_ch", &g_cfg.stream_channels, sizeof(uint32_t));
    cb("hpi/disp_bright", &g_cfg.display_brightness, sizeof(uint8_t));
    cb("hpi/disp_sleep", &g_cfg.display_sleep_s, sizeof(uint16_t));
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(hpi_cfg, "hpi", cfg_get, cfg_set, NULL, cfg_export);

/* Deferred, coalesced brightness persist (see config_service.h): each save is
 * a QSPI erase/program plus, past CONFIG_SETTINGS_FILE_MAX_LINES, a full
 * settings-file compaction, and must never run on the caller -- the LVGL UI
 * thread. The work re-reads the cache when it fires, so a whole slider drag
 * collapses into one write of the final value.
 *
 * Its own queue, not the system one: the system workqueue is where the GT911
 * reports touch, so parking a flash write there would break input. */
#define CFG_SAVE_DEBOUNCE_MS  750
#define CFG_WQ_STACK          3072   /* settings + littlefs + QSPI driver */
#define CFG_WQ_PRIO           10     /* below the UI and every acquisition path */

static K_THREAD_STACK_DEFINE(cfg_wq_stack, CFG_WQ_STACK);
static struct k_work_q cfg_wq;
static bool cfg_wq_ready;

static void cfg_bright_save_work(struct k_work *w)
{
    ARG_UNUSED(w);
    uint8_t b = g_cfg.display_brightness;
    int rc = settings_save_one("hpi/disp_bright", &b, sizeof(b));

    if (rc != 0) {
        LOG_WRN("brightness persist failed (%d)", rc);
    }
}

static void cfg_sleep_save_work(struct k_work *w)
{
    ARG_UNUSED(w);
    uint16_t s = g_cfg.display_sleep_s;
    int rc = settings_save_one("hpi/disp_sleep", &s, sizeof(s));

    if (rc != 0) {
        LOG_WRN("display-sleep persist failed (%d)", rc);
    }
}

static K_WORK_DELAYABLE_DEFINE(cfg_bright_save, cfg_bright_save_work);
static K_WORK_DELAYABLE_DEFINE(cfg_sleep_save, cfg_sleep_save_work);

int hpi_config_service_init(void)
{
    int rc = settings_subsys_init();
    if (rc != 0 && rc != -EALREADY) {
        LOG_WRN("settings_subsys_init failed (%d) -- using defaults", rc);
        return rc;
    }
    rc = settings_load_subtree("hpi");
    if (rc != 0) {
        LOG_WRN("settings load failed (%d) -- using defaults", rc);
    }

    /* Seed the settings file with current defaults on first boot. Without this
     * the file doesn't exist yet, so every boot logs a harmless `fs open -2`
     * (ENOENT); creating it also proves the write path works. */
    struct fs_dirent ev;
    if (fs_stat(CONFIG_SETTINGS_FILE_PATH, &ev) != 0) {
        int sv = settings_save();
        LOG_INF("config: seeded %s (%d)", CONFIG_SETTINGS_FILE_PATH, sv);
    }

    k_work_queue_start(&cfg_wq, cfg_wq_stack, K_THREAD_STACK_SIZEOF(cfg_wq_stack),
                       CFG_WQ_PRIO, &(struct k_work_queue_config){ .name = "hpi_cfg" });
    cfg_wq_ready = true;

    LOG_INF("config: name='%s' stream_ch=0x%02x bright=%u",
            g_cfg.device_name, g_cfg.stream_channels, g_cfg.display_brightness);
    return 0;
}

uint32_t hpi_config_stream_channels(void) { return g_cfg.stream_channels; }
uint8_t  hpi_config_display_brightness(void) { return g_cfg.display_brightness; }
uint16_t hpi_config_display_sleep_s(void) { return g_cfg.display_sleep_s; }
const char *hpi_config_device_name(void) { return g_cfg.device_name; }

int hpi_config_set_stream_channels(uint32_t ch)
{
    g_cfg.stream_channels = ch;
    return settings_save_one("hpi/stream_ch", &g_cfg.stream_channels, sizeof(uint32_t));
}

int hpi_config_set_display_brightness(uint8_t b)
{
    g_cfg.display_brightness = b;
    if (!cfg_wq_ready) {
        /* Before the service is up (or if the queue failed to start) fall back
         * to the inline write rather than dropping the value silently. */
        return settings_save_one("hpi/disp_bright", &b, sizeof(uint8_t));
    }
    k_work_reschedule_for_queue(&cfg_wq, &cfg_bright_save, K_MSEC(CFG_SAVE_DEBOUNCE_MS));
    return 0;
}

int hpi_config_set_display_sleep_s(uint16_t s)
{
    g_cfg.display_sleep_s = s;
    if (!cfg_wq_ready) {
        return settings_save_one("hpi/disp_sleep", &s, sizeof(s));
    }
    k_work_reschedule_for_queue(&cfg_wq, &cfg_sleep_save, K_MSEC(CFG_SAVE_DEBOUNCE_MS));
    return 0;
}

int hpi_config_set_device_name(const char *name)
{
    if (name == NULL) {
        return -EINVAL;
    }
    strncpy(g_cfg.device_name, name, sizeof(g_cfg.device_name) - 1);
    g_cfg.device_name[sizeof(g_cfg.device_name) - 1] = '\0';
    return settings_save_one("hpi/name", g_cfg.device_name,
                             strlen(g_cfg.device_name) + 1);
}
