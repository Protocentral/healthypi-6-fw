/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * HealthyLink framework service. See healthylink_service.h + hl_provider.h.
 *
 * Hardware note: on v4 the HealthyLink controller reads slot A's EEPROM
 * (`healthylink` node, eeprom=&healthylink_eeprom). Slot B's EEPROM isn't wired
 * yet (dtsi TODO), so detection currently runs on slot A only; slot B reports
 * EMPTY until its EEPROM lands. Per-slot LDO control is a documented bring-up
 * hook (TODO(hw)) — wire it to the slot EN GPIO when validating modules.
 */

#include "healthylink_service.h"
#include "hl_provider.h"
#include "hl_arbiter.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <string.h>

#if defined(CONFIG_HEALTHYLINK)
#include <healthylink/healthylink.h>
#endif

LOG_MODULE_REGISTER(hl_fw, CONFIG_HPI_APP_LOG_LEVEL);

struct hl_slot_rt {
    hl_slot_t                        slot;
    const struct device             *dev;     /* controller/slot device for detect */
    enum hl_slot_state               state;
    bool                             powered;
    uint16_t                         module_id;
    const struct hl_module_provider *prov;
    struct hl_ctx                    ctx;
};

static struct hl_slot_rt g_slots[HL_NUM_SLOTS];
static struct k_mutex    g_lock;

/* The controller device backs slot A's EEPROM today (see header note). */
#if defined(CONFIG_HEALTHYLINK) && DT_NODE_EXISTS(DT_NODELABEL(healthylink))
static const struct device *const slot_a_dev = DEVICE_DT_GET(DT_NODELABEL(healthylink));
#else
static const struct device *const slot_a_dev = NULL;
#endif

static const struct hl_module_provider *find_provider(uint16_t module_id)
{
    STRUCT_SECTION_FOREACH(hl_module_provider, p) {
        if (p->module_id == module_id) {
            return p;
        }
    }
    return NULL;
}

/* ---- hl_provider.h framework entry points ---- */

int hl_slot_power(hl_slot_t slot, bool on)
{
    if (slot >= HL_NUM_SLOTS) {
        return -EINVAL;
    }
    /* TODO(hw): drive the slot EN_MOD_x LDO enable GPIO (PI1 for slot A) and
     * honor the MP2696B power budget. Skeleton logs the request for now. */
    g_slots[slot].powered = on;
    LOG_INF("slot %d power %s", slot, on ? "ON" : "off");
    return 0;
}

int hl_slot_quarantine(hl_slot_t slot)
{
    if (slot >= HL_NUM_SLOTS) {
        return -EINVAL;
    }
    k_mutex_lock(&g_lock, K_FOREVER);
    struct hl_slot_rt *s = &g_slots[slot];
    if (s->prov && s->prov->stop) {
        (void)s->prov->stop(&s->ctx);
    }
    (void)hl_slot_power(slot, false);
    hl_arbiter_release(slot);
    s->state = HL_SLOT_QUARANTINED;
    k_mutex_unlock(&g_lock);
    LOG_WRN("slot %d QUARANTINED (module 0x%04x) -- core unaffected",
            slot, s->module_id);
    /* TODO: emit group-64 evt_module_removed / a health event. */
    return 0;
}

/* Bring one slot up: detect -> match -> claim -> power -> probe -> start. */
static void bring_up_slot(struct hl_slot_rt *s)
{
    s->state = HL_SLOT_EMPTY;
    s->module_id = 0;   /* HEALTHYLINK_MODULE_ID_INVALID */
    s->prov = NULL;

    if (s->dev == NULL || !device_is_ready(s->dev)) {
        return;   /* no detect path for this slot yet */
    }

#if defined(CONFIG_HEALTHYLINK)
    (void)healthylink_detect(s->dev);
    uint16_t id = healthylink_get_module_id(s->dev);
    if (id == HEALTHYLINK_MODULE_ID_INVALID) {
        LOG_INF("slot %d: empty", s->slot);
        return;
    }
    s->module_id = id;

    const struct hl_module_provider *p = find_provider(id);
    if (p == NULL) {
        LOG_WRN("slot %d: module 0x%04x present but no provider", s->slot, id);
        s->state = HL_SLOT_UNSUPPORTED;
        return;
    }
    s->prov = p;
    s->ctx = (struct hl_ctx){ .slot = s->slot, .module_id = id, .priv = NULL };

    if (p->probe && p->probe(&s->ctx) != 0) {
        LOG_ERR("slot %d: %s probe failed", s->slot, p->name);
        s->state = HL_SLOT_ERROR;
        return;
    }
    if (hl_arbiter_claim(s->slot, p->caps) != 0) {
        s->state = HL_SLOT_ERROR;
        return;
    }
    (void)hl_slot_power(s->slot, true);
    if (p->start && p->start(&s->ctx) != 0) {
        LOG_ERR("slot %d: %s start failed -> quarantine", s->slot, p->name);
        hl_slot_quarantine(s->slot);
        return;
    }
    s->state = HL_SLOT_ACTIVE;
    LOG_INF("slot %d: %s active (module 0x%04x, caps 0x%08x)",
            s->slot, p->name, id, p->caps);
#endif
}

int hl_framework_init(void)
{
    k_mutex_init(&g_lock);

    g_slots[HL_SLOT_A] = (struct hl_slot_rt){ .slot = HL_SLOT_A, .dev = slot_a_dev };
    g_slots[HL_SLOT_B] = (struct hl_slot_rt){ .slot = HL_SLOT_B, .dev = NULL };

    for (int i = 0; i < HL_NUM_SLOTS; i++) {
        bring_up_slot(&g_slots[i]);
    }
    LOG_INF("HealthyLink framework: slot A=%d slot B=%d",
            g_slots[HL_SLOT_A].state, g_slots[HL_SLOT_B].state);
    return 0;
}

/* ---- status / control accessors (group-64 handler) ---- */

int hl_get_slot_status(hl_slot_t slot, struct hl_slot_status *out)
{
    if (slot >= HL_NUM_SLOTS || out == NULL) {
        return -EINVAL;
    }
    k_mutex_lock(&g_lock, K_FOREVER);
    struct hl_slot_rt *s = &g_slots[slot];
    out->state = (uint8_t)s->state;
    out->module_id = s->module_id;
    out->caps = s->prov ? s->prov->caps : 0;
    out->powered = s->powered;
    out->name[0] = '\0';
    if (s->prov && s->prov->name) {
        strncpy(out->name, s->prov->name, sizeof(out->name) - 1);
        out->name[sizeof(out->name) - 1] = '\0';
    }
    k_mutex_unlock(&g_lock);
    return 0;
}

int hl_set_slot_power(hl_slot_t slot, bool on)
{
    return hl_slot_power(slot, on);
}
