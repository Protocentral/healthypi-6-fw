/*
 * Host shim for the subset of <zephyr/kernel.h> that app_m4/src/algorithm_module.c
 * touches. Only the HRV maths is exercised on the host; the kernel objects
 * below exist so the module compiles unmodified, and are never run.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef HPI_HOST_SHIM_ZEPHYR_KERNEL_H
#define HPI_HOST_SHIM_ZEPHYR_KERNEL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>
#include <zephyr/toolchain.h>

#define K_FOREVER        (-1)
#define K_NO_WAIT        0
#define K_TICKS_FOREVER  (-1)

struct k_msgq { int unused; };
struct k_sem  { int unused; };
typedef int k_tid_t;

#define K_MSGQ_DEFINE(name, msg_size, max_msgs, align) struct k_msgq name
#define K_SEM_DEFINE(name, initial, limit)             struct k_sem name
#define K_THREAD_DEFINE(name, stack, entry, p1, p2, p3, prio, opts, delay) \
	static const k_tid_t name = 0; \
	static void (*const name##_entry_unused)(void *, void *, void *) __attribute__((unused)) = entry

/* Settable by the test so time-based logic is deterministic. */
extern uint32_t hpi_host_uptime_ms;

static inline uint32_t k_uptime_get_32(void) { return hpi_host_uptime_ms; }
static inline uint32_t k_cycle_get_32(void) { return 0; }
static inline uint32_t k_cyc_to_us_floor32(uint32_t c) { return c; }
static inline int k_msgq_get(struct k_msgq *q, void *d, int t) { (void)q; (void)d; (void)t; return -EAGAIN; }
static inline int k_msgq_put(struct k_msgq *q, const void *d, int t) { (void)q; (void)d; (void)t; return 0; }
static inline uint32_t k_msgq_num_used_get(struct k_msgq *q) { (void)q; return 0; }
static inline int k_sem_take(struct k_sem *s, int t) { (void)s; (void)t; return 0; }
static inline void k_sem_give(struct k_sem *s) { (void)s; }
static inline void k_thread_start(k_tid_t t) { (void)t; }

#endif
