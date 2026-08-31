/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Power menu (5 s button hold). See scr_power.h for why this is a menu rather
 * than a power-off.
 */
#include "scr_power.h"
#include "../components/hpi_ui_components.h"
#include "../fonts/hpi_symbols.h"
#include "../theme/hpi_m3_theme.h"
#include "../ui_module.h"
#include "scr_ambient.h"

#include <zephyr/logging/log.h>

#include "services/config_service.h"
#include "services/dfu_service.h"
#include "services/recording_service.h"

LOG_MODULE_DECLARE(hpi_ui, CONFIG_HPI_APP_LOG_LEVEL);

static struct {
	lv_obj_t *root;
} s_pw;

bool hpi_scr_power_active(void)
{
	return s_pw.root != NULL;
}

void hpi_scr_power_hide(void)
{
	if (s_pw.root) {
		lv_obj_del(s_pw.root);
		s_pw.root = NULL;
	}
}

/* --- actions ---------------------------------------------------------- */

static void act_cancel(lv_event_t *e)
{
	ARG_UNUSED(e);
	hpi_scr_power_hide();
}

static void act_sleep(lv_event_t *e)
{
	ARG_UNUSED(e);
	hpi_scr_power_hide();
	hpi_ui_request_sleep();
}

static void act_restart(lv_event_t *e)
{
	ARG_UNUSED(e);
	LOG_WRN("power menu: restart requested");
	hpi_scr_power_hide();
	hpi_dfu_reboot();   /* does not return */
}

static void act_recovery(lv_event_t *e)
{
	ARG_UNUSED(e);
	int rc = hpi_dfu_recovery_arm();

	if (rc != 0) {
		LOG_ERR("power menu: could not arm recovery (%d)", rc);
		hpi_scr_power_hide();
		return;
	}
	LOG_WRN("power menu: rebooting into MCUboot serial recovery");
	hpi_scr_power_hide();
	hpi_dfu_reboot();   /* does not return */
}

static void act_stop_rec(lv_event_t *e)
{
	ARG_UNUSED(e);
	LOG_INF("power menu: stopping recording");
	(void)hpi_recording_stop();
	hpi_scr_power_hide();
}

/* --- build ------------------------------------------------------------ */

/* One full-width menu row. Destructive entries take the error colour so a
 * reboot does not look like the same weight of choice as sleep. */
static void row(lv_obj_t *parent, const char *icon, const lv_font_t *icon_font,
		const char *text, lv_color_t accent, lv_event_cb_t cb)
{
	lv_obj_t *b = lv_button_create(parent);

	lv_obj_set_width(b, lv_pct(100));
	lv_obj_set_height(b, LV_SIZE_CONTENT);
	hpi_m3_apply_card(b, HPI_M3_SURFACE_CONTAINER, HPI_M3_RADIUS_LG);
	hpi_m3_apply_touch(b);
	lv_obj_set_style_pad_all(b, HPI_M3_SPACE_4, 0);
	lv_obj_set_style_pad_column(b, HPI_M3_SPACE_3, 0);
	lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *ic = lv_label_create(b);

	lv_label_set_text(ic, icon);
	lv_obj_set_style_text_font(ic, icon_font, 0);
	lv_obj_set_style_text_color(ic, accent, 0);

	lv_obj_t *l = lv_label_create(b);

	lv_label_set_text(l, text);
	lv_obj_set_style_text_font(l, HPI_M3_FONT_TITLE, 0);
	lv_obj_set_style_text_color(l, HPI_M3_ON_SURFACE, 0);
}

void hpi_scr_power_show(lv_obj_t *screen)
{
	if (s_pw.root) {
		return;
	}

	s_pw.root = lv_obj_create(screen);
	lv_obj_set_size(s_pw.root, lv_pct(100), lv_pct(100));
	/* Scrim, not an opaque fill: the menu is modal but the user should still
	 * see they are on top of something and can back out. */
	lv_obj_set_style_bg_color(s_pw.root, HPI_M3_SURFACE_DIM, 0);
	lv_obj_set_style_bg_opa(s_pw.root, LV_OPA_90, 0);
	lv_obj_set_style_border_width(s_pw.root, 0, 0);
	lv_obj_set_style_radius(s_pw.root, 0, 0);
	lv_obj_set_style_pad_all(s_pw.root, HPI_M3_SPACE_4, 0);
	lv_obj_set_style_pad_row(s_pw.root, HPI_M3_TOUCH_GAP, 0);
	lv_obj_clear_flag(s_pw.root, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(s_pw.root, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(s_pw.root, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);

	lv_obj_t *title = lv_label_create(s_pw.root);

	lv_label_set_text(title, "POWER");
	lv_obj_set_style_text_font(title, HPI_M3_FONT_CAPS, 0);
	lv_obj_set_style_text_color(title, HPI_M3_ON_SURFACE_VARIANT, 0);
	lv_obj_set_style_text_letter_space(title, 4, 0);

	if (hpi_recording_active()) {
		row(s_pw.root, HPI_SYM_REC, HPI_M3_FONT_ICON, "Stop recording",
		    HPI_M3_ERROR, act_stop_rec);
	}
	row(s_pw.root, HPI_SYM_SLEEP, HPI_M3_FONT_ICON, "Sleep", HPI_M3_PRIMARY,
	    act_sleep);
	row(s_pw.root, HPI_SYM_POWER, HPI_M3_FONT_ICON, "Restart", HPI_M3_WARNING,
	    act_restart);
	if (hpi_dfu_recovery_available()) {
		row(s_pw.root, HPI_SYM_DOWNLOAD, HPI_M3_FONT_ICON, "Recovery mode",
		    HPI_M3_WARNING, act_recovery);
	}
	/* No close glyph in the Material Symbols subset; LV_SYMBOL_CLOSE comes
	 * from the Montserrat carrier face instead of regenerating the font. */
	row(s_pw.root, LV_SYMBOL_CLOSE, HPI_M3_FONT_SYMBOL, "Cancel",
	    HPI_M3_ON_SURFACE_VARIANT, act_cancel);

	/* Say what the slide switch owns, so the absence of "Power off" reads as
	 * deliberate rather than missing. */
	lv_obj_t *note = lv_label_create(s_pw.root);

	lv_label_set_text(note, "Use the slide switch to power off");
	lv_obj_set_style_text_font(note, HPI_M3_FONT_CAPS_SM, 0);
	lv_obj_set_style_text_color(note, HPI_M3_ON_SURFACE_FAINT, 0);
	lv_obj_set_style_margin_top(note, HPI_M3_SPACE_4, 0);
}
