/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Settings + Alert limits + OTA screens. Brightness and display sleep drive
 * the config service live; the temperature unit is visual; alert thresholds
 * are local UI state with an explicit research-use caution (HealthyPi 6 is a
 * dev kit — not a medical alarm system). OTA shows the real firmware version
 * only — there is no on-device update transport.
 */
#include "scr_settings.h"
#include "../components/hpi_ui_components.h"
#include "../theme/hpi_m3_theme.h"
#include "../ui_module.h"

#include <stdio.h>
#include <zephyr/sys/util.h>
#include <app_version.h>

#include "services/config_service.h"

/* Lockup width in device pixels. */
#define OTA_LOGO_W  224

/* ---- shared building blocks ---- */

static lv_obj_t *card(lv_obj_t *parent)
{
	lv_obj_t *c = lv_obj_create(parent);
	lv_obj_set_width(c, lv_pct(100));
	lv_obj_set_height(c, LV_SIZE_CONTENT);
	hpi_m3_apply_card(c, HPI_M3_SURFACE_CONTAINER, HPI_M3_RADIUS_LG);
	lv_obj_set_style_pad_all(c, HPI_M3_SPACE_4, 0);
	lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(c, HPI_M3_SPACE_2, 0);
	return c;
}

static lv_obj_t *body_col(lv_obj_t *root)
{
	lv_obj_t *b = lv_obj_create(root);
	lv_obj_set_width(b, lv_pct(100));
	lv_obj_set_flex_grow(b, 1);
	lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(b, 0, 0);
	lv_obj_set_style_pad_all(b, HPI_M3_SPACE_3, 0);
	lv_obj_set_style_pad_row(b, HPI_M3_TOUCH_GAP, 0);
	/* Touch-sized rows overflow 800 px on Settings and Alert; the body has to
	 * scroll or the last card is drawn off-screen and cannot be reached. */
	hpi_m3_apply_scroll_v(b);
	lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
	return b;
}

/* Row with icon + name, a growable spacer, then caller-added trailing items. */
static lv_obj_t *row_head(lv_obj_t *parent, const char *icon, lv_color_t icol,
			  const char *name)
{
	lv_obj_t *r = lv_obj_create(parent);
	lv_obj_set_width(r, lv_pct(100));
	lv_obj_set_height(r, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(r, 0, 0);
	lv_obj_set_style_pad_all(r, 0, 0);
	lv_obj_set_style_pad_column(r, HPI_M3_SPACE_2, 0);
	lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);

	lv_obj_t *ic = lv_label_create(r);
	lv_label_set_text(ic, icon);
	lv_obj_set_style_text_font(ic, HPI_M3_FONT_ICON, 0);
	lv_obj_set_style_text_color(ic, icol, 0);
	lv_obj_t *nm = lv_label_create(r);
	lv_label_set_text(nm, name);
	lv_obj_set_style_text_font(nm, HPI_M3_FONT_BODY, 0);
	lv_obj_set_style_text_color(nm, HPI_M3_ON_SURFACE, 0);
	lv_obj_t *sp = lv_obj_create(r);
	lv_obj_set_style_bg_opa(sp, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(sp, 0, 0);
	lv_obj_set_style_pad_all(sp, 0, 0);
	lv_obj_set_height(sp, 1);
	lv_obj_set_flex_grow(sp, 1);
	lv_obj_clear_flag(sp, LV_OBJ_FLAG_SCROLLABLE);
	return r;
}

static void nav_cb(lv_event_t *e)
{
	intptr_t s = (intptr_t)lv_event_get_user_data(e);
	hpi_ui_show_screen((enum hpi_ui_screen)s);
}

/* A tappable settings entry: icon + name + trailing text + chevron -> screen. */
static void entry(lv_obj_t *parent, const char *icon, lv_color_t icol,
		  const char *name, const char *trail, lv_color_t tcol, int to)
{
	lv_obj_t *c = card(parent);
	lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
	hpi_m3_apply_touch(c);
	lv_obj_add_event_cb(c, nav_cb, LV_EVENT_CLICKED, (void *)(intptr_t)to);
	lv_obj_t *r = row_head(c, icon, icol, name);
	if (trail) {
		lv_obj_t *t = lv_label_create(r);
		lv_label_set_text(t, trail);
		lv_obj_set_style_text_font(t, HPI_M3_FONT_MONO, 0);
		lv_obj_set_style_text_color(t, tcol, 0);
	}
	lv_obj_t *ch = lv_label_create(r);
	lv_label_set_text(ch, HPI_SYM_CHEV);
	lv_obj_set_style_text_font(ch, HPI_M3_FONT_ICON, 0);
	lv_obj_set_style_text_color(ch, HPI_M3_ON_SURFACE_MUTED, 0);
}

/* Segmented control. `cb` NULL builds it as an indicator (caller should then
 * hpi_m3_apply_inert() it); otherwise each segment is tappable and calls back
 * with its index. Returns the container so the caller can restyle or disable it.
 */
static void seg_select(lv_obj_t *seg, int sel)
{
	uint32_t n = lv_obj_get_child_count(seg);

	for (uint32_t i = 0; i < n; i++) {
		lv_obj_t *s = lv_obj_get_child(seg, i);
		bool on = ((int)i == sel);

		lv_obj_set_style_bg_opa(s, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
		lv_obj_set_style_text_color(lv_obj_get_child(s, 0),
					    on ? HPI_M3_PRIMARY_LIGHT
					       : HPI_M3_ON_SURFACE_VARIANT, 0);
	}
}

static lv_obj_t *segmented(lv_obj_t *parent, const char *const *items, int n, int sel,
			   lv_event_cb_t cb)
{
	lv_obj_t *seg = lv_obj_create(parent);
	lv_obj_set_size(seg, lv_pct(100), HPI_M3_TOUCH_MIN);
	lv_obj_set_style_bg_opa(seg, LV_OPA_TRANSP, 0);
	lv_obj_set_style_radius(seg, HPI_M3_RADIUS_PILL, 0);
	lv_obj_set_style_border_width(seg, 1, 0);
	lv_obj_set_style_border_color(seg, HPI_M3_OUTLINE, 0);
	lv_obj_set_style_pad_all(seg, 0, 0);
	lv_obj_set_style_clip_corner(seg, true, 0);
	lv_obj_clear_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(seg, LV_FLEX_FLOW_ROW);
	for (int i = 0; i < n; i++) {
		lv_obj_t *s = lv_obj_create(seg);
		lv_obj_set_height(s, lv_pct(100));
		lv_obj_set_flex_grow(s, 1);
		lv_obj_set_style_bg_color(s, HPI_M3_PRIMARY_CONTAINER, 0);
		lv_obj_set_style_bg_opa(s, i == sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
		lv_obj_set_style_radius(s, 0, 0);
		lv_obj_set_style_border_width(s, i < n - 1 ? 1 : 0, 0);
		lv_obj_set_style_border_side(s, LV_BORDER_SIDE_RIGHT, 0);
		lv_obj_set_style_border_color(s, HPI_M3_OUTLINE, 0);
		lv_obj_set_style_pad_all(s, 0, 0);
		lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
		if (cb) {
			lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE);
			lv_obj_add_event_cb(s, cb, LV_EVENT_CLICKED,
					    (void *)(intptr_t)i);
		}
		lv_obj_t *l = lv_label_create(s);
		lv_label_set_text(l, items[i]);
		lv_obj_center(l);
		lv_obj_set_style_text_font(l, HPI_M3_FONT_CAPS_SM, 0);
		lv_obj_set_style_text_color(l, i == sel ? HPI_M3_PRIMARY_LIGHT
							: HPI_M3_ON_SURFACE_VARIANT, 0);
	}
	return seg;
}

/* ============================ Settings ============================ */

static struct hpi_ui_stepper s_bright;

/* One tap = one step (never an lv_slider here: its ~10 px track fires this per
 * touch sample on the LVGL thread). Both calls are non-blocking --
 * hpi_config_set_display_brightness() caches the value and schedules the /lfs
 * write on the config service's own workqueue. */
static void bright_changed(int32_t v, void *user)
{
	ARG_UNUSED(user);
	hpi_config_set_display_brightness((uint8_t)v);
	hpi_ui_set_brightness((uint8_t)v);
}

/* Display sleep: the idle timeout before the ambient clock takes over. 0 keeps
 * the display up indefinitely. Kept as a table so the labels and the seconds
 * cannot drift apart. */
static const char *const SLEEP_LABEL[] = { "30 s", "1 MIN", "5 MIN", "NEVER" };
static const uint16_t    SLEEP_SECS[]  = { 30,     60,      300,     0       };
#define SLEEP_N  ((int)ARRAY_SIZE(SLEEP_SECS))

static lv_obj_t *s_sleep_seg;

static int sleep_index(void)
{
	uint16_t cur = hpi_config_display_sleep_s();

	for (int i = 0; i < SLEEP_N; i++) {
		if (SLEEP_SECS[i] == cur) {
			return i;
		}
	}
	return 1;   /* stored value is not one of the offered steps -> show 1 MIN */
}

static void sleep_cb(lv_event_t *e)
{
	int i = (int)(intptr_t)lv_event_get_user_data(e);

	if (i < 0 || i >= SLEEP_N) {
		return;
	}
	hpi_config_set_display_sleep_s(SLEEP_SECS[i]);
	seg_select(s_sleep_seg, i);
}

lv_obj_t *hpi_scr_settings_create(lv_obj_t *parent)
{
	lv_obj_t *root = lv_obj_create(parent);
	lv_obj_set_size(root, lv_pct(100), lv_pct(100));
	hpi_m3_apply_card(root, HPI_M3_SURFACE, 0);
	lv_obj_set_style_border_width(root, 0, 0);
	lv_obj_set_style_pad_all(root, 0, 0);
	lv_obj_set_style_pad_row(root, 0, 0);
	lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);

	hpi_ui_subbar_create(root, "Settings", HPI_UI_SCREEN_MORE);
	lv_obj_t *body = body_col(root);

	/* Brightness (live), 10% steps over 10..100. The ambient dim level (5%) is
	 * set programmatically and is deliberately not reachable here. */
	lv_obj_t *bc = card(body);
	row_head(bc, HPI_SYM_BRIGHT, HPI_M3_CTA, "Brightness");
	hpi_ui_stepper_create(&s_bright, bc, 10, 100, 10,
			      hpi_config_display_brightness(), "%d%%",
			      bright_changed, NULL);
	lv_obj_set_width(s_bright.root, lv_pct(100));

	/* Display sleep (live) -- drives the UI's ambient-clock idle timeout. */
	lv_obj_t *sc = card(body);
	row_head(sc, HPI_SYM_SLEEP, HPI_M3_PRIMARY, "Display sleep");
	s_sleep_seg = segmented(sc, SLEEP_LABEL, SLEEP_N, sleep_index(),
				sleep_cb);

	/* Temperature unit -- inert: there is no temperature producer yet, so the
	 * Temp chip reads "--" and there is nothing for a unit to apply to. */
	lv_obj_t *tc = card(body);
	lv_obj_t *tr = row_head(tc, HPI_SYM_TEMP, HPI_M3_SIG_TEMP, "Temperature unit");
	static const char *const UNIT[] = { "\xC2\xB0" "C", "\xC2\xB0" "F" };
	lv_obj_t *useg = lv_obj_create(tr);
	lv_obj_set_size(useg, 2 * HPI_M3_TOUCH_MIN, HPI_M3_TOUCH_MIN);
	lv_obj_set_style_bg_opa(useg, LV_OPA_TRANSP, 0);
	lv_obj_set_style_radius(useg, HPI_M3_RADIUS_PILL, 0);
	lv_obj_set_style_border_width(useg, 1, 0);
	lv_obj_set_style_border_color(useg, HPI_M3_OUTLINE, 0);
	lv_obj_set_style_pad_all(useg, 0, 0);
	lv_obj_set_style_clip_corner(useg, true, 0);
	lv_obj_clear_flag(useg, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(useg, LV_FLEX_FLOW_ROW);
	for (int i = 0; i < 2; i++) {
		lv_obj_t *s = lv_obj_create(useg);
		lv_obj_set_height(s, lv_pct(100));
		lv_obj_set_flex_grow(s, 1);
		lv_obj_set_style_bg_color(s, HPI_M3_PRIMARY_CONTAINER, 0);
		lv_obj_set_style_bg_opa(s, i == 0 ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
		lv_obj_set_style_radius(s, 0, 0);
		lv_obj_set_style_border_width(s, 0, 0);
		lv_obj_set_style_pad_all(s, 0, 0);
		lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_t *l = lv_label_create(s);
		lv_label_set_text(l, UNIT[i]);
		lv_obj_center(l);
		lv_obj_set_style_text_font(l, HPI_M3_FONT_CAPS_SM, 0);
		lv_obj_set_style_text_color(l, i == 0 ? HPI_M3_PRIMARY_LIGHT
						      : HPI_M3_ON_SURFACE_VARIANT, 0);
	}
	hpi_m3_apply_inert(useg);

	/* Entries. */
	entry(body, HPI_SYM_ALERT, HPI_M3_ERROR, "Alert limits", "2 ACTIVE",
	      HPI_M3_ON_SURFACE_MUTED, HPI_UI_SCREEN_ALERT);
	entry(body, HPI_SYM_DOWNLOAD, HPI_M3_ON_SURFACE_VARIANT, "About & updates",
	      "v" APP_VERSION_STRING, HPI_M3_ON_SURFACE_MUTED,
	      HPI_UI_SCREEN_OTA);
	return root;
}

/* ============================ Alert limits ============================ */

/* Local UI state only — these are not wired to an alarm engine (see the caution
 * on the screen). The stepper owns the value; this keeps the handle so a future
 * consumer can read it back. */
static struct hpi_ui_stepper s_lim[4];

static void limit_row(lv_obj_t *parent, const char *icon, lv_color_t icol,
		      const char *name, int idx, int32_t init, bool on)
{
	lv_obj_t *r = lv_obj_create(parent);
	lv_obj_set_width(r, lv_pct(100));
	lv_obj_set_height(r, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(r, 1, 0);
	lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
	lv_obj_set_style_border_color(r, HPI_M3_DIVIDER, 0);
	lv_obj_set_style_pad_ver(r, HPI_M3_SPACE_2, 0);
	lv_obj_set_style_pad_hor(r, 0, 0);
	lv_obj_set_style_pad_column(r, HPI_M3_SPACE_2, 0);
	lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);

	lv_obj_t *ic = lv_label_create(r);
	lv_label_set_text(ic, icon);
	lv_obj_set_style_text_font(ic, HPI_M3_FONT_ICON, 0);
	lv_obj_set_style_text_color(ic, on ? icol : HPI_M3_ON_SURFACE_FAINT, 0);
	lv_obj_t *nm = lv_label_create(r);
	lv_label_set_text(nm, name);
	lv_obj_set_style_text_font(nm, HPI_M3_FONT_BODY, 0);
	lv_obj_set_style_text_color(nm, on ? HPI_M3_ON_SURFACE
					   : HPI_M3_ON_SURFACE_VARIANT, 0);
	lv_obj_t *sp = lv_obj_create(r);
	lv_obj_set_style_bg_opa(sp, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(sp, 0, 0);
	lv_obj_set_height(sp, 1);
	lv_obj_set_flex_grow(sp, 1);
	lv_obj_clear_flag(sp, LV_OBJ_FLAG_SCROLLABLE);

	/* Shared [ - ] value [ + ] stepper. */
	hpi_ui_stepper_create(&s_lim[idx], r, 0, 300, 1, init, "%d", NULL, NULL);
	lv_obj_set_style_text_color(s_lim[idx].value,
				    on ? HPI_M3_ON_SURFACE : HPI_M3_ON_SURFACE_VARIANT, 0);
}

lv_obj_t *hpi_scr_alert_create(lv_obj_t *parent)
{
	lv_obj_t *root = lv_obj_create(parent);
	lv_obj_set_size(root, lv_pct(100), lv_pct(100));
	hpi_m3_apply_card(root, HPI_M3_SURFACE, 0);
	lv_obj_set_style_border_width(root, 0, 0);
	lv_obj_set_style_pad_all(root, 0, 0);
	lv_obj_set_style_pad_row(root, 0, 0);
	lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);

	hpi_ui_subbar_create(root, "Alert Limits", HPI_UI_SCREEN_SETTINGS);
	lv_obj_t *body = body_col(root);

	/* Research-use caution banner. */
	lv_obj_t *warn = lv_obj_create(body);
	lv_obj_set_width(warn, lv_pct(100));
	lv_obj_set_height(warn, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_color(warn, HPI_M3_ERROR, 0);
	lv_obj_set_style_bg_opa(warn, LV_OPA_10, 0);
	lv_obj_set_style_radius(warn, HPI_M3_RADIUS_MD, 0);
	lv_obj_set_style_border_width(warn, 1, 0);
	lv_obj_set_style_border_color(warn, HPI_M3_ERROR, 0);
	lv_obj_set_style_border_opa(warn, LV_OPA_40, 0);
	lv_obj_set_style_pad_all(warn, HPI_M3_SPACE_3, 0);
	lv_obj_set_style_pad_column(warn, HPI_M3_SPACE_2, 0);
	lv_obj_clear_flag(warn, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(warn, LV_FLEX_FLOW_ROW);
	lv_obj_t *wi = lv_label_create(warn);
	lv_label_set_text(wi, HPI_SYM_EMERG);
	lv_obj_set_style_text_font(wi, HPI_M3_FONT_ICON, 0);
	lv_obj_set_style_text_color(wi, HPI_M3_ERROR, 0);
	lv_obj_t *wt = lv_label_create(warn);
	lv_label_set_text(wt, "HealthyPi 6 is a development kit. Alerts are visual "
			  "aids for bench work \xE2\x80\x94 not a medical alarm "
			  "system (no IEC 60601-1-8 conformance).");
	lv_obj_set_style_text_font(wt, HPI_M3_FONT_LABEL, 0);
	lv_obj_set_style_text_color(wt, HPI_M3_ERROR, 0);
	lv_label_set_long_mode(wt, LV_LABEL_LONG_WRAP);
	lv_obj_set_flex_grow(wt, 1);

	/* Thresholds. */
	lv_obj_t *lc = card(body);
	limit_row(lc, HPI_SYM_HR,   HPI_M3_SIG_HR,   "HR high",  0, 120, true);
	limit_row(lc, HPI_SYM_HR,   HPI_M3_SIG_HR,   "HR low",   1, 50,  true);
	limit_row(lc, HPI_SYM_SPO2, HPI_M3_SIG_SPO2, "SpO2 low", 2, 90,  true);
	limit_row(lc, HPI_SYM_RESP, HPI_M3_SIG_RESP, "Resp high", 3, 30, false);
	return root;
}

/* ============================ OTA ============================ */

lv_obj_t *hpi_scr_ota_create(lv_obj_t *parent)
{
	lv_obj_t *root = lv_obj_create(parent);
	lv_obj_set_size(root, lv_pct(100), lv_pct(100));
	hpi_m3_apply_card(root, HPI_M3_SURFACE, 0);
	lv_obj_set_style_border_width(root, 0, 0);
	lv_obj_set_style_pad_all(root, 0, 0);
	lv_obj_set_style_pad_row(root, 0, 0);
	lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);

	hpi_ui_subbar_create(root, "Firmware Update", HPI_UI_SCREEN_SETTINGS);

	lv_obj_t *body = lv_obj_create(root);
	lv_obj_set_width(body, lv_pct(100));
	lv_obj_set_flex_grow(body, 1);
	lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(body, 0, 0);
	lv_obj_set_style_pad_hor(body, HPI_M3_SPACE_6, 0);
	lv_obj_set_style_pad_row(body, HPI_M3_SPACE_2, 0);
	lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(body, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);

	/* Brand lockup; the version line sits straight under. */
	hpi_ui_logo_create(body, OTA_LOGO_W);

	lv_obj_t *ver = lv_label_create(body);
	lv_label_set_text(ver, "v" APP_VERSION_STRING);
	lv_obj_set_style_text_font(ver, HPI_M3_FONT_MONO, 0);
	lv_obj_set_style_text_color(ver, HPI_M3_ON_SURFACE_MUTED, 0);

	/* There is no on-device update check and no transport to do one over, so
	 * never claim "up to date" -- state only what is actually known. */
	lv_obj_t *status = lv_label_create(body);
	lv_label_set_text(status, "NO ON-DEVICE UPDATE CHECK");
	lv_obj_set_style_text_font(status, HPI_M3_FONT_CAPS, 0);
	lv_obj_set_style_text_color(status, HPI_M3_ON_SURFACE_MUTED, 0);
	lv_obj_set_style_margin_top(status, HPI_M3_SPACE_4, 0);

	/* Power caution. */
	lv_obj_t *warn = lv_obj_create(body);
	lv_obj_set_width(warn, lv_pct(100));
	lv_obj_set_height(warn, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_color(warn, HPI_M3_CTA, 0);
	lv_obj_set_style_bg_opa(warn, LV_OPA_10, 0);
	lv_obj_set_style_radius(warn, HPI_M3_RADIUS_MD, 0);
	lv_obj_set_style_border_width(warn, 1, 0);
	lv_obj_set_style_border_color(warn, HPI_M3_CTA, 0);
	lv_obj_set_style_border_opa(warn, LV_OPA_40, 0);
	lv_obj_set_style_pad_all(warn, HPI_M3_SPACE_3, 0);
	lv_obj_set_style_pad_column(warn, HPI_M3_SPACE_2, 0);
	lv_obj_set_style_margin_top(warn, HPI_M3_SPACE_4, 0);
	lv_obj_clear_flag(warn, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(warn, LV_FLEX_FLOW_ROW);
	lv_obj_t *pi = lv_label_create(warn);
	lv_label_set_text(pi, HPI_SYM_POWER);
	lv_obj_set_style_text_font(pi, HPI_M3_FONT_ICON, 0);
	lv_obj_set_style_text_color(pi, HPI_M3_CTA, 0);
	lv_obj_t *pt = lv_label_create(warn);
	lv_label_set_text(pt, "Update from a host: healthypi fw update, over the "
			  "control USB port. Wi-Fi and SD-card update are not "
			  "implemented. Keep power connected during a flash \xE2\x80\x94 "
			  "the device restarts afterward.");
	lv_obj_set_style_text_font(pt, HPI_M3_FONT_LABEL, 0);
	lv_obj_set_style_text_color(pt, HPI_M3_WARNING, 0);
	lv_label_set_long_mode(pt, LV_LABEL_LONG_WRAP);
	lv_obj_set_flex_grow(pt, 1);
	return root;
}
