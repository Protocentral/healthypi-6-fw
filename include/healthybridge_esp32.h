/*
 * Copyright (c) 2025-2026 Protocentral
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file healthybridge_esp32.h
 * @brief HealthyBridge ESP32-C6 co-processor -- public umbrella header.
 *
 * The host link is UART4 with hardware RTS/CTS. Consumers should use the
 * transport-neutral API (healthybridge_esp32_link.h): resolve
 * HEALTHYBRIDGE_LINK_NODE and call healthybridge_send_*() / healthybridge_wifi_*().
 */

#ifndef HEALTHYBRIDGE_ESP32_H
#define HEALTHYBRIDGE_ESP32_H

#include "healthybridge_esp32_link.h"
#include "healthybridge_esp32_protocol.h"

#endif /* HEALTHYBRIDGE_ESP32_H */
