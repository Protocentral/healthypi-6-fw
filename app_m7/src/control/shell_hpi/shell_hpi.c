/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * HealthyPi shell adapter (control plane, dev/factory only).
 *
 * Compiled only when CONFIG_SHELL=y — never in the customer image.
 * Programmatic access uses MCUmgr (CDC 1); these commands are a developer
 * convenience with no compatibility guarantee. Adapter-parity with MCUmgr
 * group 64: each verb is a thin wrapper over an L4 service or platform call.
 *
 * `hpi` command tree:
 *
 *   hpi sys info | ver | reboot            [LIVE]
 *   hpi fs                                  [LIVE]
 *   hpi wdt                                 [LIVE]
 *   hpi stream start|stop|status            [-> stream_service]
 *   hpi rec start|stop|ls|rm                [-> recording_service]
 *   hpi mod list|info|power                 [-> healthylink]
 *   hpi sd status|get|transfer              [-> recording/MSC]
 *   hpi diag selftest|leadoff|stats         [-> diag_service]
 *   hpi update                              [-> MCUboot]
 *   hpi wifi status|softap|on|off           [LIVE -> connectivity]
 *   hpi ble  on|off                         [LIVE — co-processor advertising]
 *   hpi link status|on|off                  [LIVE — co-processor power]
 *
 * Placeholders are registered so the full surface is discoverable via
 * `hpi help` / tab-completion; each prints which step brings it online.
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/fs/fs.h>
#include <app_version.h>

#include "platform/watchdog.h"
#include "platform/fs_mount.h"
#include "services/connectivity/healthybridge_service.h"

/* ---- LIVE: system ---- */

static int cmd_sys_info(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);
    uint8_t uid[12] = {0};
    ssize_t n = hwinfo_get_device_id(uid, sizeof(uid));

    shell_print(sh, "version : %s", APP_VERSION_EXTENDED_STRING);
    shell_print(sh, "board   : %s", CONFIG_BOARD_TARGET);
    shell_print(sh, "build   : %s %s", __DATE__, __TIME__);
    if (n > 0) {
        shell_fprintf(sh, SHELL_NORMAL, "uid     : ");
        for (ssize_t i = 0; i < n; i++) {
            shell_fprintf(sh, SHELL_NORMAL, "%02x", uid[i]);
        }
        shell_print(sh, "");
    }
    shell_print(sh, "uptime  : %u s", k_uptime_get_32() / 1000U);
#if defined(CONFIG_HPI_DEV_MODE)
    shell_print(sh, "flavor  : dev");
#else
    shell_print(sh, "flavor  : production");
#endif
    return 0;
}

static int cmd_sys_ver(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);
    shell_print(sh, "%s", APP_VERSION_EXTENDED_STRING);
    return 0;
}

static int cmd_sys_reboot(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);
    shell_print(sh, "rebooting...");
    k_msleep(50);
    sys_reboot(SYS_REBOOT_COLD);
    return 0;
}

/* ---- LIVE: filesystem ---- */

static int cmd_fs(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);
    if (!platform_fs_is_ready()) {
        shell_print(sh, "fs: SD not ready");
        return 0;
    }
    struct fs_statvfs s;
    int rc = fs_statvfs("/SD:", &s);
    if (rc != 0) {
        shell_print(sh, "fs: ready, statvfs failed (%d)", rc);
        return 0;
    }
    uint32_t total_mb = (uint32_t)((uint64_t)s.f_frsize * s.f_blocks >> 20);
    uint32_t free_mb  = (uint32_t)((uint64_t)s.f_frsize * s.f_bfree  >> 20);
    shell_print(sh, "fs: ready  /SD:  %u MB total, %u MB free", total_mb, free_mb);
    return 0;
}

/* ---- LIVE: watchdog ---- */

static int cmd_wdt(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);
    shell_print(sh, "wdt: %s (timeout %d ms)",
                platform_watchdog_active() ? "armed" : "inactive",
                CONFIG_HPI_WATCHDOG_TIMEOUT_MS);
    return 0;
}

/* ---- LIVE: WiFi (thin adapter over the connectivity service) ----
 *
 * Parity with MCUmgr group 64 0x0070-0x0073; no logic here, the service owns
 * it. `softap` opens the ESP32 captive portal, which is the only supported way
 * to provision credentials -- they are never pushed over the host link, so
 * there is deliberately no `connect` verb.
 */

static int cmd_wifi_status(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);
    struct hpi_wifi_info wi = {0};
    int rc = hpi_connectivity_wifi_status(&wi);

    if (rc == -ENODATA) {
        shell_print(sh, "wifi: link up, peer reports no status payload");
        return 0;
    }
    if (rc != 0) {
        shell_error(sh, "wifi: status failed (%d)", rc);
        return rc;
    }
    shell_print(sh, "wifi: state=%u rssi=%ddBm ip=%u.%u.%u.%u ssid='%s'",
                wi.state, wi.rssi, wi.ip[0], wi.ip[1], wi.ip[2], wi.ip[3], wi.ssid);
    return 0;
}

static int cmd_wifi_softap(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);
    int rc = hpi_connectivity_wifi_softap();

    shell_print(sh, "wifi: softap %s (%d)", rc == 0 ? "requested" : "FAILED", rc);
    return rc;
}

static int cmd_wifi_on(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);
    int rc = hpi_connectivity_wifi_enable(true);

    shell_print(sh, "wifi: enable %s (%d)", rc == 0 ? "requested" : "FAILED", rc);
    return rc;
}

static int cmd_ble_on(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);
    int rc = hpi_connectivity_ble_enable(true);

    shell_print(sh, "ble: enable %s (%d)", rc == 0 ? "requested" : "FAILED", rc);
    return rc == 0 ? 0 : -EIO;
}

static int cmd_ble_off(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);
    int rc = hpi_connectivity_ble_enable(false);

    shell_print(sh, "ble: disable %s (%d)", rc == 0 ? "requested" : "FAILED", rc);
    return rc == 0 ? 0 : -EIO;
}

static const char *link_name(uint8_t s)
{
    switch (s) {
    case HPI_CONN_LINK_OFF:      return "off";
    case HPI_CONN_LINK_STARTING: return "starting";
    case HPI_CONN_LINK_UP:       return "up";
    case HPI_CONN_LINK_FAULT:    return "fault";
    default:                     return "unknown";
    }
}

static int cmd_link_status(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);
    struct hpi_conn_status st;

    hpi_connectivity_get_status(&st);
    shell_print(sh, "link: %s | radios:%s%s | wifi state=%u rssi=%ddBm ssid='%s'"
                    " ip=%u.%u.%u.%u | ble adv=%d conn=%d",
                link_name(st.link_state),
                (st.radios & HPI_CONN_RADIO_WIFI) ? " wifi" : "",
                (st.radios & HPI_CONN_RADIO_BLE) ? " ble" : "",
                st.wifi_state, st.rssi, st.ssid,
                st.ip[0], st.ip[1], st.ip[2], st.ip[3],
                (int)st.ble_adv, (int)st.ble_conn);
    return 0;
}

/*
 * Power the co-processor with NO radio. This is the escape hatch for flashing
 * it: the M7 holds its EN line low by default, and a C6 in reset does not
 * enumerate its USB-Serial/JTAG port, so `idf.py flash` on an assembled unit
 * fails in a way that looks like broken hardware until you know this exists.
 */
static int cmd_link_on(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);
    int rc = hpi_connectivity_enable(0);

    shell_print(sh, "link: power up %s (%d) -- no radios started",
                rc == 0 ? "requested" : "FAILED", rc);
    return rc == 0 ? 0 : -EIO;
}

static int cmd_link_off(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);
    hpi_connectivity_disable();
    shell_print(sh, "link: power down requested");
    return 0;
}

static int cmd_wifi_off(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);
    int rc = hpi_connectivity_wifi_enable(false);

    shell_print(sh, "wifi: disable %s (%d)", rc == 0 ? "requested" : "FAILED", rc);
    return rc;
}

/* ---- Placeholders for verbs whose service lands in a later step ---- */

static int cmd_todo(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    shell_warn(sh, "'%s' is not available yet in this build", argv[0]);
    return -ENOTSUP;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_sys,
    SHELL_CMD(info,   NULL, "Device info (version, board, uid, uptime)", cmd_sys_info),
    SHELL_CMD(ver,    NULL, "Firmware version", cmd_sys_ver),
    SHELL_CMD(reboot, NULL, "Reboot the device", cmd_sys_reboot),
    SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_wifi,
    SHELL_CMD(status, NULL, "WiFi/link status from the co-processor", cmd_wifi_status),
    SHELL_CMD(softap, NULL, "Open the SoftAP captive portal (provisioning)", cmd_wifi_softap),
    SHELL_CMD(on,     NULL, "Enable WiFi STA using stored credentials", cmd_wifi_on),
    SHELL_CMD(off,    NULL, "Disable WiFi (stops STA or SoftAP)", cmd_wifi_off),
    SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_ble,
    SHELL_CMD(on,  NULL, "Start BLE advertising on the co-processor", cmd_ble_on),
    SHELL_CMD(off, NULL, "Stop BLE advertising", cmd_ble_off),
    SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_link,
    SHELL_CMD(status, NULL, "Co-processor link state and radios", cmd_link_status),
    SHELL_CMD(on,     NULL, "Power the co-processor, no radios (flashing/bench)", cmd_link_on),
    SHELL_CMD(off,    NULL, "Radios down and co-processor into reset", cmd_link_off),
    SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_hpi,
    SHELL_CMD(sys,    &sub_sys, "System info / reboot", NULL),
    SHELL_CMD(fs,     NULL,     "Filesystem status", cmd_fs),
    SHELL_CMD(wdt,    NULL,     "Watchdog status", cmd_wdt),
    SHELL_CMD(stream, NULL,     "Stream control", cmd_todo),
    SHELL_CMD(rec,    NULL,     "Recording control", cmd_todo),
    SHELL_CMD(mod,    NULL,     "HealthyLink modules", cmd_todo),
    SHELL_CMD(sd,     NULL,     "SD card / transfer mode", cmd_todo),
    SHELL_CMD(diag,   NULL,     "Diagnostics / self-test", cmd_todo),
    SHELL_CMD(update, NULL,     "Firmware update", cmd_todo),
    SHELL_CMD(wifi,   &sub_wifi, "WiFi control via the ESP32 co-processor", NULL),
    SHELL_CMD(ble,    &sub_ble,  "BLE advertising on the ESP32 co-processor", NULL),
    SHELL_CMD(link,   &sub_link, "ESP32 co-processor power / link state", NULL),
    SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(hpi, &sub_hpi, "HealthyPi 6 device commands", NULL);
