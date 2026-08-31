/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Home screen: a single focal heart-rate readout with a small live ECG
 * sparkline, a 2x2 vital-chip grid (SpO2 / Resp / Temp / HRV) and the shared
 * status bar. Full waveform lanes live on the Live tab; PPG is not drawn here.
 * Truthful-data rule: a vital that is not yet produced (0) renders a "--"
 * placeholder, never a fabricated number.
 */
#include "scr_home.h"
#include "scr_trends.h"
#include "../components/hpi_ui_components.h"
#include "../theme/hpi_m3_theme.h"
#include "../ui_module.h"

#include <stdio.h>
#include <string.h>
#include <zephyr/sys/util.h>

#include "core/sample_formats.h"

/* Sparkline depth ~5 s: the UI thread decimates ECG (~490 S/s) to ~125 Hz
 * (ui_module ECG_DECIM), so 625 points ~= 5 s. Keep in sync with ECG_DECIM. */
#define HPI_HOME_WAVE_POINTS  625
/* "No value yet" placeholder. ASCII only: the built-in LVGL Montserrat fonts
 * lack an em-dash (U+2014) — it renders as a .notdef box. */
#define NA_STR                "--"

/* TEST-ONLY layout aid: seed representative sample numbers so the numeral fonts,
 * sizes and positions can be eyeballed on the panel without live data. When set,
 * a value that isn't produced yet keeps its seeded sample instead of "--" (real
 * values still override the moment they arrive). SET BACK TO 0 for the truthful
 * "--" behaviour before validating against real signals or shipping. */
#define HPI_UI_TEST_VALUES    0

static struct hpi_ui_waveform s_ecg;

/* Focal HR + the four vital chips. The status bar (date · link · USB · battery ·
 * clock) is the shared component and needs no handles here. */
static struct {
	lv_obj_t *hr;        /* big focal HR numeral */
	lv_obj_t *src;       /* HR provenance chip: "ECG" / "PPG" */
	lv_obj_t *leadrow;   /* lead-off banner (hidden while the leads are on) */
	lv_obj_t *leadtxt;   /* "ECG LEADS OFF · RA" */
	lv_obj_t *spo2;
	lv_obj_t *resp;
	lv_obj_t *temp;
	lv_obj_t *hrv;
	uint8_t   lead_off;      /* last mask rendered */
	uint8_t   vit_flags;     /* HP6_VIT_* from the last vitals frame */
	bool      lead_primed;   /* false until the first ECG sample lands */
} s_h;

/* ---- 2x2 vital chips ---- */

/* One vital chip: [icon] [value] [unit] ......... [role].  Matches the design's
 * card (icon · big number · unit · role right-aligned). */
/* `trend` is the Trends series this chip deep-links to (tapping a vital opens
 * Trends for that vital), or HOME_CHIP_HRV to route to the HRV screen instead. */
#define HOME_CHIP_HRV  (-1)

static void chip_cb(lv_event_t *e)
{
	int t = (int)(intptr_t)lv_event_get_user_data(e);

	if (t == HOME_CHIP_HRV) {
		hpi_ui_show_screen(HPI_UI_SCREEN_HRV);
		return;
	}
	hpi_scr_trends_select((enum hpi_trend_vital)t);
	hpi_ui_show_screen(HPI_UI_SCREEN_TRENDS);
}

static void chip_create(lv_obj_t **value_out, lv_obj_t *rowparent, const char *icon,
			const char *unit, const char *role, lv_color_t accent,
			int trend)
{
	lv_obj_t *chip = lv_obj_create(rowparent);
	lv_obj_set_flex_grow(chip, 1);
	lv_obj_set_height(chip, LV_SIZE_CONTENT);
	hpi_m3_apply_card(chip, HPI_M3_SURFACE_CONTAINER, HPI_M3_RADIUS_LG);
	hpi_m3_apply_touch(chip);
	lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(chip, chip_cb, LV_EVENT_CLICKED, (void *)(intptr_t)trend);
	lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(chip, HPI_M3_SPACE_2, 0);

	lv_obj_t *ic = lv_label_create(chip);
	lv_label_set_text(ic, icon);
	lv_obj_set_style_text_font(ic, HPI_M3_FONT_ICON, 0);
	lv_obj_set_style_text_color(ic, accent, 0);

	lv_obj_t *val = lv_label_create(chip);
	lv_label_set_text(val, NA_STR);
	lv_obj_set_style_text_font(val, HPI_M3_FONT_NUMERAL, 0);
	lv_obj_set_style_text_color(val, HPI_M3_ON_SURFACE, 0);

	lv_obj_t *u = lv_label_create(chip);
	lv_label_set_text(u, unit);
	lv_obj_set_style_text_font(u, HPI_M3_FONT_CAPS_SM, 0);
	lv_obj_set_style_text_color(u, HPI_M3_ON_SURFACE_MUTED, 0);

	lv_obj_t *r = lv_label_create(chip);
	lv_label_set_text(r, role);
	lv_obj_set_style_text_font(r, HPI_M3_FONT_CAPS_SM, 0);
	lv_obj_set_style_text_color(r, HPI_M3_ON_SURFACE_FAINT, 0);
	lv_obj_set_flex_grow(r, 1);
	lv_obj_set_style_text_align(r, LV_TEXT_ALIGN_RIGHT, 0);

	*value_out = val;
}

static lv_obj_t *chip_row(lv_obj_t *parent)
{
	lv_obj_t *row = lv_obj_create(parent);
	lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_pad_all(row, 0, 0);
	lv_obj_set_style_pad_column(row, HPI_M3_SPACE_2, 0);
	lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	return row;
}

/* ---- screen ---- */

lv_obj_t *hpi_scr_home_create(lv_obj_t *parent)
{
	lv_obj_t *root = lv_obj_create(parent);
	lv_obj_set_size(root, lv_pct(100), lv_pct(100));
	hpi_m3_apply_card(root, HPI_M3_SURFACE, 0);
	lv_obj_set_style_border_width(root, 0, 0);
	lv_obj_set_style_pad_all(root, HPI_M3_SPACE_3, 0);
	lv_obj_set_style_pad_row(root, HPI_M3_SPACE_2, 0);
	lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);

	/* No title: Home's left slot carries the live date. */
	hpi_ui_statusbar_create(root, NULL);

	/* Focal block: HR caption + big numeral + ECG sparkline, vertically
	 * centred in the space above the chips (the "one focal vital" of 1b). */
	lv_obj_t *focal = lv_obj_create(root);
	lv_obj_set_width(focal, lv_pct(100));
	lv_obj_set_flex_grow(focal, 1);
	lv_obj_set_style_bg_opa(focal, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(focal, 0, 0);
	lv_obj_set_style_pad_all(focal, 0, 0);
	lv_obj_clear_flag(focal, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(focal, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(focal, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);

	/* Lead-off banner, above the focal block. Hidden while every electrode is
	 * on; when shown it names the electrodes, not just "ECG LEADS OFF". */
	s_h.leadrow = lv_obj_create(focal);
	lv_obj_set_width(s_h.leadrow, lv_pct(100));
	lv_obj_set_height(s_h.leadrow, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_color(s_h.leadrow, HPI_M3_ERROR, 0);
	lv_obj_set_style_bg_opa(s_h.leadrow, LV_OPA_10, 0);
	lv_obj_set_style_radius(s_h.leadrow, HPI_M3_RADIUS_MD, 0);
	lv_obj_set_style_border_width(s_h.leadrow, 1, 0);
	lv_obj_set_style_border_color(s_h.leadrow, HPI_M3_ERROR, 0);
	lv_obj_set_style_border_opa(s_h.leadrow, LV_OPA_40, 0);
	lv_obj_set_style_pad_all(s_h.leadrow, HPI_M3_SPACE_2, 0);
	lv_obj_set_style_pad_column(s_h.leadrow, HPI_M3_SPACE_2, 0);
	lv_obj_set_style_margin_bottom(s_h.leadrow, HPI_M3_SPACE_3, 0);
	lv_obj_clear_flag(s_h.leadrow, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(s_h.leadrow, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(s_h.leadrow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	lv_obj_add_flag(s_h.leadrow, LV_OBJ_FLAG_HIDDEN);

	lv_obj_t *leadic = lv_label_create(s_h.leadrow);
	lv_label_set_text(leadic, HPI_SYM_WARN);
	lv_obj_set_style_text_font(leadic, HPI_M3_FONT_ICON, 0);
	lv_obj_set_style_text_color(leadic, HPI_M3_ERROR, 0);

	s_h.leadtxt = lv_label_create(s_h.leadrow);
	lv_label_set_text(s_h.leadtxt, "ECG LEADS OFF");
	lv_obj_set_style_text_font(s_h.leadtxt, HPI_M3_FONT_CAPS, 0);
	lv_obj_set_style_text_color(s_h.leadtxt, HPI_M3_ERROR, 0);

	/* Caption row: amber heart icon + "HEART RATE". */
	lv_obj_t *caprow = lv_obj_create(focal);
	lv_obj_set_size(caprow, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(caprow, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(caprow, 0, 0);
	lv_obj_set_style_pad_all(caprow, 0, 0);
	lv_obj_set_style_pad_column(caprow, HPI_M3_SPACE_2, 0);
	lv_obj_clear_flag(caprow, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(caprow, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(caprow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);

	lv_obj_t *capic = lv_label_create(caprow);
	lv_label_set_text(capic, HPI_SYM_HR);
	lv_obj_set_style_text_font(capic, HPI_M3_FONT_ICON, 0);
	lv_obj_set_style_text_color(capic, HPI_M3_SIG_HR, 0);

	lv_obj_t *cap = lv_label_create(caprow);
	lv_label_set_text(cap, "HEART RATE");
	lv_obj_set_style_text_font(cap, HPI_M3_FONT_CAPS, 0);
	lv_obj_set_style_text_color(cap, HPI_M3_ON_SURFACE_VARIANT, 0);
	lv_obj_set_style_text_letter_space(cap, 3, 0);   /* design caps tracking */

	lv_obj_t *hrrow = lv_obj_create(focal);
	lv_obj_set_size(hrrow, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(hrrow, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(hrrow, 0, 0);
	lv_obj_set_style_pad_all(hrrow, 0, 0);
	lv_obj_set_style_pad_column(hrrow, HPI_M3_SPACE_2, 0);
	lv_obj_clear_flag(hrrow, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(hrrow, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(hrrow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END,
			      LV_FLEX_ALIGN_END);

	s_h.hr = lv_label_create(hrrow);
	lv_label_set_text(s_h.hr, NA_STR);
	lv_obj_set_style_text_font(s_h.hr, HPI_M3_FONT_NUMERAL_XL, 0);
	lv_obj_set_style_text_color(s_h.hr, HPI_M3_ON_SURFACE, 0);

	lv_obj_t *unit = lv_label_create(hrrow);
	lv_label_set_text(unit, "BPM");
	lv_obj_set_style_text_font(unit, HPI_M3_FONT_CAPS, 0);
	lv_obj_set_style_text_color(unit, HPI_M3_ON_SURFACE_MUTED, 0);

	/* Provenance chip. An ECG heart rate and a PPG pulse rate are not the same
	 * measurement, and this screen shows one big number for both — so the
	 * number says which sensor it came from, and takes that signal's colour. */
	s_h.src = lv_label_create(hrrow);
	lv_label_set_text(s_h.src, "");
	lv_obj_set_style_text_font(s_h.src, HPI_M3_FONT_CAPS_SM, 0);
	lv_obj_set_style_text_color(s_h.src, HPI_M3_ON_SURFACE_FAINT, 0);
	lv_obj_set_style_bg_color(s_h.src, HPI_M3_SURFACE_CONTAINER, 0);
	lv_obj_set_style_bg_opa(s_h.src, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(s_h.src, HPI_M3_RADIUS_XS, 0);
	lv_obj_set_style_pad_hor(s_h.src, HPI_M3_SPACE_2, 0);
	lv_obj_set_style_pad_ver(s_h.src, HPI_M3_SPACE_1, 0);
	lv_obj_set_style_margin_bottom(s_h.src, HPI_M3_SPACE_2, 0);

	/* Small live ECG sparkline (amber). Reuse the waveform component but strip
	 * its card chrome + caption so it reads as a sparkline, not a panel. */
	hpi_ui_waveform_create(&s_ecg, focal, "", HPI_M3_SIG_ECG,
			       HPI_HOME_WAVE_POINTS, -2000, 2000, 9);
	lv_obj_set_style_border_width(s_ecg.panel, 0, 0);
	lv_obj_set_style_bg_opa(s_ecg.panel, LV_OPA_TRANSP, 0);
	lv_obj_set_style_pad_all(s_ecg.panel, 0, 0);
	lv_obj_set_flex_grow(s_ecg.panel, 0);
	lv_obj_set_width(s_ecg.panel, lv_pct(90));
	lv_obj_set_height(s_ecg.panel, 56);
	lv_obj_set_style_margin_top(s_ecg.panel, 24, 0);   /* gap below the focal HR */
	lv_obj_set_style_line_opa(s_ecg.chart, LV_OPA_60, LV_PART_ITEMS);
	lv_obj_t *ecgcap = lv_obj_get_child(s_ecg.panel, 0);   /* hide empty caption */
	if (ecgcap) {
		lv_obj_add_flag(ecgcap, LV_OBJ_FLAG_HIDDEN);
	}

	/* 2x2 vital chips — the LAST item inside the centred focal group, so they
	 * sit directly under the ECG sparkline (per the design's markup), not pinned
	 * to the bottom of the screen. */
	lv_obj_t *chips = lv_obj_create(focal);
	lv_obj_set_width(chips, lv_pct(100));
	lv_obj_set_height(chips, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(chips, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(chips, 0, 0);
	lv_obj_set_style_pad_all(chips, 0, 0);
	lv_obj_set_style_pad_row(chips, HPI_M3_SPACE_3, 0);
	lv_obj_set_style_margin_top(chips, 36, 0);   /* design grid margin-top */
	lv_obj_clear_flag(chips, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(chips, LV_FLEX_FLOW_COLUMN);

	lv_obj_t *r1 = chip_row(chips);
	chip_create(&s_h.spo2, r1, HPI_SYM_SPO2, "%",           "SPO2", HPI_M3_SIG_SPO2,
		    HPI_TREND_SPO2);
	chip_create(&s_h.resp, r1, HPI_SYM_RESP, "/MIN",        "RESP", HPI_M3_SIG_RESP,
		    HPI_TREND_RESP);
	lv_obj_t *r2 = chip_row(chips);
	chip_create(&s_h.temp, r2, HPI_SYM_TEMP, "\xC2\xB0" "C", "TEMP", HPI_M3_SIG_TEMP,
		    HPI_TREND_TEMP);
	chip_create(&s_h.hrv,  r2, HPI_SYM_HRV,  "MS",          "HRV",  HPI_M3_SIG_HRV,
		    HOME_CHIP_HRV);

#if HPI_UI_TEST_VALUES
	/* Seed sample numbers (design values) for on-panel layout testing. */
	lv_label_set_text(s_h.hr,   "72");
	lv_label_set_text(s_h.spo2, "98");
	lv_label_set_text(s_h.resp, "16");
	lv_label_set_text(s_h.temp, "36.5");
	lv_label_set_text(s_h.hrv,  "48");
#endif

	return root;
}

/* Render the HR numeral + provenance chip from the last vitals frame and the
 * lead-off state. Both inputs arrive on different channels at different rates,
 * so the two callers below funnel through here rather than each doing half. */
static void render_hr_source(void)
{
	bool from_ppg = (s_h.vit_flags & HP6_VIT_HR_FROM_PPG) != 0;
	bool weak = (s_h.vit_flags & HP6_VIT_PPG_WEAK) != 0;
	bool have = strcmp(lv_label_get_text(s_h.hr), NA_STR) != 0;

	if (!have) {
		lv_label_set_text(s_h.src, "");
		return;
	}
	lv_label_set_text(s_h.src, from_ppg ? (weak ? "PPG?" : "PPG") : "ECG");
	lv_obj_set_style_text_color(s_h.src,
		from_ppg ? (weak ? HPI_M3_ON_SURFACE_MUTED : HPI_M3_SIG_PPG)
			 : HPI_M3_SIG_HR, 0);
	lv_obj_set_style_text_color(s_h.hr,
		from_ppg ? HPI_M3_SIG_PPG : HPI_M3_ON_SURFACE, 0);
}

/* Lead-off is per-sample data, so this runs ~125x/s: touch LVGL only on a real
 * transition. Setting the text unconditionally reallocates the label and
 * forces a layout every sample. */
static void render_lead_off(uint8_t mask)
{
	char names[24];
	char line[48];

	if (s_h.lead_primed && mask == s_h.lead_off) {
		return;
	}
	s_h.lead_off = mask;
	s_h.lead_primed = true;

	lv_obj_set_flag(s_h.leadrow, LV_OBJ_FLAG_HIDDEN, mask == 0);
	if (mask != 0) {
		hpi_ui_lead_off_text(mask, names, sizeof(names));
		snprintf(line, sizeof(line), "ECG LEADS OFF \xC2\xB7 %s", names);
		lv_label_set_text(s_h.leadtxt, line);
	}
	/* Dim the trace instead of letting a floating electrode's rail-to-rail
	 * artefact scroll past looking like a signal. */
	lv_obj_set_style_line_opa(s_ecg.chart, mask ? LV_OPA_20 : LV_OPA_60,
				  LV_PART_ITEMS);

	/* The vitals producer suppresses an ECG HR while the leads are off, but it
	 * only speaks when the M4 sends something. Blank the numeral here too, so
	 * a stale ECG rate cannot sit on screen next to a lead-off warning. */
	if (mask != 0 && (s_h.vit_flags & HP6_VIT_HR_FROM_PPG) == 0) {
		lv_label_set_text(s_h.hr, NA_STR);
		render_hr_source();
	}
}

void hpi_scr_home_push_ecg(int32_t lead_ii_uv, uint8_t lead_off)
{
	hpi_ui_waveform_push(&s_ecg, lead_ii_uv);
	render_lead_off(lead_off);
}

void hpi_scr_home_push_ppg(int32_t ir)
{
	ARG_UNUSED(ir);   /* PPG waveform moved to the Live tab; Home shows ECG only */
}

/* Set a value label from a real reading, or show the "no value" placeholder when
 * absent. In test-values mode an absent reading keeps the seeded sample instead
 * (real data still overrides it here). */
static void set_val(lv_obj_t *l, bool have, const char *text)
{
	if (have) {
		lv_label_set_text(l, text);
	} else {
#if !HPI_UI_TEST_VALUES
		lv_label_set_text(l, NA_STR);
#endif
	}
}

void hpi_scr_home_set_vitals(const struct hp6_vitals *v)
{
	char buf[12];

	if (s_h.hr == NULL || v == NULL) {
		return;
	}

	s_h.vit_flags = v->flags;

	buf[0] = '\0';
	if (v->hr_bpm) {
		snprintf(buf, sizeof(buf), "%u", v->hr_bpm);
	}
	set_val(s_h.hr, v->hr_bpm != 0, buf);
	render_hr_source();

	buf[0] = '\0';
	if (v->spo2_x10) {
		snprintf(buf, sizeof(buf), "%u", (v->spo2_x10 + 5u) / 10u);
	}
	set_val(s_h.spo2, v->spo2_x10 != 0, buf);

	buf[0] = '\0';
	if (v->rr_bpm) {
		snprintf(buf, sizeof(buf), "%u", v->rr_bpm);
	}
	set_val(s_h.resp, v->rr_bpm != 0, buf);

	buf[0] = '\0';
	if (v->temp_c_x100) {
		snprintf(buf, sizeof(buf), "%d.%d", v->temp_c_x100 / 100,
			 (v->temp_c_x100 % 100) / 10);
	}
	set_val(s_h.temp, v->temp_c_x100 != 0, buf);

	buf[0] = '\0';
	if (v->hrv_sdnn_ms) {
		snprintf(buf, sizeof(buf), "%u", v->hrv_sdnn_ms);
	}
	set_val(s_h.hrv, v->hrv_sdnn_ms != 0, buf);
}
