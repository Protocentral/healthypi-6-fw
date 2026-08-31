# Copyright (c) 2025-2026 Protocentral
# SPDX-License-Identifier: MIT

# HealthyBridge ESP32-C6 host link: UART4 with hardware RTS/CTS.
#
# The wire contract lives in healthybridge_esp32_protocol.h, which is
# transport-independent.
#
# The parent CMakeLists enters this directory when the devicetree node is enabled.

zephyr_library()
zephyr_library_sources(
    healthybridge_esp32_codec.c
    healthybridge_esp32_uart_driver.c
)
# test_codec.c is deliberately NOT listed. It is a host regression oracle built
# with plain cc by the healthybridge-codec CI job -- it has a main() and would
# not link into the firmware. Sources here are explicit (never GLOB), so it
# stays out on its own.
