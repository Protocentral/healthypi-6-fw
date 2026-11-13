/* Host shim for <zephyr/toolchain.h>. SPDX-License-Identifier: MIT */
#ifndef HPI_HOST_SHIM_ZEPHYR_TOOLCHAIN_H
#define HPI_HOST_SHIM_ZEPHYR_TOOLCHAIN_H
#define __packed        __attribute__((__packed__))
#define BUILD_ASSERT(c, msg) _Static_assert(c, msg)
#define ARG_UNUSED(x)   (void)(x)
#endif
