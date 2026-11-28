/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Settings + Alert limits + OTA screens. Brightness and display sleep drive
 * the config service live; the theme is saved live and applied by a restart; the temperature unit is visual; alert thresholds
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
#include "services/dfu_service.h"
#include "services/recording_service.h"

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
	hpi_m3_apply_pressable(c, HPI_M3_SURFACE_CONTAINER);
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
	hpi_ui_segmented_select(s_sleep_seg, i);
}

/* ---- theme ----------------------------------------------------------------
 * The palette is chosen once per boot (screens are built once and never
 * rebuilt), so a new choice is saved at once and APPLIED by a restart. The
 * restart is a separate, explicit tap -- picking a segment never reboots the
 * device on its own -- and it is refused while a recording is running, which
 * a reboot would cut off unclosed. */
static const char *const THEME_LABEL[HPI_CONFIG_THEME_COUNT] = { "DARK", "LIGHT" };

static struct {
	lv_obj_t   *seg;
	lv_obj_t   *restart;   /* shown only while the choice != the running theme */
	lv_obj_t   *note;
	lv_timer_t *wait;      /* polls for the save to land before rebooting */
	uint8_t     waited;
} s_th;

#define THEME_SAVE_POLL_MS   50
#define THEME_SAVE_POLL_MAX  40   /* 2 s, then reboot anyway */

static void theme_note(const char *text)
{
	lv_label_set_text(s_th.note, text);
	lv_obj_clear_flag(s_th.note, LV_OBJ_FLAG_HIDDEN);
}

static void theme_wait_cb(lv_timer_t *t)
{
	if (!hpi_config_display_theme_saved() && ++s_th.waited < THEME_SAVE_POLL_MAX) {
		return;
	}
	lv_timer_delete(t);
	s_th.wait = NULL;
	hpi_dfu_reboot();   /* does not return */
}

static void theme_restart_cb(lv_event_t *e)
{
	ARG_UNUSED(e);
	struct hpi_recording_status st = {0};

	if (s_th.wait) {
		return;   /* already restarting */
	}
	hpi_recording_get_status(&st);
	if (st.active) {
		theme_note("Stop the recording first. The theme is saved and will "
			   "apply at the next restart.");
		return;
	}
	theme_note("Restarting...");
	lv_obj_add_state(s_th.restart, LV_STATE_DISABLED);
	s_th.waited = 0;
	s_th.wait = lv_timer_create(theme_wait_cb, THEME_SAVE_POLL_MS, NULL);
}

static void theme_cb(lv_event_t *e)
{
	int i = (int)(intptr_t)lv_event_get_user_data(e);

	if (i < 0 || i >= HPI_CONFIG_THEME_COUNT || s_th.wait) {
		return;
	}
	if (hpi_config_set_display_theme((uint8_t)i) != 0) {
		return;
	}
	hpi_ui_segmented_select(s_th.seg, i);

	bool pending = (i != (int)hpi_m3_theme_active());

	if (pending) {
		lv_obj_clear_flag(s_th.restart, LV_OBJ_FLAG_HIDDEN);
		theme_note("Saved. Restart to apply.");
	} else {
		lv_obj_add_flag(s_th.restart, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(s_th.note, LV_OBJ_FLAG_HIDDEN);
	}
}

static void theme_card(lv_obj_t *body)
{
	lv_obj_t *c = card(body);

	row_head(c, HPI_SYM_BRIGHT, HPI_M3_PRIMARY, "Theme");
	/* Show the stored choice; it equals the running theme unless the device
	 * was changed over MCUmgr since boot, which the restart button covers. */
	int stored = hpi_config_display_theme();

	s_th.seg = hpi_ui_segmented_create(c, THEME_LABEL, HPI_CONFIG_THEME_COUNT,
					   stored, theme_cb);

	s_th.note = lv_label_create(c);
	lv_obj_set_width(s_th.note, lv_pct(100));
	lv_label_set_long_mode(s_th.note, LV_LABEL_LONG_WRAP);
	lv_obj_set_style_text_font(s_th.note, HPI_M3_FONT_CAPS_SM, 0);
	lv_obj_set_style_text_color(s_th.note, HPI_M3_ON_SURFACE_VARIANT, 0);
	lv_obj_add_flag(s_th.note, LV_OBJ_FLAG_HIDDEN);

	s_th.restart = hpi_ui_button_create(c, HPI_UI_BTN_FILLED, NULL,
					    "RESTART TO APPLY", theme_restart_cb, NULL);
	lv_obj_set_width(s_th.restart, lv_pct(100));
	if (stored == (int)hpi_m3_theme_active()) {
		lv_obj_add_flag(s_th.restart, LV_OBJ_FLAG_HIDDEN);
	}
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
	s_sleep_seg = hpi_ui_segmented_create(sc, SLEEP_LABEL, SLEEP_N,
					      sleep_index(), sleep_cb);

	theme_card(body);

	/* Temperature unit -- inert: there is no temperature producer yet, so the
	 * Temp chip reads "--" and there is nothing for a unit to apply to. */
	lv_obj_t *tc = card(body);
	lv_obj_t *tr = row_head(tc, HPI_SYM_TEMP, HPI_M3_SIG_TEMP, "Temperature unit");
	static const char *const UNIT[] = { "\xC2\xB0" "C", "\xC2\xB0" "F" };
	lv_obj_t *useg = hpi_ui_segmented_create(tr, UNIT, 2, 0, NULL);
	lv_obj_set_width(useg, 2 * HPI_M3_TOUCH_MIN);
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
