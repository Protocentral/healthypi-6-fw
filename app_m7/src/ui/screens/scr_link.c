/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Link — connectivity control. Wi-Fi and BLE are both live against the
 * connectivity service; nothing here fabricates a state.
 *
 * This is the ONE place a user can turn connectivity on: the device boots with
 * the ESP32-C6 held in reset and both radios down (a USB-powered unit browns
 * out otherwise), so these toggles are the only on-device path to a network.
 * SET UP WI-FI opens the captive portal, the only way credentials ever reach
 * the device.
 */
#include "scr_link.h"
#include "../components/hpi_ui_components.h"
#include "../theme/hpi_m3_theme.h"
#include "../ui_module.h"

#include <errno.h>
#include <stdio.h>

#include "services/connectivity/healthybridge_service.h"

static struct {
	lv_obj_t *wifi_pill;   /* status chip label */
	lv_obj_t *wifi_detail; /* ssid · rssi · ip  */
	lv_obj_t *wifi_sw;     /* toggle track      */
	lv_obj_t *wifi_knob;
	lv_obj_t *ble_pill;
	lv_obj_t *ble_detail;
	lv_obj_t *ble_sw;
	lv_obj_t *ble_knob;
} s_ln;

/* A small pill-shaped status chip. */
static lv_obj_t *chip(lv_obj_t *parent, const char *text, lv_color_t col)
{
	lv_obj_t *c = lv_obj_create(parent);
	lv_obj_set_size(c, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_color(c, col, 0);
	lv_obj_set_style_bg_opa(c, LV_OPA_20, 0);
	lv_obj_set_style_radius(c, HPI_M3_RADIUS_PILL, 0);
	lv_obj_set_style_border_width(c, 0, 0);
	lv_obj_set_style_pad_hor(c, HPI_M3_SPACE_2, 0);
	lv_obj_set_style_pad_ver(c, HPI_M3_SPACE_1, 0);
	lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_t *l = lv_label_create(c);
	lv_label_set_text(l, text);
	lv_obj_set_style_text_font(l, HPI_M3_FONT_CAPS_SM, 0);
	lv_obj_set_style_text_color(l, col, 0);
	return l;   /* return the label so callers can retext it */
}

/* A 44x24 visual toggle; returns the track, writes the knob to *knob_out.
 *
 * Do NOT use hpi_m3_apply_touch() here -- its min sizes would deform the
 * 44x24 track into a 64x64 square. The expanded click area meets the touch
 * floor instead (44+2*20 = 84 wide, 24+2*20 = 64 tall).
 */
static lv_obj_t *toggle(lv_obj_t *parent, bool on, lv_obj_t **knob_out)
{
	lv_obj_t *sw = lv_obj_create(parent);
	lv_obj_set_size(sw, 44, 24);
	lv_obj_set_ext_click_area(sw, 20);
	lv_obj_set_style_radius(sw, 12, 0);
	lv_obj_set_style_bg_color(sw, on ? HPI_M3_PRIMARY : HPI_M3_OUTLINE, 0);
	lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(sw, 0, 0);
	lv_obj_clear_flag(sw, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_t *k = lv_obj_create(sw);
	lv_obj_set_size(k, 20, 20);
	lv_obj_align(k, on ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, on ? -2 : 2, 0);
	lv_obj_set_style_radius(k, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(k, on ? HPI_M3_SURFACE : HPI_M3_ON_SURFACE_MUTED, 0);
	lv_obj_set_style_bg_opa(k, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(k, 0, 0);
	lv_obj_clear_flag(k, LV_OBJ_FLAG_SCROLLABLE);
	if (knob_out) {
		*knob_out = k;
	}
	return sw;
}

static void toggle_set(lv_obj_t *sw, lv_obj_t *knob, bool on)
{
	lv_obj_set_style_bg_color(sw, on ? HPI_M3_PRIMARY : HPI_M3_OUTLINE, 0);
	lv_obj_align(knob, on ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, on ? -2 : 2, 0);
	lv_obj_set_style_bg_color(knob, on ? HPI_M3_SURFACE : HPI_M3_ON_SURFACE_MUTED, 0);
}

static lv_obj_t *link_card(lv_obj_t *parent, const char *icon, lv_color_t icol,
			   const char *name, lv_obj_t **hdr_out)
{
	lv_obj_t *c = lv_obj_create(parent);
	lv_obj_set_width(c, lv_pct(100));
	lv_obj_set_height(c, LV_SIZE_CONTENT);
	hpi_m3_apply_card(c, HPI_M3_SURFACE_CONTAINER, HPI_M3_RADIUS_LG);
	lv_obj_set_style_pad_all(c, HPI_M3_SPACE_4, 0);
	lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(c, HPI_M3_SPACE_2, 0);

	lv_obj_t *hdr = lv_obj_create(c);
	lv_obj_set_width(hdr, lv_pct(100));
	lv_obj_set_height(hdr, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(hdr, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(hdr, 0, 0);
	lv_obj_set_style_pad_all(hdr, 0, 0);
	lv_obj_set_style_pad_column(hdr, HPI_M3_SPACE_2, 0);
	lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);

	lv_obj_t *ic = lv_label_create(hdr);
	lv_label_set_text(ic, icon);
	lv_obj_set_style_text_font(ic, HPI_M3_FONT_ICON, 0);
	lv_obj_set_style_text_color(ic, icol, 0);
	lv_obj_t *nm = lv_label_create(hdr);
	lv_label_set_text(nm, name);
	lv_obj_set_style_text_font(nm, HPI_M3_FONT_CAPS, 0);
	lv_obj_set_style_text_color(nm, HPI_M3_ON_SURFACE, 0);

	lv_obj_t *sp = lv_obj_create(hdr);   /* growable spacer -> trailing items right */
	lv_obj_set_style_bg_opa(sp, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(sp, 0, 0);
	lv_obj_set_style_pad_all(sp, 0, 0);
	lv_obj_set_height(sp, 1);
	lv_obj_set_flex_grow(sp, 1);
	lv_obj_clear_flag(sp, LV_OBJ_FLAG_SCROLLABLE);

	if (hdr_out) {
		*hdr_out = hdr;
	}
	return c;
}

/*
 * Both toggles read the CURRENT state, ask for the opposite, and let
 * hpi_scr_link_refresh() render whatever actually happened. Never paint the
 * new position optimistically: enabling takes the better part of a second
 * (co-processor boot), and the service reports STARTING for that interval.
 * Both service calls are non-blocking by contract, which is what makes them
 * safe from the LVGL thread; see healthybridge_service.h.
 */
static void wifi_toggle_cb(lv_event_t *e)
{
	ARG_UNUSED(e);
	struct hpi_conn_status st;

	hpi_connectivity_get_status(&st);
	hpi_connectivity_wifi_enable((st.radios & HPI_CONN_RADIO_WIFI) == 0);
	hpi_scr_link_refresh();
}

static void ble_toggle_cb(lv_event_t *e)
{
	ARG_UNUSED(e);
	struct hpi_conn_status st;

	hpi_connectivity_get_status(&st);
	hpi_connectivity_ble_enable((st.radios & HPI_CONN_RADIO_BLE) == 0);
	hpi_scr_link_refresh();
}

/*
 * Open the co-processor's SoftAP captive portal -- the only way credentials
 * ever reach the device: no command carries an SSID and password over the
 * host link, and there is no on-device keyboard.
 */
static void softap_cb(lv_event_t *e)
{
	ARG_UNUSED(e);
	hpi_connectivity_wifi_softap();
	hpi_scr_link_refresh();
}

lv_obj_t *hpi_scr_link_create(lv_obj_t *parent)
{
	lv_obj_t *root = lv_obj_create(parent);
	lv_obj_set_size(root, lv_pct(100), lv_pct(100));
	hpi_m3_apply_card(root, HPI_M3_SURFACE, 0);
	lv_obj_set_style_border_width(root, 0, 0);
	lv_obj_set_style_pad_all(root, 0, 0);
	lv_obj_set_style_pad_row(root, 0, 0);
	lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);

	hpi_ui_subbar_create(root, "Connectivity", HPI_UI_SCREEN_MORE);

	lv_obj_t *body = lv_obj_create(root);
	lv_obj_set_width(body, lv_pct(100));
	lv_obj_set_flex_grow(body, 1);
	lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(body, 0, 0);
	lv_obj_set_style_pad_all(body, HPI_M3_SPACE_3, 0);
	lv_obj_set_style_pad_row(body, HPI_M3_SPACE_3, 0);
	lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
	/* The card stack can exceed the 800 px panel; without scrolling the last
	 * card is silently clipped, i.e. unreachable. */
	hpi_m3_apply_scroll_v(body);

	/* Wi-Fi (live). */
	lv_obj_t *whdr;
	lv_obj_t *wc = link_card(body, HPI_SYM_WIFI, HPI_M3_PRIMARY, "WI-FI", &whdr);
	s_ln.wifi_pill = chip(whdr, "OFF", HPI_M3_ON_SURFACE_VARIANT);
	s_ln.wifi_sw = toggle(whdr, false, &s_ln.wifi_knob);
	lv_obj_add_flag(s_ln.wifi_sw, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(s_ln.wifi_sw, wifi_toggle_cb, LV_EVENT_CLICKED, NULL);
	s_ln.wifi_detail = lv_label_create(wc);
	lv_label_set_text(s_ln.wifi_detail, "Off");
	lv_obj_set_style_text_font(s_ln.wifi_detail, HPI_M3_FONT_MONO, 0);
	lv_obj_set_style_text_color(s_ln.wifi_detail, HPI_M3_ON_SURFACE_MUTED, 0);
	lv_label_set_long_mode(s_ln.wifi_detail, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(s_ln.wifi_detail, lv_pct(100));

	/* Provisioning: a full-width button inside the Wi-Fi card. */
	lv_obj_t *sap = lv_obj_create(wc);
	lv_obj_set_width(sap, lv_pct(100));
	lv_obj_set_height(sap, LV_SIZE_CONTENT);
	hpi_m3_apply_card(sap, HPI_M3_SURFACE, HPI_M3_RADIUS_PILL);
	hpi_m3_apply_touch(sap);
	lv_obj_set_style_border_width(sap, 1, 0);
	lv_obj_set_style_border_color(sap, HPI_M3_PRIMARY, 0);
	lv_obj_set_style_pad_ver(sap, HPI_M3_SPACE_2, 0);
	lv_obj_clear_flag(sap, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(sap, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(sap, softap_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *sapl = lv_label_create(sap);
	lv_label_set_text(sapl, "SET UP WI-FI");
	lv_obj_set_style_text_font(sapl, HPI_M3_FONT_CAPS_SM, 0);
	lv_obj_set_style_text_color(sapl, HPI_M3_PRIMARY, 0);
	lv_obj_center(sapl);

	/* BLE (live -- the co-processor handles BLE_ADV_START/_STOP). */
	lv_obj_t *bhdr;
	lv_obj_t *bc = link_card(body, HPI_SYM_BT, HPI_M3_PRIMARY, "BLE", &bhdr);
	s_ln.ble_pill = chip(bhdr, "OFF", HPI_M3_ON_SURFACE_VARIANT);
	s_ln.ble_sw = toggle(bhdr, false, &s_ln.ble_knob);
	lv_obj_add_flag(s_ln.ble_sw, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(s_ln.ble_sw, ble_toggle_cb, LV_EVENT_CLICKED, NULL);
	s_ln.ble_detail = lv_label_create(bc);
	lv_label_set_text(s_ln.ble_detail, "Not advertising");
	lv_obj_set_style_text_font(s_ln.ble_detail, HPI_M3_FONT_MONO, 0);
	lv_obj_set_style_text_color(s_ln.ble_detail, HPI_M3_ON_SURFACE_MUTED, 0);

	/* USB CDC (capability — composite always enumerated). */
	lv_obj_t *uhdr;
	link_card(body, HPI_SYM_USB, HPI_M3_ON_SURFACE_VARIANT, "USB CDC", &uhdr);
	chip(uhdr, "COMPOSITE", HPI_M3_SUCCESS);

	/* Footer note. */
	lv_obj_t *note = lv_label_create(body);
	lv_label_set_text(note, "Streaming pauses automatically while recording to SD.");
	lv_obj_set_style_text_font(note, HPI_M3_FONT_LABEL, 0);
	lv_obj_set_style_text_color(note, HPI_M3_ON_SURFACE_FAINT, 0);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, lv_pct(100));
	lv_obj_set_style_margin_top(note, LV_SIZE_CONTENT, 0);

	hpi_scr_link_refresh();
	return root;
}

static void pill_set(lv_obj_t *pill, const char *text, lv_color_t col)
{
	lv_label_set_text(pill, text);
	lv_obj_set_style_text_color(pill, col, 0);
}

/*
 * Called ~2 Hz from ui_module, on the LVGL thread. Reads only the connectivity
 * service's cache -- it must never block or issue a round-trip to the
 * co-processor (an absent one costs the driver's full 300 ms timeout).
 */
void hpi_scr_link_refresh(void)
{
	struct hpi_conn_status st;
	char buf[80];

	if (s_ln.wifi_pill == NULL) {
		return;
	}
	hpi_connectivity_get_status(&st);

	toggle_set(s_ln.wifi_sw, s_ln.wifi_knob, (st.radios & HPI_CONN_RADIO_WIFI) != 0);
	toggle_set(s_ln.ble_sw, s_ln.ble_knob, (st.radios & HPI_CONN_RADIO_BLE) != 0);

	/* ---- Wi-Fi ---- */
	if (st.link_state == HPI_CONN_LINK_OFF) {
		/* The normal resting state, not an error: the device boots this
		 * way on purpose. Say what to do, not what is broken. */
		pill_set(s_ln.wifi_pill, "OFF", HPI_M3_ON_SURFACE_VARIANT);
		lv_label_set_text(s_ln.wifi_detail,
				  "Radio off. Turn it on when you need it — it is the "
				  "largest draw on the battery.");
	} else if (st.link_state == HPI_CONN_LINK_STARTING) {
		pill_set(s_ln.wifi_pill, "STARTING", HPI_M3_WARNING);
		lv_label_set_text(s_ln.wifi_detail, "Powering the co-processor...");
	} else if (st.link_state == HPI_CONN_LINK_FAULT) {
		pill_set(s_ln.wifi_pill, "NO LINK", HPI_M3_ERROR);
		lv_label_set_text(s_ln.wifi_detail, "Co-processor is not responding");
	} else if (st.wifi_state == HPI_CONN_WIFI_CONNECTED) {
		pill_set(s_ln.wifi_pill, "CONNECTED", HPI_M3_SUCCESS);
		snprintf(buf, sizeof(buf), "%s  %d dBm  %u.%u.%u.%u", st.ssid, st.rssi,
			 st.ip[0], st.ip[1], st.ip[2], st.ip[3]);
		lv_label_set_text(s_ln.wifi_detail, buf);
	} else if (st.wifi_state == HPI_CONN_WIFI_CONNECTING) {
		pill_set(s_ln.wifi_pill, "CONNECTING", HPI_M3_WARNING);
		lv_label_set_text(s_ln.wifi_detail, "Associating...");
	} else if (st.wifi_state == HPI_CONN_WIFI_AP_MODE) {
		pill_set(s_ln.wifi_pill, "SETUP", HPI_M3_PRIMARY);
		lv_label_set_text(s_ln.wifi_detail,
				  "Join the \"HealthyPi-...\" network, then open "
				  "192.168.4.1 to choose your Wi-Fi.");
	} else if (st.wifi_state == HPI_CONN_WIFI_ERROR) {
		pill_set(s_ln.wifi_pill, "ERROR", HPI_M3_ERROR);
		lv_label_set_text(s_ln.wifi_detail, "The radio failed to start");
	} else {
		pill_set(s_ln.wifi_pill, "IDLE", HPI_M3_ON_SURFACE_VARIANT);
		lv_label_set_text(s_ln.wifi_detail,
				  (st.radios & HPI_CONN_RADIO_WIFI)
					  ? "No stored network — use SET UP WI-FI"
					  : "Radio off");
	}

	/* ---- BLE ---- */
	if (st.link_state != HPI_CONN_LINK_UP) {
		pill_set(s_ln.ble_pill, "OFF", HPI_M3_ON_SURFACE_VARIANT);
		lv_label_set_text(s_ln.ble_detail, "Not advertising");
	} else if (st.ble_conn) {
		pill_set(s_ln.ble_pill, "CONNECTED", HPI_M3_SUCCESS);
		lv_label_set_text(s_ln.ble_detail, "A device is receiving vitals");
	} else if (st.ble_adv) {
		pill_set(s_ln.ble_pill, "ADVERTISING", HPI_M3_PRIMARY);
		lv_label_set_text(s_ln.ble_detail, "Discoverable — open the app to pair");
	} else {
		pill_set(s_ln.ble_pill, "OFF", HPI_M3_ON_SURFACE_VARIANT);
		lv_label_set_text(s_ln.ble_detail, "Not advertising");
	}
}
