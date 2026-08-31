/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Button service (L4) -- turns presses of the single user button (PI3) into the
 * gestures the redesign's button map defines, and routes each to the right
 * owner. The full gesture map is the list below.
 *
 *   short press, recording      -> mark the instant (handled here)
 *   short press, not recording  -> toggle display sleep   (UI takes the action)
 *   double press                -> jump to Home           (UI takes the action)
 *   hold 5 s                    -> power menu             (UI takes the action)
 *
 * Marking is handled here so it produces exactly the same record as the
 * on-screen MARK button. The other three are presentation: a service never
 * calls into L5, so the UI polls hpi_button_take_action() in its loop
 * (dependency stays L5 -> L4).
 */

#ifndef HPI_SERVICES_BUTTON_SERVICE_H
#define HPI_SERVICES_BUTTON_SERVICE_H

#ifdef __cplusplus
extern "C" {
#endif

enum hpi_button_action {
	HPI_BTN_NONE = 0,
	HPI_BTN_TOGGLE_SLEEP,  /* short press while not recording */
	HPI_BTN_HOME,          /* double press */
	HPI_BTN_POWER_MENU,    /* 5 s hold -- opens the power menu */
};

int hpi_button_service_init(void);

/* Consume the pending action, or HPI_BTN_NONE. Call from the UI loop. Only the
 * most recent action is kept -- a gesture that arrives while one is still
 * pending replaces it, since acting on a stale press is worse than dropping it. */
enum hpi_button_action hpi_button_take_action(void);

#ifdef __cplusplus
}
#endif

#endif /* HPI_SERVICES_BUTTON_SERVICE_H */
