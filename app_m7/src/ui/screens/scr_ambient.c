/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Ambient clock overlay. A dim, minimal sleep face: clock + date
 * from the RTC, the latest HR, and the recording status (recording keeps running
 * in the background). ui_module drives show/hide + backlight; a touch anywhere
 * wakes (LVGL inactivity resets on input). Reads only RTC + recording status.
 */
#include "scr_ambient.h"
#include "../theme/hpi_m3_theme.h"

#include <stdio.h>
#include <zephyr/drivers/rtc.h>

#include "core/sample_formats.h"   /* HP6_VIT_* HR provenance flags */
#include "services/recording_service.h"

static struct {
	lv_obj_t *root;
	lv_obj_t *clock;
	lv_obj_t *date;
	lv_obj_t *hr;
	lv_obj_t *hr_src;   /* "PPG" when the rate is not from the ECG */
	lv_obj_t *rec;
	uint16_t  hr_bpm;
	uint8_t   hr_flags;   /* HP6_VIT_* from the last vitals frame */
} s_am;

void hpi_scr_ambient_set_hr(uint16_t hr_bpm, uint8_t flags)
{
	s_am.hr_bpm = hr_bpm;
	s_am.hr_flags = flags;
}

bool hpi_scr_ambient_active(void)
{
	return s_am.root != NULL;
}

void hpi_scr_ambient_show(lv_obj_t *screen)
{
	if (s_am.root) {
		return;
	}
	s_am.root = lv_obj_create(screen);
	lv_obj_set_size(s_am.root, lv_pct(100), lv_pct(100));
	lv_obj_set_style_bg_color(s_am.root, HPI_M3_SURFACE_DIM, 0);
	lv_obj_set_style_bg_opa(s_am.root, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(s_am.root, 0, 0);
	lv_obj_set_style_radius(s_am.root, 0, 0);
	lv_obj_set_style_pad_all(s_am.root, 0, 0);
	lv_obj_clear_flag(s_am.root, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(s_am.root, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(s_am.root, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(s_am.root, HPI_M3_SPACE_3, 0);

	s_am.clock = lv_label_create(s_am.root);
	lv_label_set_text(s_am.clock, "--:--");
	lv_obj_set_style_text_font(s_am.clock, HPI_M3_FONT_NUMERAL_XL, 0);
	lv_obj_set_style_text_color(s_am.clock, HPI_M3_ON_SURFACE_VARIANT, 0);

	s_am.date = lv_label_create(s_am.root);
	lv_label_set_text(s_am.date, "");
	lv_obj_set_style_text_font(s_am.date, HPI_M3_FONT_CAPS, 0);
	lv_obj_set_style_text_color(s_am.date, HPI_M3_ON_SURFACE_FAINT, 0);
	lv_obj_set_style_text_letter_space(s_am.date, 4, 0);

	lv_obj_t *row = lv_obj_create(s_am.root);
	lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_pad_all(row, 0, 0);
	lv_obj_set_style_pad_column(row, HPI_M3_SPACE_6, 0);
	lv_obj_set_style_margin_top(row, HPI_M3_SPACE_6, 0);
	lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);

	lv_obj_t *hrbox = lv_obj_create(row);
	lv_obj_set_size(hrbox, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(hrbox, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(hrbox, 0, 0);
	lv_obj_set_style_pad_all(hrbox, 0, 0);
	lv_obj_set_style_pad_column(hrbox, HPI_M3_SPACE_1, 0);
	lv_obj_clear_flag(hrbox, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(hrbox, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(hrbox, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	lv_obj_t *hi = lv_label_create(hrbox);
	lv_label_set_text(hi, HPI_SYM_HR);
	lv_obj_set_style_text_font(hi, HPI_M3_FONT_ICON, 0);
	lv_obj_set_style_text_color(hi, HPI_M3_ON_SURFACE_FAINT, 0);
	s_am.hr = lv_label_create(hrbox);
	lv_label_set_text(s_am.hr, "--");
	lv_obj_set_style_text_font(s_am.hr, HPI_M3_FONT_NUMERAL, 0);
	lv_obj_set_style_text_color(s_am.hr, HPI_M3_ON_SURFACE_MUTED, 0);

	/* Source marker: blank for an ECG rate, "PPG" when the ECG is unavailable
	 * (leads off / no beats) and the number is a pulse rate instead. The face
	 * stays monochrome by design, so this is a word rather than a colour. */
	s_am.hr_src = lv_label_create(hrbox);
	lv_label_set_text(s_am.hr_src, "");
	lv_obj_set_style_text_font(s_am.hr_src, HPI_M3_FONT_CAPS_SM, 0);
	lv_obj_set_style_text_color(s_am.hr_src, HPI_M3_ON_SURFACE_FAINT, 0);

	s_am.rec = lv_label_create(row);
	lv_label_set_text(s_am.rec, "");
	lv_obj_set_style_text_font(s_am.rec, HPI_M3_FONT_CAPS_SM, 0);
	lv_obj_set_style_text_color(s_am.rec, HPI_M3_ON_SURFACE_MUTED, 0);

	lv_obj_t *hint = lv_label_create(s_am.root);
	lv_label_set_text(hint, "Touch or press button to wake");
	lv_obj_set_style_text_font(hint, HPI_M3_FONT_CAPS_SM, 0);
	lv_obj_set_style_text_color(hint, HPI_M3_OUTLINE, 0);
	lv_obj_set_style_margin_top(hint, HPI_M3_SPACE_6, 0);

	hpi_scr_ambient_refresh();
}

void hpi_scr_ambient_hide(void)
{
	if (s_am.root) {
		lv_obj_del(s_am.root);
		s_am.root = NULL;
	}
}

void hpi_scr_ambient_refresh(void)
{
	static const char *const WD[7] = { "SUN", "MON", "TUE", "WED",
					   "THU", "FRI", "SAT" };
	static const char *const MO[12] = { "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
					    "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };
	char buf[24];

	if (s_am.root == NULL) {
		return;
	}

	const struct device *rtc = DEVICE_DT_GET_OR_NULL(DT_ALIAS(rtc));
	struct rtc_time t;

	if (rtc && device_is_ready(rtc) && rtc_get_time(rtc, &t) == 0) {
		snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
		lv_label_set_text(s_am.clock, buf);
		int wd = (t.tm_wday >= 0 && t.tm_wday < 7) ? t.tm_wday : 0;
		int mo = (t.tm_mon >= 0 && t.tm_mon < 12) ? t.tm_mon : 0;
		snprintf(buf, sizeof(buf), "%s %02d %s", WD[wd], t.tm_mday, MO[mo]);
		lv_label_set_text(s_am.date, buf);
	}

	if (s_am.hr_bpm) {
		snprintf(buf, sizeof(buf), "%u", s_am.hr_bpm);
		lv_label_set_text(s_am.hr, buf);
	} else {
		lv_label_set_text(s_am.hr, "--");
	}
	lv_label_set_text(s_am.hr_src,
			  (s_am.hr_bpm && (s_am.hr_flags & HP6_VIT_HR_FROM_PPG))
				  ? "PPG" : "");

	struct hpi_recording_status st;
	hpi_recording_get_status(&st);
	if (st.active) {
		uint32_t s = st.duration_ms / 1000U;

		snprintf(buf, sizeof(buf), "REC %02u:%02u:%02u",
			 s / 3600U, (s / 60U) % 60U, s % 60U);
		lv_label_set_text(s_am.rec, buf);
		lv_obj_set_style_text_color(s_am.rec, HPI_M3_ERROR, 0);
	} else {
		lv_label_set_text(s_am.rec, "");
	}
}
