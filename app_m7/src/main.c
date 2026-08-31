/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * HealthyPi 6 -- M7 application entry: board bring-up (banner, watchdog,
 * FS-ready handshake), sample bus + zbus event plane init, then the service
 * start-up sequence. main() ends in a silent watchdog-feed loop.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/rtc.h>
#include <app_version.h>

#include "platform/watchdog.h"
#include "platform/health.h"
#include "control/security/hpi_security.h"
#include "services/connectivity/healthybridge_service.h"
#include "platform/fs_mount.h"
#include "core/sample_bus.h"
#include "core/acquisition.h"
#include "platform/ipc.h"
#include "bus/hpi_events.h"
#include "transport/usb_composite/usbd.h"
#include "services/stream_service.h"
#include "services/recording_service.h"
#include "services/power_service.h"
#include "services/config_service.h"
#include "services/button_service.h"
#include "healthylink/hl_provider.h"
#include "ui/ui_module.h"
#if defined(CONFIG_HPI_BUS_SELFTEST)
#include "core/bus_selftest.h"
#endif

LOG_MODULE_REGISTER(main, CONFIG_HPI_APP_LOG_LEVEL);

#define HEARTBEAT_PERIOD_MS 500

/* CDC0 stream sink: adapts the USB stream pipe to the stream service's sink
 * interface. Keeps the stream service transport-agnostic (sink injected here). */
static bool cdc0_sink_write(const void *buf, uint32_t len)
{
    return hpi_usb_stream_write((const uint8_t *)buf, (size_t)len);
}
static const struct hpi_stream_sink cdc0_stream_sink = {
    .name = "cdc0-hp6",
    .write = cdc0_sink_write,
    .is_connected = hpi_usb_host_connected,
};

/* Report WHY the SoC last reset, then clear the flags so the next boot is
 * unambiguous. IWDG and BOR reset the chip in hardware with no chance to log,
 * so the only evidence is the RCC reset flags -- and only until they are read.
 */
static void report_reset_cause(void)
{
    uint32_t cause = 0;

    if (hwinfo_get_reset_cause(&cause) != 0) {
        LOG_WRN("reset cause: unavailable on this target");
        return;
    }
    (void)hwinfo_clear_reset_cause();

    if (cause == 0) {
        LOG_INF("reset cause: none reported");
        return;
    }

    /* WATCHDOG first: it is the one that means the firmware stopped feeding
     * the IWDG for CONFIG_HPI_WATCHDOG_TIMEOUT_MS, i.e. a real hang, and it is
     * the one worth shouting about. */
    if (cause & RESET_WATCHDOG) {
        LOG_ERR("reset cause: WATCHDOG (IWDG) -- the firmware stopped feeding it "
                "for %d ms. Something blocked main() or a higher-priority thread "
                "spun.", CONFIG_HPI_WATCHDOG_TIMEOUT_MS);
    }
    if (cause & RESET_BROWNOUT) {
        LOG_ERR("reset cause: BROWNOUT -- the supply sagged below the BOR "
                "threshold. Check the battery, the USB supply and the 3V3 rail "
                "under load, not the firmware.");
    }
    if (cause & RESET_CPU_LOCKUP) {
        LOG_ERR("reset cause: CPU LOCKUP -- a fault escalated inside the fault "
                "handler itself.");
    }
    if (cause & RESET_SOFTWARE) {
        LOG_INF("reset cause: software (sys_reboot / a commanded reset)");
    }
    if (cause & RESET_PIN) {
        LOG_INF("reset cause: NRST pin (debugger, or the reset button)");
    }
    if (cause & RESET_POR) {
        LOG_INF("reset cause: power-on / BOR power-up (a normal cold boot)");
    }
    if (cause & RESET_LOW_POWER_WAKE) {
        LOG_INF("reset cause: low-power wake");
    }
    if (cause & RESET_DEBUG) {
        LOG_INF("reset cause: debugger");
    }

    /* Always print the raw word: the decoded lines above cover the causes this
     * SoC actually reports, and anything left over is worth seeing verbatim
     * rather than silently dropping. */
    LOG_INF("reset cause: raw flags 0x%08x", cause);
}

static void print_banner(void)
{
#if defined(CONFIG_HPI_APP_BANNER)
    uint8_t uid[12] = {0};
    ssize_t n = hwinfo_get_device_id(uid, sizeof(uid));

    LOG_INF("========================================");
    LOG_INF(" HealthyPi 6");
    LOG_INF("  version : %s", APP_VERSION_EXTENDED_STRING);
    LOG_INF("  board   : %s", CONFIG_BOARD_TARGET);
    LOG_INF("  build   : %s %s", __DATE__, __TIME__);
    if (n > 0) {
        LOG_INF("  uid     : %02x%02x%02x%02x%02x%02x%02x%02x",
                uid[0], uid[1], uid[2], uid[3],
                uid[4], uid[5], uid[6], uid[7]);
    }
#if defined(CONFIG_HPI_DEV_MODE)
    LOG_WRN("  flavor  : DEV (verbose, UART TX-only, no shell) -- do not ship");
#else
    LOG_INF("  flavor  : production");
#endif
    LOG_INF("========================================");
#endif
}

int main(void)
{
    print_banner();

    /* Before anything can reset the flags, and before the watchdog below is
     * re-armed. */
    report_reset_cause();

    /* Hardware watchdog first -- if anything below wedges, the IWDG resets
     * the SoC. Absence of a watchdog node is non-fatal during bring-up. */
    int rc = platform_watchdog_init();
    if (rc == 0) {
        LOG_INF("watchdog: armed");

    /* RTC status. The clock is LSE-backed and survives a reset, but the STM32
     * driver returns -ENODATA until the calendar has been set once (the INITS
     * flag), so a device that has never had its time set shows "--" in the
     * status bar for ever with nothing in the log to say why. Set it over the
     * stock MCUmgr os datetime group; VBAT then keeps it. */
    {
        const struct device *rtc_dev = DEVICE_DT_GET_OR_NULL(DT_ALIAS(rtc));
        struct rtc_time rt;

        if (rtc_dev == NULL || !device_is_ready(rtc_dev)) {
            LOG_WRN("rtc: not available; timestamps and the clock will be unset");
        } else if (rtc_get_time(rtc_dev, &rt) == 0) {
            LOG_INF("rtc: %04d-%02d-%02d %02d:%02d:%02d",
                    rt.tm_year + 1900, rt.tm_mon + 1, rt.tm_mday,
                    rt.tm_hour, rt.tm_min, rt.tm_sec);
        } else {
            LOG_WRN("rtc: never set -- clock shows '--' and recordings get "
                    "timestamp_start=0. Set it with the MCUmgr datetime command.");
        }
    }
    } else {
        LOG_WRN("watchdog: not active (%d)", rc);
    }

    /* Data bus (the spine) + event plane up before any producer/consumer. */
    (void)hpi_bus_init();
    hpi_events_publish(HPI_EVT_BUS_READY, 0);
#if defined(CONFIG_HPI_BUS_SELFTEST)
    hpi_bus_selftest_run();   /* boot-time proof the bus moves frames (dev) */
#endif

    /* Register subsystems with the health monitor before they start:
     * always-on producers heartbeat-armed, event-driven services explicit.
     * Must happen ahead of the UI so the boot self-test screen finds a
     * populated table the moment it starts polling. */
    hpi_health_register(HPI_SUBSYS_ACQ, "acq", 2000);
    hpi_health_register(HPI_SUBSYS_M4_IPC, "m4-ipc", 8000);
    hpi_health_register(HPI_SUBSYS_STREAM, "stream", 0);
    hpi_health_register(HPI_SUBSYS_RECORDING, "recording", 0);
    hpi_health_register(HPI_SUBSYS_HEALTHYLINK, "healthylink", 0);

    /* On-device LVGL UI -- a bus consumer + service client. No-op when
     * CONFIG_HPI_DISPLAY_ENABLED=n (headless build).
     *
     * Started EARLY, immediately after the bus, so the splash reaches the
     * panel while the rest of bring-up (notably the up-to-3 s FS-ready gate
     * below) is still running. Safe because the UI thread never assumes a
     * booted system: it paints the splash from LVGL alone, then polls
     * hpi_health_snapshot() with a max-timeout fallback. It runs at priority
     * 8, below acquisition and IPC, and everything it consumes -- the sample
     * bus and the health table -- is initialised above. */
    hpi_ui_init();

    /* Filesystem bring-up with the FS-ready handshake. A mount failure is
     * non-fatal -- we degrade to "SD not ready" instead of faulting.
     *
     * This blocks main() for up to CONFIG_HPI_FS_READY_TIMEOUT_MS (default
     * 3000) when no card is present. The UI is already up by now, so the wait
     * is spent showing the splash rather than a blank panel. */
    (void)platform_fs_mount_init();
    if (platform_fs_wait_ready(CONFIG_HPI_FS_READY_TIMEOUT_MS)) {
        LOG_INF("filesystem: ready");
        hpi_events_publish(HPI_EVT_FS_READY, 0);
    } else {
        LOG_WRN("filesystem: SD not ready (continuing in degraded mode)");
        hpi_events_publish(HPI_EVT_FS_NOT_READY, 0);
    }

    /* device security -- load the unlock secret + boot LOCKED. No-op
     * (always-unlocked) when CONFIG_HPI_SECURITY=n. */
    hpi_security_init();

    /* start onboard ECG/PPG acquisition -> sample bus. */
    (void)hpi_acquisition_init();

    /* start the M4 IPC feed (HR/SpO2/HRV -> bus). Non-blocking;
     * binds on its own thread (M4 delays ~7 s before it appears). */
    (void)hpi_ipc_init();

    /* bring up the USB composite (CDC0 stream + CDC1 MCUmgr). The
     * group-64 control plane self-registers via MCUMGR_HANDLER_DEFINE; SMP runs
     * on CDC1 through the stock uart_mcumgr transport. Headless host surface. */
    hpi_usb_start();

    /* wire the stream service to the CDC0 sink (sink-agnostic service;
     * main owns the transport binding). stream_start/stop on CDC1 control it. */
    hpi_stream_service_init(&cdc0_stream_sink);

    hpi_health_set(HPI_SUBSYS_STREAM, HPI_HEALTH_OK, NULL);

    /* recording service (bus -> .HP6 on SD). Controlled via group 64
     * sd_record_start/stop; writes only while active. */
    (void)hpi_recording_service_init();
    hpi_health_set(HPI_SUBSYS_RECORDING,
                   platform_fs_is_ready() ? HPI_HEALTH_OK : HPI_HEALTH_DEGRADED,
                   platform_fs_is_ready() ? NULL : "SD not ready");

    /* power/telemetry (battery monitor) -- feeds group-64 telemetry. */
    (void)hpi_power_service_init();

    /* config service -- load persistent settings from /lfs (QSPI). The
     * LittleFS is auto-mounted by the fstab node before main() runs. */
    (void)hpi_config_service_init();
    (void)hpi_button_service_init();

    /* HealthyLink module framework -- detect modules per slot, match a
     * registered provider, claim resources, and start under a supervisor. A
     * faulting module is quarantined; core acquisition is never disturbed. */
    (void)hl_framework_init();
    hpi_health_set(HPI_SUBSYS_HEALTHYLINK, HPI_HEALTH_OK, NULL);

    /* ESP32 connectivity (WiFi/BLE) -- a bus consumer that streams to the
     * ESP32-C6 over the HealthyBridge UART4 link. No-op when
     * CONFIG_HPI_CONNECTIVITY=n. */
    hpi_connectivity_init();

    /* start the health monitor now that every subsystem is registered. */
    hpi_health_start();

    LOG_INF("bring-up complete; entering watchdog-feed loop");

    /* Feed the watchdog silently; status is queried on demand via `hpi sys
     * info` / `hpi wdt`. CONFIG_HPI_HEARTBEAT_LOG=y re-enables a periodic
     * "alive" line. */
    while (1) {
        platform_watchdog_feed();
#if defined(CONFIG_HPI_HEARTBEAT_LOG)
        LOG_INF("alive: uptime %u s, fs=%s, wdt=%s",
                k_uptime_get_32() / 1000U,
                platform_fs_is_ready() ? "ready" : "down",
                platform_watchdog_active() ? "on" : "off");
        k_msleep(5000);
#else
        k_msleep(HEARTBEAT_PERIOD_MS);
#endif
    }

    return 0;
}
