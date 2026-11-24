/* Copyright (c) 2026 Protocentral Electronics. SPDX-License-Identifier: MIT
 * Host-build stand-in for <zephyr/sys/util.h>. */
#ifndef HPI_HOST_STUB_ZEPHYR_UTIL_H
#define HPI_HOST_STUB_ZEPHYR_UTIL_H
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define ARG_UNUSED(x) (void)(x)
#endif
