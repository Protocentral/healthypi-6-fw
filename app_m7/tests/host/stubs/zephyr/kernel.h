/*
 * Copyright (c) 2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Host-build stand-in for <zephyr/kernel.h>: what core/resp_rate.c touches.
 * The thread and timing objects exist so the module compiles; the host test
 * drives the module's sample path directly and never starts the thread.
 */
#ifndef HPI_HOST_STUB_ZEPHYR_KERNEL_H
#define HPI_HOST_STUB_ZEPHYR_KERNEL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <zephyr/toolchain.h>

struct k_thread { int unused; };
typedef int k_thread_stack_t;
#define K_THREAD_STACK_DEFINE(name, size) k_thread_stack_t name[1]
#define K_THREAD_STACK_SIZEOF(s) sizeof(s)
#define K_NO_WAIT 0

static inline int64_t k_uptime_get(void) { return 0; }
static inline void *k_thread_create(struct k_thread *t, k_thread_stack_t *s,
				    size_t sz, void (*fn)(void *, void *, void *),
				    void *a, void *b, void *c, int prio,
				    uint32_t opt, int delay)
{
	(void)t; (void)s; (void)sz; (void)fn; (void)a; (void)b; (void)c;
	(void)prio; (void)opt; (void)delay;
	return NULL;
}
static inline int k_thread_name_set(struct k_thread *t, const char *n)
{
	(void)t; (void)n;
	return 0;
}

#endif
