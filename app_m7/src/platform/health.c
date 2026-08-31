/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Subsystem health & heartbeat monitor -- see health.h.
 */

#include "health.h"
#include "../bus/hpi_events.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(hpi_health, CONFIG_HPI_APP_LOG_LEVEL);

#define POLL_MS    CONFIG_HPI_HEALTH_POLL_MS

static struct hpi_health_entry entries[HPI_SUBSYS_COUNT];
static struct k_spinlock lock;

const char *hpi_health_state_str(enum hpi_health_state s)
{
	switch (s) {
	case HPI_HEALTH_OK:       return "OK";
	case HPI_HEALTH_DEGRADED: return "DEGRADED";
	case HPI_HEALTH_FAILED:   return "FAILED";
	default:                  return "UNKNOWN";
	}
}

/* Pack (subsys id << 8 | state) into the event arg for host/UI observers. */
static inline int32_t health_evt_arg(enum hpi_subsys id, enum hpi_health_state s)
{
	return (int32_t)(((uint32_t)id << 8) | (uint32_t)s);
}

void hpi_health_register(enum hpi_subsys id, const char *name, uint32_t hb_timeout_ms)
{
	if (id >= HPI_SUBSYS_COUNT) {
		return;
	}
	k_spinlock_key_t k = k_spin_lock(&lock);
	entries[id].name = name;
	entries[id].state = HPI_HEALTH_UNKNOWN;
	entries[id].registered = 1;
	entries[id].hb_timeout_ms = hb_timeout_ms;
	entries[id].last_seen_ms = 0;
	entries[id].reason = NULL;
	k_spin_unlock(&lock, k);
	LOG_INF("registered '%s' (heartbeat=%u ms)", name ? name : "?", hb_timeout_ms);
}

/* Apply a state change under lock; returns true if the state actually changed.
 * Logging + event publish happen by the caller, outside the lock. */
static bool set_state_locked(enum hpi_subsys id, enum hpi_health_state st, const char *reason)
{
	if (!entries[id].registered || entries[id].state == st) {
		entries[id].reason = reason ? reason : entries[id].reason;
		return false;
	}
	entries[id].state = (uint8_t)st;
	entries[id].reason = reason;
	return true;
}

static void announce(enum hpi_subsys id, enum hpi_health_state st, const char *reason)
{
	if (st == HPI_HEALTH_OK) {
		LOG_INF("%s -> OK", entries[id].name ? entries[id].name : "?");
	} else {
		LOG_WRN("%s -> %s (%s)", entries[id].name ? entries[id].name : "?",
			hpi_health_state_str(st), reason ? reason : "");
	}
	hpi_events_publish(HPI_EVT_HEALTH, health_evt_arg(id, st));
}

void hpi_health_set(enum hpi_subsys id, enum hpi_health_state state, const char *reason)
{
	if (id >= HPI_SUBSYS_COUNT) {
		return;
	}
	k_spinlock_key_t k = k_spin_lock(&lock);
	bool changed = set_state_locked(id, state, reason);
	k_spin_unlock(&lock, k);
	if (changed) {
		announce(id, state, reason);
	}
}

void hpi_health_alive(enum hpi_subsys id)
{
	if (id >= HPI_SUBSYS_COUNT) {
		return;
	}
	/* Cheap + ISR-safe: only stamp the last-seen time. The monitor thread
	 * promotes the subsystem back to OK (and announces/logs/publishes) on its
	 * next poll -- doing it here would risk zbus/LOG from an ISR/trigger ctx. */
	k_spinlock_key_t k = k_spin_lock(&lock);
	entries[id].last_seen_ms = k_uptime_get();
	k_spin_unlock(&lock, k);
}

enum hpi_health_state hpi_health_get(enum hpi_subsys id)
{
	if (id >= HPI_SUBSYS_COUNT) {
		return HPI_HEALTH_UNKNOWN;
	}
	return (enum hpi_health_state)entries[id].state;
}

void hpi_health_snapshot(struct hpi_health_report *out)
{
	if (!out) {
		return;
	}
	enum hpi_health_state worst = HPI_HEALTH_OK;
	k_spinlock_key_t k = k_spin_lock(&lock);
	for (int i = 0; i < HPI_SUBSYS_COUNT; i++) {
		out->e[i] = entries[i];
		if (entries[i].registered && entries[i].state > worst) {
			worst = (enum hpi_health_state)entries[i].state;
		}
	}
	k_spin_unlock(&lock, k);
	out->overall = (uint8_t)worst;
}

enum hpi_health_state hpi_health_overall(void)
{
	struct hpi_health_report r;
	hpi_health_snapshot(&r);
	return (enum hpi_health_state)r.overall;
}

/* ---- monitor thread: enforce heartbeat deadlines ---- */
static void monitor(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	for (;;) {
		k_msleep(POLL_MS);
		int64_t now = k_uptime_get();

		for (int i = 0; i < HPI_SUBSYS_COUNT; i++) {
			enum hpi_health_state newst = HPI_HEALTH_UNKNOWN;
			const char *reason = NULL;
			bool changed = false;

			k_spinlock_key_t k = k_spin_lock(&lock);
			struct hpi_health_entry *e = &entries[i];
			/* Only heartbeat-armed subsystems that have checked in at least
			 * once are watched; never-seen ones stay UNKNOWN until first beat. */
			if (e->registered && e->hb_timeout_ms > 0 && e->last_seen_ms > 0) {
				int64_t age = now - e->last_seen_ms;
				if (age > (int64_t)(2 * e->hb_timeout_ms)) {
					newst = HPI_HEALTH_FAILED;
					reason = "heartbeat lost";
				} else if (age > (int64_t)e->hb_timeout_ms) {
					newst = HPI_HEALTH_DEGRADED;
					reason = "heartbeat late";
				} else {
					newst = HPI_HEALTH_OK;
				}
				if (newst != HPI_HEALTH_UNKNOWN) {
					changed = set_state_locked((enum hpi_subsys)i, newst, reason);
				}
			}
			k_spin_unlock(&lock, k);

			if (changed) {
				announce((enum hpi_subsys)i, newst, reason);
			}
		}
	}
}

K_THREAD_STACK_DEFINE(health_stack, 1024);
static struct k_thread health_thread;
static bool started;

void hpi_health_start(void)
{
	if (started) {
		return;
	}
	started = true;
	k_thread_create(&health_thread, health_stack, K_THREAD_STACK_SIZEOF(health_stack),
			monitor, NULL, NULL, NULL,
			K_LOWEST_APPLICATION_THREAD_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&health_thread, "health");
	LOG_INF("health monitor started (poll %d ms)", POLL_MS);
}
