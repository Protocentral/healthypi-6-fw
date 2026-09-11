/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * HealthyLink framework service. See healthylink_service.h + hl_provider.h.
 *
 * Each slot is its own HealthyLink driver device (see the slot nodes in the
 * board devicetree).
 *
 * Locking, so status never waits on hardware:
 *   g_op_lock  serialises everything that touches hardware (bring-up, power,
 *              quarantine); held across I2C and provider calls.
 *   g_lock     guards the published fields, held only to copy them.
 * The op path is the only writer of the published fields, so it may read them
 * under g_op_lock alone.
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

/* The driver makes a device only for a slot node with an `eeprom`. */
#if defined(CONFIG_HEALTHYLINK) && \
	DT_NODE_HAS_STATUS(DT_NODELABEL(healthylink_slot_a), okay) && \
	DT_NODE_HAS_PROP(DT_NODELABEL(healthylink_slot_a), eeprom)
#define HL_SLOT_A_DEV DEVICE_DT_GET(DT_NODELABEL(healthylink_slot_a))
#else
#define HL_SLOT_A_DEV NULL
#endif

#if defined(CONFIG_HEALTHYLINK) && \
	DT_NODE_HAS_STATUS(DT_NODELABEL(healthylink_slot_b), okay) && \
	DT_NODE_HAS_PROP(DT_NODELABEL(healthylink_slot_b), eeprom)
#define HL_SLOT_B_DEV DEVICE_DT_GET(DT_NODELABEL(healthylink_slot_b))
#else
#define HL_SLOT_B_DEV NULL
#endif

struct hl_slot_rt {
    hl_slot_t                        slot;
    const struct device             *dev;     /* HealthyLink slot device, or NULL */

    /* Published: written under g_lock, read by hl_get_slot_status(). */
    enum hl_slot_state               state;
    uint16_t                         module_id;
    const struct hl_module_provider *prov;
    bool                             busy;

    /* Op path only (g_op_lock). */
    struct hl_ctx                    ctx;

    /* hl_request_slot_power(): the latest request wins. */
    struct k_work                    work;
    volatile bool                    req_on;
};

static struct hl_slot_rt g_slots[HL_NUM_SLOTS] = {
    [HL_SLOT_A] = { .slot = HL_SLOT_A, .dev = HL_SLOT_A_DEV },
    [HL_SLOT_B] = { .slot = HL_SLOT_B, .dev = HL_SLOT_B_DEV },
};
static K_MUTEX_DEFINE(g_lock);
static K_MUTEX_DEFINE(g_op_lock);
static bool g_inited;

static char slot_char(hl_slot_t slot)
{
    return (char)('A' + (int)slot);
}

static const struct hl_module_provider *find_provider(uint16_t module_id)
{
    STRUCT_SECTION_FOREACH(hl_module_provider, p) {
        if (p->module_id == module_id) {
            return p;
        }
    }
    return NULL;
}

static void publish(struct hl_slot_rt *s, enum hl_slot_state state,
                    uint16_t module_id, const struct hl_module_provider *prov)
{
    k_mutex_lock(&g_lock, K_FOREVER);
    s->state = state;
    s->module_id = module_id;
    s->prov = prov;
    k_mutex_unlock(&g_lock);
}

static void set_busy(struct hl_slot_rt *s, bool busy)
{
    k_mutex_lock(&g_lock, K_FOREVER);
    s->busy = busy;
    k_mutex_unlock(&g_lock);
}

/* ---- hl_provider.h framework entry points ---- */

/* The rail alone; detect/start/stop are the caller's business. */
int hl_slot_power(hl_slot_t slot, bool on)
{
    if (slot >= HL_NUM_SLOTS) {
        return -EINVAL;
    }
#if defined(CONFIG_HEALTHYLINK)
    const struct device *dev = g_slots[slot].dev;

    if (dev == NULL) {
        return -ENODEV;
    }
    return healthylink_slot_power(dev, on);
#else
    ARG_UNUSED(on);
    return -ENOTSUP;
#endif
}

/* Stop what runs in a slot and cut its power. Caller holds g_op_lock. */
static void stop_slot(struct hl_slot_rt *s, bool started)
{
    if (started && s->prov != NULL && s->prov->stop != NULL) {
        (void)s->prov->stop(&s->ctx);
    }
    hl_arbiter_release(s->slot);
    (void)hl_slot_power(s->slot, false);
}

int hl_slot_quarantine(hl_slot_t slot)
{
    if (slot >= HL_NUM_SLOTS) {
        return -EINVAL;
    }
    k_mutex_lock(&g_op_lock, K_FOREVER);   /* recursive from bring_up_slot() */
    struct hl_slot_rt *s = &g_slots[slot];

    stop_slot(s, true);
    publish(s, HL_SLOT_QUARANTINED, s->module_id, s->prov);
    k_mutex_unlock(&g_op_lock);
    LOG_WRN("slot %c QUARANTINED (module 0x%04x) -- core unaffected",
            slot_char(slot), s->module_id);
    /* TODO: emit group-64 evt_module_removed / a health event. */
    return 0;
}

/* Bring one slot up: detect -> match -> probe -> claim -> power -> start.
 * Caller holds g_op_lock. Any outcome other than ACTIVE leaves it unpowered. */
static void bring_up_slot(struct hl_slot_rt *s)
{
#if defined(CONFIG_HEALTHYLINK)
    const char c = slot_char(s->slot);

    if (s->dev == NULL || !device_is_ready(s->dev)) {
        publish(s, HL_SLOT_EMPTY, 0, NULL);
        return;   /* no detection path for this slot */
    }

    (void)healthylink_detect(s->dev);   /* identify-then-power */
    enum healthylink_status hs = healthylink_get_status(s->dev);
    uint16_t id = healthylink_get_module_id(s->dev);

    if (hs == HEALTHYLINK_STATUS_NOT_PRESENT) {
        LOG_INF("slot %c: empty", c);
        publish(s, HL_SLOT_EMPTY, 0, NULL);
        return;
    }
    if (hs == HEALTHYLINK_STATUS_ERROR || id == HEALTHYLINK_MODULE_ID_INVALID) {
        LOG_ERR("slot %c: a module answered but could not be identified or "
                "powered (module 0x%04x)", c, id);
        (void)hl_slot_power(s->slot, false);
        publish(s, HL_SLOT_ERROR, id, NULL);
        return;
    }

    const struct hl_module_provider *p = find_provider(id);

    if (p == NULL) {
        LOG_WRN("slot %c: module 0x%04x present but no provider -- left "
                "unpowered", c, id);
        (void)hl_slot_power(s->slot, false);
        publish(s, HL_SLOT_UNSUPPORTED, id, NULL);
        return;
    }
    s->ctx = (struct hl_ctx){ .slot = s->slot, .module_id = id, .priv = NULL };

    if (p->probe && p->probe(&s->ctx) != 0) {
        LOG_ERR("slot %c: %s probe failed", c, p->name);
        (void)hl_slot_power(s->slot, false);
        publish(s, HL_SLOT_ERROR, id, p);
        return;
    }
    if (hl_arbiter_claim(s->slot, p->caps) != 0) {
        /* The other slot already holds an interface this module needs. */
        (void)hl_slot_power(s->slot, false);
        publish(s, HL_SLOT_ERROR, id, p);
        return;
    }
    if (hl_slot_power(s->slot, true) != 0) {
        LOG_ERR("slot %c: %s refused power (load-switch fault)", c, p->name);
        hl_arbiter_release(s->slot);
        publish(s, HL_SLOT_ERROR, id, p);
        return;
    }
    if (p->start && p->start(&s->ctx) != 0) {
        LOG_ERR("slot %c: %s start failed -> quarantine", c, p->name);
        publish(s, HL_SLOT_ERROR, id, p);
        hl_slot_quarantine(s->slot);
        return;
    }
    publish(s, HL_SLOT_ACTIVE, id, p);
    LOG_INF("slot %c: %s active (module 0x%04x, caps 0x%08x)",
            c, p->name, id, p->caps);
#else
    publish(s, HL_SLOT_EMPTY, 0, NULL);
#endif
}

static void power_work_fn(struct k_work *w)
{
    struct hl_slot_rt *s = CONTAINER_OF(w, struct hl_slot_rt, work);
    bool on = s->req_on;
    int rc = hl_set_slot_power(s->slot, on);

    LOG_INF("slot %c: power %s (on-device request) -> %d", slot_char(s->slot),
            on ? "on" : "off", rc);
}

int hl_framework_init(void)
{
    k_mutex_lock(&g_op_lock, K_FOREVER);
    for (int i = 0; i < HL_NUM_SLOTS; i++) {
        k_work_init(&g_slots[i].work, power_work_fn);
        bring_up_slot(&g_slots[i]);
    }
    g_inited = true;
    k_mutex_unlock(&g_op_lock);

    LOG_INF("HealthyLink framework: slot A=%d slot B=%d",
            g_slots[HL_SLOT_A].state, g_slots[HL_SLOT_B].state);
    return 0;
}

/* ---- status / control accessors ---- */

int hl_get_slot_status(hl_slot_t slot, struct hl_slot_status *out)
{
    if (slot >= HL_NUM_SLOTS || out == NULL) {
        return -EINVAL;
    }
    struct hl_slot_rt *s = &g_slots[slot];

    k_mutex_lock(&g_lock, K_FOREVER);
    out->state = (uint8_t)s->state;
    out->module_id = s->module_id;
    out->caps = s->prov ? s->prov->caps : 0;
    out->busy = s->busy;
    out->name[0] = '\0';
    if (s->prov && s->prov->name) {
        strncpy(out->name, s->prov->name, sizeof(out->name) - 1);
        out->name[sizeof(out->name) - 1] = '\0';
    }
    k_mutex_unlock(&g_lock);

    /* No I/O: both read cached state. */
    out->detectable = s->dev != NULL && device_is_ready(s->dev);
#if defined(CONFIG_HEALTHYLINK)
    out->powered = out->detectable && healthylink_slot_is_powered(s->dev);
#else
    out->powered = false;
#endif
    return 0;
}

int hl_set_slot_power(hl_slot_t slot, bool on)
{
    if (slot >= HL_NUM_SLOTS) {
        return -EINVAL;
    }
    struct hl_slot_rt *s = &g_slots[slot];

    if (s->dev == NULL) {
        return -ENODEV;   /* no ID EEPROM: nothing to detect or power */
    }

    int rc;

    k_mutex_lock(&g_op_lock, K_FOREVER);
    set_busy(s, true);
    if (on) {
        if (s->state != HL_SLOT_ACTIVE) {
            bring_up_slot(s);
        }
        rc = s->state == HL_SLOT_ACTIVE ? 0
           : s->state == HL_SLOT_EMPTY  ? -ENODEV
           : -EIO;
    } else {
        stop_slot(s, s->state == HL_SLOT_ACTIVE);
        publish(s, s->module_id != 0 ? HL_SLOT_OFF : HL_SLOT_EMPTY,
                s->module_id, s->prov);
        rc = 0;
    }
    set_busy(s, false);
    k_mutex_unlock(&g_op_lock);
    return rc;
}

int hl_request_slot_power(hl_slot_t slot, bool on)
{
    if (slot >= HL_NUM_SLOTS) {
        return -EINVAL;
    }
    struct hl_slot_rt *s = &g_slots[slot];

    if (!g_inited) {
        return -EAGAIN;
    }
    if (s->dev == NULL) {
        return -ENODEV;
    }
    s->req_on = on;
    set_busy(s, true);
    (void)k_work_submit(&s->work);
    return 0;
}
