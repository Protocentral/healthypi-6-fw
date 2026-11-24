/* Copyright (c) 2026 Protocentral Electronics. SPDX-License-Identifier: MIT
 * Host-build stand-in for <zephyr/sys/atomic.h> (single-threaded host test). */
#ifndef HPI_HOST_STUB_ZEPHYR_ATOMIC_H
#define HPI_HOST_STUB_ZEPHYR_ATOMIC_H
typedef long atomic_t;
static inline long atomic_get(const atomic_t *a) { return *a; }
static inline long atomic_set(atomic_t *a, long v) { long o = *a; *a = v; return o; }
#endif
