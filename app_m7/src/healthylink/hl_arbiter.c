/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 */

#include "hl_arbiter.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <healthylink/healthylink.h>   /* HEALTHYLINK_CAP_REQUIRES_* */

LOG_MODULE_REGISTER(hl_arbiter, CONFIG_HPI_APP_LOG_LEVEL);

/* Only the "required interface" bits (0..7) are mutually exclusive across slots;
 * feature/power bits are per-module and not arbitrated. */
#define HL_REQUIRED_IFACE_MASK 0x000000FFu

#define HL_NUM_SLOTS 2

static uint32_t g_slot_caps[HL_NUM_SLOTS];
static struct k_mutex g_lock;
static bool g_inited;

static void ensure_init(void)
{
    if (!g_inited) {
        k_mutex_init(&g_lock);
        g_inited = true;
    }
}

int hl_arbiter_claim(hl_slot_t slot, uint32_t caps)
{
    if (slot >= HL_NUM_SLOTS) {
        return -EINVAL;
    }
    ensure_init();

    uint32_t want = caps & HL_REQUIRED_IFACE_MASK;
    int rc = 0;

    k_mutex_lock(&g_lock, K_FOREVER);
    uint32_t held_by_others = 0;
    for (int i = 0; i < HL_NUM_SLOTS; i++) {
        if (i != (int)slot) {
            held_by_others |= g_slot_caps[i];
        }
    }
    if (want & held_by_others) {
        LOG_ERR("slot %d: resource conflict (want 0x%02x, busy 0x%02x)",
                slot, want, (want & held_by_others));
        rc = -EBUSY;
    } else {
        g_slot_caps[slot] = want;
    }
    k_mutex_unlock(&g_lock);
    return rc;
}

void hl_arbiter_release(hl_slot_t slot)
{
    if (slot >= HL_NUM_SLOTS) {
        return;
    }
    ensure_init();
    k_mutex_lock(&g_lock, K_FOREVER);
    g_slot_caps[slot] = 0;
    k_mutex_unlock(&g_lock);
}

uint32_t hl_arbiter_claimed(void)
{
    uint32_t all = 0;
    ensure_init();
    k_mutex_lock(&g_lock, K_FOREVER);
    for (int i = 0; i < HL_NUM_SLOTS; i++) {
        all |= g_slot_caps[i];
    }
    k_mutex_unlock(&g_lock);
    return all;
}
