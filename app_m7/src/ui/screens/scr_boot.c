/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Boot overlay: splash + self-test. A full-screen surface layered over the
 * main UI at start-up; ui_module drives the splash -> self-test -> teardown
 * timing. The self-test rows reflect the real health snapshot
 * (hpi_health_snapshot), so nothing here is fabricated.
 */
#include "scr_boot.h"
#include "../components/hpi_ui_components.h"
#include "../theme/hpi_m3_theme.h"

#include "platform/health.h"

/* Lockup widths in device pixels. */
#define SPLASH_LOGO_W    312   /* splash          */
#define SELFTEST_LOGO_W  158   /* self-test header */

/* The five health subsystems, in display order, with friendly labels. */
static const struct {
	enum hpi_subsys id;
	const char     *name;
} SUBSYS[] = {
	{ HPI_SUBSYS_ACQ,         "Acquisition (ECG/PPG)" },
	{ HPI_SUBSYS_M4_IPC,      "M4 co-processor" },
	{ HPI_SUBSYS_STREAM,      "USB stream" },
	{ HPI_SUBSYS_RECORDING,   "SD recording" },
	{ HPI_SUBSYS_HEALTHYLINK, "HealthyLink modules" },
};
#define SUBSYS_N  ((int)(sizeof(SUBSYS) / sizeof(SUBSYS[0])))

static struct {
	lv_obj_t *root;      /* full-screen overlay */
	lv_obj_t *splash;
	lv_obj_t *test;
	lv_obj_t *icon[SUBSYS_N];   /* per-row status glyph */
} s_b;

lv_obj_t *hpi_scr_boot_create(lv_obj_t *screen)
{
	s_b.root = lv_obj_create(screen);
	lv_obj_set_size(s_b.root, lv_pct(100), lv_pct(100));
	lv_obj_set_style_bg_color(s_b.root, HPI_M3_SURFACE_DIM, 0);
	lv_obj_set_style_bg_opa(s_b.root, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(s_b.root, 0, 0);
	lv_obj_set_style_radius(s_b.root, 0, 0);
	lv_obj_set_style_pad_all(s_b.root, 0, 0);
	lv_obj_clear_flag(s_b.root, LV_OBJ_FLAG_SCROLLABLE);

	/* Splash. */
	s_b.splash = lv_obj_create(s_b.root);
	lv_obj_set_size(s_b.splash, lv_pct(100), lv_pct(100));
	lv_obj_set_style_bg_opa(s_b.splash, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(s_b.splash, 0, 0);
	lv_obj_clear_flag(s_b.splash, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(s_b.splash, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(s_b.splash, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(s_b.splash, HPI_M3_SPACE_2, 0);

	/* The lockup carries the product name; only the company line goes under. */
	hpi_ui_logo_create(s_b.splash, SPLASH_LOGO_W);

	lv_obj_t *tag = lv_label_create(s_b.splash);
	lv_label_set_text(tag, "PROTOCENTRAL ELECTRONICS");
	lv_obj_set_style_text_font(tag, HPI_M3_FONT_CAPS_SM, 0);
	lv_obj_set_style_text_color(tag, HPI_M3_ON_SURFACE_MUTED, 0);
	lv_obj_set_style_text_letter_space(tag, 3, 0);
	lv_obj_set_style_pad_top(tag, HPI_M3_SPACE_3, 0);

	s_b.test = NULL;
	return s_b.root;
}

void hpi_scr_boot_selftest(void)
{
	if (s_b.root == NULL) {
		return;
	}
	if (s_b.splash) {
		lv_obj_add_flag(s_b.splash, LV_OBJ_FLAG_HIDDEN);
	}

	s_b.test = lv_obj_create(s_b.root);
	lv_obj_set_size(s_b.test, lv_pct(100), lv_pct(100));
	lv_obj_set_style_bg_opa(s_b.test, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(s_b.test, 0, 0);
	lv_obj_set_style_pad_all(s_b.test, HPI_M3_SPACE_6, 0);
	lv_obj_set_style_pad_row(s_b.test, HPI_M3_SPACE_2, 0);
	lv_obj_clear_flag(s_b.test, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(s_b.test, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(s_b.test, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
			      LV_FLEX_ALIGN_START);

	/* Header: the lockup with a small caps caption under it. */
	hpi_ui_logo_create(s_b.test, SELFTEST_LOGO_W);

	lv_obj_t *hdr = lv_label_create(s_b.test);
	lv_label_set_text(hdr, "SELF-TEST");
	lv_obj_set_style_text_font(hdr, HPI_M3_FONT_CAPS_SM, 0);
	lv_obj_set_style_text_color(hdr, HPI_M3_ON_SURFACE_MUTED, 0);
	lv_obj_set_style_text_letter_space(hdr, 3, 0);
	lv_obj_set_style_pad_bottom(hdr, HPI_M3_SPACE_3, 0);

	struct hpi_health_report r;
	hpi_health_snapshot(&r);

	for (int i = 0; i < SUBSYS_N; i++) {
		lv_obj_t *row = lv_obj_create(s_b.test);
		lv_obj_set_width(row, lv_pct(100));
		lv_obj_set_height(row, LV_SIZE_CONTENT);
		lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
		lv_obj_set_style_border_width(row, 1, 0);
		lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
		lv_obj_set_style_border_color(row, HPI_M3_DIVIDER, 0);
		lv_obj_set_style_pad_ver(row, HPI_M3_SPACE_2, 0);
		lv_obj_set_style_pad_hor(row, 0, 0);
		lv_obj_set_style_pad_column(row, HPI_M3_SPACE_2, 0);
		lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
				      LV_FLEX_ALIGN_CENTER);

		lv_obj_t *nm = lv_label_create(row);
		lv_label_set_text(nm, SUBSYS[i].name);
		lv_obj_set_style_text_font(nm, HPI_M3_FONT_BODY, 0);
		lv_obj_set_style_text_color(nm, HPI_M3_ON_SURFACE_VARIANT, 0);
		lv_obj_set_flex_grow(nm, 1);

		s_b.icon[i] = lv_label_create(row);
		lv_obj_set_style_text_font(s_b.icon[i], HPI_M3_FONT_ICON, 0);
	}
	hpi_scr_boot_refresh();
}

void hpi_scr_boot_refresh(void)
{
	struct hpi_health_report r;

	if (s_b.test == NULL) {
		return;
	}
	hpi_health_snapshot(&r);

	for (int i = 0; i < SUBSYS_N; i++) {
		if (s_b.icon[i] == NULL) {
			continue;
		}
		switch (r.e[SUBSYS[i].id].state) {
		case HPI_HEALTH_OK:
			lv_label_set_text(s_b.icon[i], HPI_SYM_CHECK);
			lv_obj_set_style_text_color(s_b.icon[i], HPI_M3_SUCCESS, 0);
			break;
		case HPI_HEALTH_DEGRADED:
			lv_label_set_text(s_b.icon[i], HPI_SYM_CHECK);
			lv_obj_set_style_text_color(s_b.icon[i], HPI_M3_WARNING, 0);
			break;
		case HPI_HEALTH_FAILED:
			lv_label_set_text(s_b.icon[i], HPI_SYM_PENDING);
			lv_obj_set_style_text_color(s_b.icon[i], HPI_M3_ERROR, 0);
			break;
		default:
			lv_label_set_text(s_b.icon[i], HPI_SYM_PENDING);
			lv_obj_set_style_text_color(s_b.icon[i], HPI_M3_ON_SURFACE_FAINT, 0);
			break;
		}
	}
}

bool hpi_scr_boot_all_pass(void)
{
	struct hpi_health_report r;

	hpi_health_snapshot(&r);
	for (int i = 0; i < SUBSYS_N; i++) {
		uint8_t st = r.e[SUBSYS[i].id].state;

		if (st == HPI_HEALTH_UNKNOWN || st == HPI_HEALTH_FAILED) {
			return false;
		}
	}
	return true;
}

void hpi_scr_boot_destroy(void)
{
	if (s_b.root) {
		lv_obj_del(s_b.root);
		s_b.root = NULL;
		s_b.splash = NULL;
		s_b.test = NULL;
	}
}
