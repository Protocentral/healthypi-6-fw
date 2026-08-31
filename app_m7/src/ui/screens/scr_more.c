/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * "More" submenu + generic placeholder screen.
 * LVGL 9, direct style setters only (no static lv_style_t). Every color/font/
 * shape comes from the HealthyPi 6 palette tokens in theme/hpi_m3_theme.h.
 */
#include "scr_more.h"
#include "../components/hpi_ui_components.h"
#include "../theme/hpi_m3_theme.h"
#include "../ui_module.h"

/* One tappable row in the More submenu: an outlined card with a title that
 * routes to `target` on click. */
static void more_row_cb(lv_event_t *e)
{
	intptr_t target = (intptr_t)lv_event_get_user_data(e);
	hpi_ui_show_screen((enum hpi_ui_screen)target);
}

static void more_add_row(lv_obj_t *list, const char *title, enum hpi_ui_screen target)
{
	lv_obj_t *row = lv_button_create(list);
	lv_obj_set_width(row, lv_pct(100));
	lv_obj_set_height(row, LV_SIZE_CONTENT);
	hpi_m3_apply_card(row, HPI_M3_SURFACE_CONTAINER, HPI_M3_RADIUS_MD);
	hpi_m3_apply_touch(row);   /* content sizing alone gave ~44 px rows */
	lv_obj_add_event_cb(row, more_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)target);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);

	lv_obj_t *t = lv_label_create(row);
	lv_label_set_text(t, title);
	lv_obj_set_style_text_font(t, HPI_M3_FONT_TITLE, 0);
	lv_obj_set_style_text_color(t, HPI_M3_ON_SURFACE, 0);

	lv_obj_t *chev = lv_label_create(row);
	lv_label_set_text(chev, LV_SYMBOL_RIGHT);
	lv_obj_set_style_text_font(chev, HPI_M3_FONT_SYMBOL, 0);
	lv_obj_set_style_text_color(chev, HPI_M3_ON_SURFACE_VARIANT, 0);
}

lv_obj_t *hpi_scr_more_create(lv_obj_t *parent)
{
	lv_obj_t *root = lv_obj_create(parent);
	lv_obj_set_size(root, lv_pct(100), lv_pct(100));
	hpi_m3_apply_card(root, HPI_M3_SURFACE, 0);
	lv_obj_set_style_border_width(root, 0, 0);
	lv_obj_set_style_pad_all(root, 0, 0);
	lv_obj_set_style_pad_row(root, 0, 0);
	lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);

	/* The status bar is fixed and the ROWS scroll under it, so the clock and
	 * battery can never scroll off the top of the screen. */
	hpi_ui_statusbar_create(root, "More");

	lv_obj_t *list = lv_obj_create(root);
	lv_obj_set_width(list, lv_pct(100));
	lv_obj_set_flex_grow(list, 1);
	lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(list, 0, 0);
	lv_obj_set_style_pad_all(list, HPI_M3_SPACE_4, 0);
	lv_obj_set_style_pad_row(list, HPI_M3_TOUCH_GAP, 0);
	hpi_m3_apply_scroll_v(list);   /* rows are taller now; do not clip the last */
	lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

	more_add_row(list, "Link",     HPI_UI_SCREEN_LINK);
	more_add_row(list, "HRV",      HPI_UI_SCREEN_HRV);
	more_add_row(list, "Settings", HPI_UI_SCREEN_SETTINGS);
	more_add_row(list, "OTA",      HPI_UI_SCREEN_OTA);

	return root;
}

/* ---- generic placeholder ---- */

static void back_cb(lv_event_t *e)
{
	intptr_t target = (intptr_t)lv_event_get_user_data(e);
	hpi_ui_show_screen((enum hpi_ui_screen)target);
}

lv_obj_t *hpi_scr_placeholder_create(lv_obj_t *parent, const char *title,
				     const char *subtitle, int back_to)
{
	lv_obj_t *root = lv_obj_create(parent);
	lv_obj_set_size(root, lv_pct(100), lv_pct(100));
	hpi_m3_apply_card(root, HPI_M3_SURFACE, 0);
	lv_obj_set_style_border_width(root, 0, 0);
	lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(root, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_all(root, HPI_M3_SPACE_4, 0);
	lv_obj_set_style_pad_row(root, HPI_M3_SPACE_2, 0);

	if (back_to >= 0) {
		lv_obj_t *back = lv_button_create(root);
		lv_obj_set_height(back, LV_SIZE_CONTENT);
		hpi_m3_apply_card(back, HPI_M3_SURFACE_CONTAINER, HPI_M3_RADIUS_PILL);
		hpi_m3_apply_touch(back);
		lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED,
				    (void *)(intptr_t)back_to);
		lv_obj_t *bl = lv_label_create(back);
		lv_label_set_text(bl, LV_SYMBOL_LEFT "  Back");
		lv_obj_set_style_text_color(bl, HPI_M3_PRIMARY, 0);
		lv_obj_set_style_text_font(bl, HPI_M3_FONT_SYMBOL, 0);
	}

	lv_obj_t *t = lv_label_create(root);
	lv_label_set_text(t, title);
	lv_obj_set_style_text_font(t, HPI_M3_FONT_HEADLINE, 0);
	lv_obj_set_style_text_color(t, HPI_M3_ON_SURFACE, 0);

	lv_obj_t *s = lv_label_create(root);
	lv_label_set_text(s, subtitle);
	lv_obj_set_style_text_font(s, HPI_M3_FONT_BODY, 0);
	lv_obj_set_style_text_color(s, HPI_M3_ON_SURFACE_VARIANT, 0);
	lv_obj_set_style_text_align(s, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_long_mode(s, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(s, lv_pct(80));

	return root;
}
