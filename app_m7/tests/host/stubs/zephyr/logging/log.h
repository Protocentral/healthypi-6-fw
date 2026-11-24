/* Copyright (c) 2026 Protocentral Electronics. SPDX-License-Identifier: MIT
 * Host-build stand-in for <zephyr/logging/log.h>: logging compiled out. */
#ifndef HPI_HOST_STUB_ZEPHYR_LOG_H
#define HPI_HOST_STUB_ZEPHYR_LOG_H
#define LOG_MODULE_REGISTER(...)
#define LOG_ERR(...) do { } while (0)
#define LOG_WRN(...) do { } while (0)
#define LOG_INF(...) do { } while (0)
#define LOG_DBG(...) do { } while (0)
#endif
