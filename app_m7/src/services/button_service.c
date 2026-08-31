/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Button service -- gesture decoding for the single user button. See
 * button_service.h for the map and why the UI actions are polled rather than
 * called.
 */

#include "button_service.h"
#include "recording_service.h"
#include "bus/hpi_events.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

LOG_MODULE_REGISTER(hpi_btn, CONFIG_HPI_APP_LOG_LEVEL);

/* Gesture timing. DOUBLE_GAP is the window after a release in which a second
 * press counts as a double; it also delays a single short press by the same
 * amount, since a short press cannot act until it is known not to be the first
 * half of a double. HOLD_MS fires while the button is still down, not on
 * release; once it fires, the release is swallowed. */
#define BTN_DOUBLE_GAP_MS  350
#define BTN_HOLD_MS        5000

/* The gpio-keys driver instantiates one device per PARENT node (the `buttons`
 * container), not per key, so take the parent of the key we care about. */
#define BTN_NODE   DT_PARENT(DT_NODELABEL(user_button))
#define BTN_CODE   DT_PROP(DT_NODELABEL(user_button), zephyr_code)

static const struct device *const btn_dev = DEVICE_DT_GET_OR_NULL(BTN_NODE);

static atomic_t g_pending = ATOMIC_INIT(HPI_BTN_NONE);

/* Defined statically, not initialised in init(): the input callback is
 * registered at link time and can fire before hpi_button_service_init() runs. */
static void single_work_fn(struct k_work *w);
static void hold_work_fn(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(g_single_work, single_work_fn);  /* short press */
static K_WORK_DELAYABLE_DEFINE(g_hold_work, hold_work_fn);      /* 5 s hold */

static int64_t g_press_ms;      /* uptime at the last press           */
static bool    g_hold_fired;    /* hold already dispatched this press */
static bool    g_await_second;  /* a release is waiting out the double window */

static void post(enum hpi_button_action a)
{
	atomic_set(&g_pending, (atomic_val_t)a);
}

enum hpi_button_action hpi_button_take_action(void)
{
	return (enum hpi_button_action)atomic_set(&g_pending, HPI_BTN_NONE);
}

/* A short press that outlived the double-tap window. While recording this marks
 * the instant -- the same record the on-screen MARK button writes -- otherwise
 * it toggles display sleep. */
static void single_work_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	g_await_second = false;

	if (hpi_recording_active()) {
		int seq = hpi_recording_mark();

		if (seq >= 0) {
			LOG_INF("button: short -> mark %d", seq);
			hpi_events_publish(HPI_EVT_USER_MARK, seq);
		}
		return;
	}
	LOG_INF("button: short -> toggle sleep");
	post(HPI_BTN_TOGGLE_SLEEP);
}

static void hold_work_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	g_hold_fired = true;
	/* Cancel any pending single: the press became a hold. */
	(void)k_work_cancel_delayable(&g_single_work);
	g_await_second = false;
	LOG_INF("button: hold %d ms -> power menu", BTN_HOLD_MS);
	post(HPI_BTN_POWER_MENU);
}

static void button_cb(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);

	/* gpio-keys reports the key code with value 1 = pressed, 0 = released.
	 * Check the code as well as the type: this callback is scoped to the
	 * button device below, but INPUT_EV_KEY is not ours alone -- the GT911
	 * touch controller reports BTN_TOUCH with the same event type, and an
	 * unscoped callback would take every screen tap as a button press. */
	if (evt->type != INPUT_EV_KEY || evt->code != BTN_CODE) {
		return;
	}

	if (evt->value) {
		LOG_INF("button: press");
		g_press_ms = k_uptime_get();
		g_hold_fired = false;
		k_work_reschedule(&g_hold_work, K_MSEC(BTN_HOLD_MS));

		if (g_await_second) {
			/* Second press inside the window: a double. Drop the
			 * queued single and act now. */
			(void)k_work_cancel_delayable(&g_single_work);
			(void)k_work_cancel_delayable(&g_hold_work);
			g_await_second = false;
			LOG_INF("button: double -> home");
			post(HPI_BTN_HOME);
		}
		return;
	}

	/* Release. */
	LOG_INF("button: release after %d ms", (int)(k_uptime_get() - g_press_ms));
	(void)k_work_cancel_delayable(&g_hold_work);
	if (g_hold_fired) {
		g_hold_fired = false;   /* the hold already acted; swallow this */
		return;
	}
	/* Wait out the double window before treating it as a single. */
	g_await_second = true;
	k_work_reschedule(&g_single_work, K_MSEC(BTN_DOUBLE_GAP_MS));
}

/* Scoped to the button device: a NULL device here means "every input
 * device", which would deliver the GT911's touch events too. */
INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(BTN_NODE), button_cb, NULL);

int hpi_button_service_init(void)
{
	if (btn_dev == NULL) {
		LOG_WRN("no user_button in DT; hardware button unavailable");
		return -ENODEV;
	}
	if (!device_is_ready(btn_dev)) {
		LOG_ERR("user button not ready");
		return -ENODEV;
	}
	LOG_INF("button service ready (short/double/hold %d ms)", BTN_HOLD_MS);
	return 0;
}
