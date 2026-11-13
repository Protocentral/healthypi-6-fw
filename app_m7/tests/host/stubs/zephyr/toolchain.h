/*
 * Copyright (c) 2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Host-build stand-in for <zephyr/toolchain.h>: just the two things the
 * canonical payload header (core/sample_formats.h) uses. Only on the include
 * path of the host tests in this directory, never of a firmware build.
 */
#ifndef HPI_HOST_STUB_ZEPHYR_TOOLCHAIN_H
#define HPI_HOST_STUB_ZEPHYR_TOOLCHAIN_H

#define __packed __attribute__((__packed__))
#define BUILD_ASSERT(cond, msg) _Static_assert(cond, msg)

#endif
