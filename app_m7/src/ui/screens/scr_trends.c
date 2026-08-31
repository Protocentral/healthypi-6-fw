/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Trends + HRV screens. Trends keeps a small rolling history of each vital
 * (fed from the same VITALS frames the rest of the UI consumes) and plots the
 * selected one with min/avg/max; per-vital tabs switch the series, the HRV tab
 * routes to the HRV screen. Range chips are visual — there is no on-device
 * history store, so the window is what the session has accumulated.
 */
#include "scr_trends.h"
#include "../components/hpi_ui_components.h"
#include "../theme/hpi_m3_theme.h"
#include "../ui_module.h"

#include <stdio.h>

#include "core/sample_formats.h"

#define TREND_N   120     /* rolling samples (~2 min at 1 Hz vitals) */
#define NA_STR    "--"

enum vital { V_HR, V_SPO2, V_RESP, V_TEMP, V_N };

static const struct {
	const char *tab;
	const char *unit;
	int32_t     ymin, ymax;
	uint16_t    scale;         /* real value = stored / scale (Temp x10) */
} VMETA[V_N] = {
	{ "HR",   "BPM",         40,  180, 1  },
	{ "SPO2", "%",           80,  100, 1  },
	{ "RESP", "/MIN",        5,   40,  1  },
	{ "TEMP", "\xC2\xB0" "C", 340, 420, 10 },
};

static lv_color_t vcol(enum vital v)
{
	switch (v) {
	case V_HR:   return HPI_M3_SIG_HR;
	case V_SPO2: return HPI_M3_SIG_SPO2;
	case V_RESP: return HPI_M3_SIG_RESP;
	default:     return HPI_M3_SIG_TEMP;
	}
}

/* Rolling history (chronological ring). Value 0 = "no sample" sentinel. */
static struct {
	int16_t  buf[V_N][TREND_N];
	uint16_t head;
	uint16_t count;
} s_hist;

static struct {
	enum vital  sel;
	lv_obj_t   *tabs[V_N + 1];   /* +1 = HRV tab */
	lv_obj_t   *chart;
	lv_chart_series_t *series;
	lv_obj_t   *stat_min, *stat_avg, *stat_max;
} s_t;

/* HRV screen numerics. */
static struct {
	lv_obj_t *sdnn, *rmssd;
} s_hrv;

static int32_t s_render[TREND_N];

void hpi_scr_trends_push_vitals(const struct hp6_vitals *v)
{
	if (v == NULL) {
		return;
	}
	int16_t vals[V_N] = {
		(int16_t)v->hr_bpm,
		(int16_t)((v->spo2_x10 + 5) / 10),
		(int16_t)v->rr_bpm,
		v->temp_c_x100,
	};
	for (int i = 0; i < V_N; i++) {
		s_hist.buf[i][s_hist.head] = vals[i];
	}
	s_hist.head = (s_hist.head + 1) % TREND_N;
	if (s_hist.count < TREND_N) {
		s_hist.count++;
	}

	if (s_hrv.sdnn) {
		char b[8];

		if (v->hrv_sdnn_ms) {
			snprintf(b, sizeof(b), "%u", v->hrv_sdnn_ms);
			lv_label_set_text(s_hrv.sdnn, b);
		} else {
			lv_label_set_text(s_hrv.sdnn, NA_STR);
		}
		if (v->hrv_rmssd_ms) {
			snprintf(b, sizeof(b), "%u", v->hrv_rmssd_ms);
			lv_label_set_text(s_hrv.rmssd, b);
		} else {
			lv_label_set_text(s_hrv.rmssd, NA_STR);
		}
	}
}

/* --- tabs --- */

/* Switch the plotted series and restyle the tab strip. Shared by the tab taps
 * and by hpi_scr_trends_select() (Home chips deep-linking in). */
static void tab_select(intptr_t idx)
{
	s_t.sel = (enum vital)idx;
	for (int i = 0; i <= V_N; i++) {
		bool on = (i == (int)idx);

		lv_obj_set_style_bg_opa(s_t.tabs[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
		lv_obj_t *l = lv_obj_get_child(s_t.tabs[i], 0);
		if (l) {
			lv_obj_set_style_text_color(l, on ? vcol((enum vital)idx)
							  : HPI_M3_ON_SURFACE_VARIANT, 0);
		}
	}
	lv_obj_set_style_bg_color(s_t.tabs[(int)idx], vcol((enum vital)idx), 0);
	lv_obj_set_style_bg_opa(s_t.tabs[(int)idx], LV_OPA_20, 0);
	lv_chart_set_series_color(s_t.chart, s_t.series, vcol(s_t.sel));
	hpi_scr_trends_refresh();
}

static void tab_cb(lv_event_t *e)
{
	intptr_t idx = (intptr_t)lv_event_get_user_data(e);

	if (idx == V_N) {                     /* HRV tab -> HRV screen */
		hpi_ui_show_screen(HPI_UI_SCREEN_HRV);
		return;
	}
	tab_select(idx);
}

void hpi_scr_trends_select(enum hpi_trend_vital v)
{
	if ((int)v < 0 || (int)v >= V_N || s_t.chart == NULL) {
		return;
	}
	tab_select((intptr_t)v);
}

static void tab_add(lv_obj_t *bar, const char *text, int idx)
{
	lv_obj_t *t = lv_button_create(bar);
	lv_obj_set_height(t, HPI_M3_TOUCH_MIN);   /* live tab: switches the series */
	lv_obj_set_style_radius(t, HPI_M3_RADIUS_PILL, 0);
	lv_obj_set_style_bg_opa(t, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(t, 1, 0);
	lv_obj_set_style_border_color(t, HPI_M3_OUTLINE, 0);
	lv_obj_set_style_pad_hor(t, HPI_M3_SPACE_3, 0);
	lv_obj_add_event_cb(t, tab_cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
	lv_obj_t *l = lv_label_create(t);
	lv_label_set_text(l, text);
	lv_obj_center(l);
	lv_obj_set_style_text_font(l, HPI_M3_FONT_CAPS_SM, 0);
	lv_obj_set_style_text_color(l, HPI_M3_ON_SURFACE_VARIANT, 0);
	s_t.tabs[idx] = t;
}

static lv_obj_t *stat_box(lv_obj_t *parent, const char *label, lv_obj_t **val)
{
	lv_obj_t *b = lv_obj_create(parent);
	lv_obj_set_flex_grow(b, 1);
	lv_obj_set_height(b, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(b, 0, 0);
	lv_obj_set_style_pad_all(b, 0, 0);
	lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	lv_obj_t *lb = lv_label_create(b);
	lv_label_set_text(lb, label);
	lv_obj_set_style_text_font(lb, HPI_M3_FONT_CAPS_SM, 0);
	lv_obj_set_style_text_color(lb, HPI_M3_ON_SURFACE_FAINT, 0);
	*val = lv_label_create(b);
	lv_label_set_text(*val, NA_STR);
	lv_obj_set_style_text_font(*val, HPI_M3_FONT_NUMERAL, 0);
	lv_obj_set_style_text_color(*val, HPI_M3_ON_SURFACE, 0);
	return b;
}

lv_obj_t *hpi_scr_trends_create(lv_obj_t *parent)
{
	lv_obj_t *root = lv_obj_create(parent);
	lv_obj_set_size(root, lv_pct(100), lv_pct(100));
	hpi_m3_apply_card(root, HPI_M3_SURFACE, 0);
	lv_obj_set_style_border_width(root, 0, 0);
	lv_obj_set_style_pad_all(root, HPI_M3_SPACE_3, 0);
	lv_obj_set_style_pad_row(root, HPI_M3_SPACE_3, 0);
	lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);

	hpi_ui_statusbar_create(root, "Trends");

	/* Vital tabs (HR/SpO2/Resp/Temp/HRV). */
	lv_obj_t *tabs = lv_obj_create(root);
	lv_obj_set_width(tabs, lv_pct(100));
	lv_obj_set_height(tabs, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(tabs, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(tabs, 0, 0);
	lv_obj_set_style_pad_all(tabs, 0, 0);
	lv_obj_set_style_pad_column(tabs, HPI_M3_SPACE_1, 0);
	lv_obj_clear_flag(tabs, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(tabs, LV_FLEX_FLOW_ROW);
	for (int i = 0; i < V_N; i++) {
		tab_add(tabs, VMETA[i].tab, i);
	}
	tab_add(tabs, "HRV", V_N);

	/* Chart card. */
	lv_obj_t *card = lv_obj_create(root);
	lv_obj_set_width(card, lv_pct(100));
	lv_obj_set_flex_grow(card, 1);
	hpi_m3_apply_card(card, HPI_M3_SURFACE_CONTAINER, HPI_M3_RADIUS_LG);
	lv_obj_set_style_pad_all(card, HPI_M3_SPACE_3, 0);
	lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(card, HPI_M3_SPACE_2, 0);

	s_t.chart = lv_chart_create(card);
	lv_obj_set_width(s_t.chart, lv_pct(100));
	lv_obj_set_flex_grow(s_t.chart, 1);
	lv_chart_set_type(s_t.chart, LV_CHART_TYPE_LINE);
	lv_chart_set_point_count(s_t.chart, TREND_N);
	lv_chart_set_div_line_count(s_t.chart, 4, 0);
	lv_obj_set_style_bg_opa(s_t.chart, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(s_t.chart, 0, 0);
	lv_obj_set_style_line_width(s_t.chart, 2, LV_PART_ITEMS);
	hpi_ui_chart_hide_points(s_t.chart);
	lv_obj_set_style_line_color(s_t.chart, HPI_M3_DIVIDER, LV_PART_MAIN);
	s_t.series = lv_chart_add_series(s_t.chart, vcol(V_HR),
					 LV_CHART_AXIS_PRIMARY_Y);

	/* min / avg / max. */
	lv_obj_t *stats = lv_obj_create(card);
	lv_obj_set_width(stats, lv_pct(100));
	lv_obj_set_height(stats, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(stats, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(stats, 0, 0);
	lv_obj_set_style_pad_all(stats, 0, 0);
	lv_obj_clear_flag(stats, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(stats, LV_FLEX_FLOW_ROW);
	stat_box(stats, "MIN", &s_t.stat_min);
	stat_box(stats, "AVG", &s_t.stat_avg);
	stat_box(stats, "MAX", &s_t.stat_max);

	s_t.sel = V_HR;
	lv_obj_set_style_bg_color(s_t.tabs[V_HR], vcol(V_HR), 0);
	lv_obj_set_style_bg_opa(s_t.tabs[V_HR], LV_OPA_20, 0);
	lv_obj_set_style_text_color(lv_obj_get_child(s_t.tabs[V_HR], 0), vcol(V_HR), 0);

	hpi_scr_trends_refresh();
	return root;
}

void hpi_scr_trends_refresh(void)
{
	if (s_t.chart == NULL) {
		return;
	}

	enum vital v = s_t.sel;
	int32_t lo = INT32_MAX, hi = INT32_MIN;
	int64_t sum = 0;
	int n = 0;

	/* Walk the ring oldest -> newest into the render buffer. */
	uint16_t start = (s_hist.head + TREND_N - s_hist.count) % TREND_N;
	for (int i = 0; i < TREND_N; i++) {
		if (i < TREND_N - s_hist.count) {
			s_render[i] = LV_CHART_POINT_NONE;
			continue;
		}
		uint16_t idx = (start + (i - (TREND_N - s_hist.count))) % TREND_N;
		int16_t raw = s_hist.buf[v][idx];

		if (raw == 0) {
			s_render[i] = LV_CHART_POINT_NONE;
			continue;
		}
		s_render[i] = raw;
		if (raw < lo) lo = raw;
		if (raw > hi) hi = raw;
		sum += raw;
		n++;
	}

	lv_chart_set_range(s_t.chart, LV_CHART_AXIS_PRIMARY_Y,
			   VMETA[v].ymin, VMETA[v].ymax);
	lv_chart_set_ext_y_array(s_t.chart, s_t.series, s_render);
	lv_chart_refresh(s_t.chart);

	char b[12];
	uint16_t sc = VMETA[v].scale;

	if (n == 0) {
		lv_label_set_text(s_t.stat_min, NA_STR);
		lv_label_set_text(s_t.stat_avg, NA_STR);
		lv_label_set_text(s_t.stat_max, NA_STR);
		return;
	}
	int32_t avg = (int32_t)(sum / n);
	if (sc == 10) {
		snprintf(b, sizeof(b), "%d.%d", lo / 10, lo % 10);
		lv_label_set_text(s_t.stat_min, b);
		snprintf(b, sizeof(b), "%d.%d", avg / 10, avg % 10);
		lv_label_set_text(s_t.stat_avg, b);
		snprintf(b, sizeof(b), "%d.%d", hi / 10, hi % 10);
		lv_label_set_text(s_t.stat_max, b);
	} else {
		snprintf(b, sizeof(b), "%d", lo);  lv_label_set_text(s_t.stat_min, b);
		snprintf(b, sizeof(b), "%d", avg); lv_label_set_text(s_t.stat_avg, b);
		snprintf(b, sizeof(b), "%d", hi);  lv_label_set_text(s_t.stat_max, b);
	}
}

/* ============================ HRV screen ============================ */

static void hrv_metric(lv_obj_t *parent, const char *name, const char *unit,
		       lv_obj_t **val)
{
	lv_obj_t *c = lv_obj_create(parent);
	lv_obj_set_width(c, lv_pct(100));
	lv_obj_set_height(c, LV_SIZE_CONTENT);
	hpi_m3_apply_card(c, HPI_M3_SURFACE_CONTAINER, HPI_M3_RADIUS_LG);
	lv_obj_set_style_pad_all(c, HPI_M3_SPACE_4, 0);
	lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END,
			      LV_FLEX_ALIGN_END);
	lv_obj_set_style_pad_column(c, HPI_M3_SPACE_2, 0);

	lv_obj_t *nm = lv_label_create(c);
	lv_label_set_text(nm, name);
	lv_obj_set_style_text_font(nm, HPI_M3_FONT_CAPS, 0);
	lv_obj_set_style_text_color(nm, HPI_M3_SIG_HRV, 0);

	*val = lv_label_create(c);
	lv_label_set_text(*val, NA_STR);
	lv_obj_set_style_text_font(*val, HPI_M3_FONT_NUMERAL, 0);
	lv_obj_set_style_text_color(*val, HPI_M3_ON_SURFACE, 0);
	lv_obj_set_flex_grow(*val, 1);
	lv_obj_set_style_text_align(*val, LV_TEXT_ALIGN_RIGHT, 0);

	lv_obj_t *un = lv_label_create(c);
	lv_label_set_text(un, unit);
	lv_obj_set_style_text_font(un, HPI_M3_FONT_CAPS_SM, 0);
	lv_obj_set_style_text_color(un, HPI_M3_ON_SURFACE_MUTED, 0);
}

lv_obj_t *hpi_scr_hrv_create(lv_obj_t *parent)
{
	lv_obj_t *root = lv_obj_create(parent);
	lv_obj_set_size(root, lv_pct(100), lv_pct(100));
	hpi_m3_apply_card(root, HPI_M3_SURFACE, 0);
	lv_obj_set_style_border_width(root, 0, 0);
	lv_obj_set_style_pad_all(root, 0, 0);
	lv_obj_set_style_pad_row(root, 0, 0);
	lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);

	/* Shared sub-screen header (back chevron + title + clock/battery). */
	hpi_ui_subbar_create(root, "HRV Analysis", HPI_UI_SCREEN_TRENDS);

	lv_obj_t *body = lv_obj_create(root);
	lv_obj_set_width(body, lv_pct(100));
	lv_obj_set_flex_grow(body, 1);
	lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(body, 0, 0);
	lv_obj_set_style_pad_all(body, HPI_M3_SPACE_4, 0);
	lv_obj_set_style_pad_row(body, HPI_M3_SPACE_3, 0);
	lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);

	hrv_metric(body, "SDNN",  "MS", &s_hrv.sdnn);
	hrv_metric(body, "RMSSD", "MS", &s_hrv.rmssd);

	lv_obj_t *note = lv_label_create(body);
	lv_label_set_text(note, "Poincare plot needs beat-to-beat RR intervals "
			  "(pending M4 export).");
	lv_obj_set_style_text_font(note, HPI_M3_FONT_LABEL, 0);
	lv_obj_set_style_text_color(note, HPI_M3_ON_SURFACE_FAINT, 0);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, lv_pct(100));
	return root;
}
