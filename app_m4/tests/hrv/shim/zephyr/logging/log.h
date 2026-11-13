/*
 * Host shim for <zephyr/logging/log.h>: log lines go to stdout prefixed "LOG ",
 * so the test runner can read values the module only logs (the unclamped
 * band powers). SPDX-License-Identifier: MIT
 */
#ifndef HPI_HOST_SHIM_ZEPHYR_LOG_H
#define HPI_HOST_SHIM_ZEPHYR_LOG_H
#include <stdio.h>
#define LOG_MODULE_REGISTER(...)
#define HPI_HOST_LOG(lvl, fmt, ...) printf("LOG " lvl " " fmt "\n", ##__VA_ARGS__)
#define LOG_ERR(fmt, ...) HPI_HOST_LOG("ERR", fmt, ##__VA_ARGS__)
#define LOG_WRN(fmt, ...) HPI_HOST_LOG("WRN", fmt, ##__VA_ARGS__)
#define LOG_INF(fmt, ...) HPI_HOST_LOG("INF", fmt, ##__VA_ARGS__)
#define LOG_DBG(fmt, ...) do { } while (0)
#endif
