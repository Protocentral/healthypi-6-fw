# Copyright (c) 2024-2026 Protocentral Electronics
# SPDX-License-Identifier: MIT
#
# HealthyPi 6 — M7 application.
# Sources are listed EXPLICITLY (no blind GLOB) so adding a file is a
# deliberate act and the dependency layering stays auditable.

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(healthypi6_m7 LANGUAGES C CXX)

target_include_directories(app PRIVATE src)

# declare the linker iterable section backing HL_MODULE_REGISTER
# (hl_provider.h). Without it, STRUCT_SECTION_FOREACH(hl_module_provider, ...)
# finds zero providers. This board uses the GNU ld template linker
# (CONFIG_CMAKE_LINKER_GENERATOR=n), so the section comes from an .ld fragment.
#
# The fragment uses ITERABLE_SECTION_RAM (the providers are non-const .data:
# RAM VMA + ROM LMA), which MUST be inserted via the DATA_SECTIONS group. Under
# the generic SECTIONS group it links (start/end symbols exist) but the .data
# providers land in the normal .data region instead of this section, leaving
# _hl_module_provider_list_start == _end -> empty registry ("no provider").
zephyr_linker_sources(DATA_SECTIONS hl_iterables.ld)

# --- build system + board bring-up ---
target_sources(app PRIVATE
    src/main.c
    src/platform/watchdog.c
    src/platform/fs_mount.c
    src/platform/health.c   # subsystem health/heartbeat monitor
)

# --- data bus (the spine) + event plane ---
# core/frame_ring.h is header-only. It has NO host tests -- this comment used
# to claim `tests/host/`, a directory that has never existed in this repo.
target_sources(app PRIVATE
    src/core/sample_bus.c
    src/bus/hpi_events.c
)
if(CONFIG_HPI_BUS_SELFTEST)
    target_sources(app PRIVATE src/core/bus_selftest.c)
endif()

# --- onboard acquisition producers (ECG/PPG -> bus) ---
target_sources(app PRIVATE
    src/core/acquisition.c
)

# --- M4 IPC vitals feed -> bus ---
target_sources(app PRIVATE
    src/platform/ipc.c
)

# --- transport (USB composite) + control (MCUmgr group 64) ---
# CDC0 = .HP6 stream pipe; CDC1 = MCUmgr SMP (stock uart_mcumgr, bound in DT).
# Group 64 device_info is the first headless gate; more handlers land in 6b.
target_sources(app PRIVATE
    src/transport/usb_composite/usbd.c
    src/control/mcumgr_hpi/hpi_mgmt_group.c
    src/control/mcumgr_hpi/hpi_system.c
    src/control/mcumgr_hpi/hpi_datetime_hook.c
)

# --- device lock/unlock state machine ---
target_sources(app PRIVATE src/control/security/hpi_security.c)
target_sources_ifdef(CONFIG_HPI_SECURITY app PRIVATE
    src/control/mcumgr_hpi/hpi_security_cmd.c
)

# --- LVGL UI on the bus (Material 3, modular) ---
# ui_module.c always compiles (self-stubs when the display is off); the M3
# components + screens compile only with the display flavor.
target_sources(app PRIVATE src/ui/ui_module.c)
target_sources_ifdef(CONFIG_HPI_DISPLAY_ENABLED app PRIVATE
    src/ui/components/hpi_ui_components.c
    src/ui/screens/scr_home.c
    src/ui/screens/scr_live.c
    src/ui/screens/scr_secondary.c
    src/ui/screens/scr_more.c
    src/ui/screens/scr_boot.c
    src/ui/screens/scr_trends.c
    src/ui/screens/scr_link.c
    src/ui/screens/scr_settings.c
    src/ui/screens/scr_ambient.c
    src/ui/screens/scr_power.c
    # Brand lockup (generated from the design artwork by ui/assets/convert_logo.py)
    src/ui/assets/hpi_logo.c
    # Redesign fonts (generated from the real design TTFs by ui/fonts/convert_fonts.sh)
    src/ui/fonts/lvgl/rubik_500_148.c
    src/ui/fonts/lvgl/rubik_500_28.c
    src/ui/fonts/lvgl/manrope_700_16.c
    src/ui/fonts/lvgl/manrope_700_12.c
    src/ui/fonts/lvgl/saira_700_28.c
    src/ui/fonts/lvgl/saira_600_18.c
    src/ui/fonts/lvgl/jetbrains_mono_14.c
    src/ui/fonts/lvgl/matsym_20.c
    src/ui/fonts/lvgl/matsym_24.c)

# --- WiFi OTA leg 1: 2nd MCUmgr SMP transport on UART4 (ESP32 gateway line) ---
# Opt-in (default n); the source self-empties when CONFIG_HPI_WIRELESS_OTA off.
target_sources_ifdef(CONFIG_HPI_WIRELESS_OTA app PRIVATE
    src/transport/wireless_ota/smp_uart4.c)

# --- ESP32 connectivity (WiFi/BLE) bus-consumer service ---
# The service self-stubs when CONFIG_HPI_CONNECTIVITY is off, so it always
# compiles; the WiFi group-64 handlers are gated. The driver headers live
# out-of-tree under drivers/misc/healthybridge_esp32.
target_sources(app PRIVATE src/services/connectivity/healthybridge_service.c)
if(CONFIG_HPI_CONNECTIVITY)
    zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../drivers/misc/healthybridge_esp32)
    target_sources(app PRIVATE src/control/mcumgr_hpi/hpi_wifi.c)
endif()

# --- stream service (bus -> .HP6 DBLK -> CDC0) + streaming control ---
target_sources(app PRIVATE
    src/services/stream_service.c
    src/control/mcumgr_hpi/hpi_streaming.c
)

# --- recording service (bus -> .HP6 file on SD) + SD record control ---
target_sources(app PRIVATE
    src/services/recording_service.c
    src/control/mcumgr_hpi/hpi_recording.c
)

# --- power/telemetry + diagnostics services + their group-64 handlers ---
target_sources(app PRIVATE
    src/services/power_service.c
    src/services/m4_update_service.c
    src/control/mcumgr_hpi/hpi_telemetry.c
    src/control/mcumgr_hpi/hpi_diag.c
)

# --- config service (persistent settings on /lfs) ---
target_sources(app PRIVATE
    src/services/config_service.c
    src/services/button_service.c
)

# --- HealthyLink module framework (provider registry + arbiter +
#     supervisor/quarantine) + skeleton EEG/NPU providers + group-64 commands ---
target_sources(app PRIVATE
    src/healthylink/hl_arbiter.c
    src/healthylink/healthylink_service.c
    src/healthylink/mod_eeg.c
    src/healthylink/mod_npu.c
    src/healthylink/npu_uart_host.c
    src/control/mcumgr_hpi/hpi_modules.c
)

# SPI4 bring-up loopback diagnostic (gated; default off)
if(CONFIG_HPI_SPI4_LOOPBACK_TEST)
    target_sources(app PRIVATE src/healthylink/spi4_loopback.c)
endif()

# --- USB MSC Transfer Mode (gated; only when the MSC class is built) ---
if(CONFIG_USBD_MSC_CLASS)
    target_sources(app PRIVATE
        src/transport/msc/transfer_mode.c
        src/control/mcumgr_hpi/hpi_transfer.c
    )
endif()

# --- Shell adapter (control plane) — dev/factory only (CONFIG_SHELL=n in prod) ---
if(CONFIG_SHELL)
    target_sources(app PRIVATE src/control/shell_hpi/shell_hpi.c)
endif()

# M4 firmware update over group-64 (signed flavor only -- it needs
# the m4_staging_partition from the MCUboot slot map).
target_sources_ifdef(CONFIG_HPI_M4_UPDATE app PRIVATE
    src/control/mcumgr_hpi/hpi_m4fw.c
)

# The M4 image is verified by THIS application, not by MCUboot (the M4 is not an
# MCUboot image -- see m4_update_service.h), so the app needs the public half of
# the release signing key compiled in. Generate it from the same PEM the
# bootloader signing uses, so there is one key for the whole device.
if(CONFIG_HPI_M4_UPDATE_REQUIRE_SIGNATURE)
    set(m4_key "${CONFIG_HPI_M4_SIGNING_KEY}")
    if(m4_key STREQUAL "")
        set(m4_key "${CMAKE_CURRENT_SOURCE_DIR}/../keys/hp6_dev_ec256.pem")
    endif()
    if(NOT EXISTS "${m4_key}")
        message(FATAL_ERROR
            "CONFIG_HPI_M4_UPDATE_REQUIRE_SIGNATURE is set but the signing key "
            "'${m4_key}' does not exist. scripts/build.sh signed generates the dev "
            "key on first run; release builds pass HP6_SIGNING_KEY.")
    endif()
    set(m4_pubkey_c "${CMAKE_CURRENT_BINARY_DIR}/m4fw_pubkey.c")
    add_custom_command(
        OUTPUT  ${m4_pubkey_c}
        COMMAND ${PYTHON_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/../tools/build/gen_m4_pubkey.py
                --key "${m4_key}" --output ${m4_pubkey_c}
        DEPENDS "${m4_key}" ${CMAKE_CURRENT_SOURCE_DIR}/../tools/build/gen_m4_pubkey.py
        COMMENT "Generating M4-update verification key from ${m4_key}"
        VERBATIM
    )
    target_sources(app PRIVATE ${m4_pubkey_c})
endif()

# DFU recovery: reboot into MCUboot serial recovery (group-64 0x00A5). The
# service compiles unconditionally and self-stubs on the dev flavor, so callers
# (UI, adapters) link either way; only the MCUmgr adapter is gated.
target_sources(app PRIVATE
    src/services/dfu_service.c
)
target_sources_ifdef(CONFIG_HPI_RECOVERY_MODE app PRIVATE
    src/control/mcumgr_hpi/hpi_recovery.c
)
