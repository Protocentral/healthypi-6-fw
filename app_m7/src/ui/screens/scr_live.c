/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Live screen. Three waveform lanes (ECG · PPG · Resp) with inline numerics, a
 * control row (Pause · zoom · sweep · leads-OK) and the shared status bar.
 * Truthful-data rule: unproduced numerics render "--"; Pause freezes the sweep
 * only (acquisition is untouched).
 */
#include "scr_live.h"
#include "../components/hpi_ui_components.h"
#include "../theme/hpi_m3_theme.h"

#include <stdio.h>
#include <zephyr/sys/util.h>

#include "core/sample_formats.h"

/* ~2.4 s window: ECG/PPG are decimated to ~125 Hz upstream, so 300 pts ~= 2.4 s. */
#define HPI_LIVE_WAVE_POINTS  300
#define NA_STR                "--"
#define HPI_UI_TEST_VALUES    0   /* keep in step with scr_home.c (layout testing) */

struct live_lane {
	struct hpi_ui_waveform wf;
	lv_obj_t *value;
};

/* Zoom and sweep steps. Never label these in mV/division or mm/s: the waveform
 * component auto-scales to fill its lane and the firmware does not know the
 * panel's physical width, so only a multiplier on the auto-scaled amplitude
 * and a window width in samples can be stated truthfully. */
static const int32_t ZOOM_Q8[]     = { 128,    256,  512,  1024 };
static const char *const ZOOM_LBL[] = { "x0.5", "x1", "x2", "x4" };
#define ZOOM_N  ((int)ARRAY_SIZE(ZOOM_Q8))

/* Window in chart points; ~125 Hz push rate upstream, so points/125 = seconds. */
static const uint16_t SWEEP_PTS[]    = { 600,   300,    150,    75 };
static const char *const SWEEP_LBL[] = { "5 s", "2.4 s", "1.2 s", "0.6 s" };
#define SWEEP_N  ((int)ARRAY_SIZE(SWEEP_PTS))

static struct {
	struct live_lane ecg, ppg, resp;
	lv_obj_t *leads;        /* "LEADS OK" / "LEADS OFF · RA" */
	lv_obj_t *pause_lbl;
	lv_obj_t *zoom_lbl, *sweep_lbl;
	uint8_t   zoom_i, sweep_i;
	bool      paused;
	int16_t   leads_shown;  /* last mask rendered; -1 = nothing yet */
	uint8_t   vit_flags;    /* HP6_VIT_* from the last vitals frame */
} s_l;

/* ---- control row ---- */

static void pause_cb(lv_event_t *e)
{
	ARG_UNUSED(e);
	s_l.paused = !s_l.paused;
	lv_label_set_text(s_l.pause_lbl, s_l.paused ? "RESUME" : "PAUSE");
}

/* An outlined pill showing a read-only setting (gain / sweep). */
/* Apply the current zoom/sweep step to all three lanes. */
static void zoom_apply(void)
{
	int32_t q = ZOOM_Q8[s_l.zoom_i];

	hpi_ui_waveform_set_zoom(&s_l.ecg.wf, q);
	hpi_ui_waveform_set_zoom(&s_l.ppg.wf, q);
	hpi_ui_waveform_set_zoom(&s_l.resp.wf, q);
	lv_label_set_text(s_l.zoom_lbl, ZOOM_LBL[s_l.zoom_i]);
}

static void sweep_apply(void)
{
	uint16_t n = SWEEP_PTS[s_l.sweep_i];

	hpi_ui_waveform_set_points(&s_l.ecg.wf, n);
	hpi_ui_waveform_set_points(&s_l.ppg.wf, n);
	hpi_ui_waveform_set_points(&s_l.resp.wf, n);
	lv_label_set_text(s_l.sweep_lbl, SWEEP_LBL[s_l.sweep_i]);
}

static void zoom_cb(lv_event_t *e)
{
	ARG_UNUSED(e);
	s_l.zoom_i = (uint8_t)((s_l.zoom_i + 1) % ZOOM_N);
	zoom_apply();
}

static void sweep_cb(lv_event_t *e)
{
	ARG_UNUSED(e);
	s_l.sweep_i = (uint8_t)((s_l.sweep_i + 1) % SWEEP_N);
	sweep_apply();
}

/* Tappable pill that cycles a setting. Returns the value label so the caller can
 * relabel it as the value changes. */
static lv_obj_t *pill_cycle(lv_obj_t *row, const char *text, lv_event_cb_t cb)
{
	lv_obj_t *p = lv_button_create(row);

	lv_obj_set_size(p, LV_SIZE_CONTENT, HPI_M3_TOUCH_MIN);
	lv_obj_set_style_bg_opa(p, LV_OPA_TRANSP, 0);
	lv_obj_set_style_radius(p, HPI_M3_RADIUS_PILL, 0);
	lv_obj_set_style_border_width(p, 1, 0);
	lv_obj_set_style_border_color(p, HPI_M3_OUTLINE, 0);
	lv_obj_set_style_pad_hor(p, HPI_M3_SPACE_3, 0);
	lv_obj_set_style_pad_ver(p, 0, 0);
	lv_obj_add_event_cb(p, cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *l = lv_label_create(p);

	lv_label_set_text(l, text);
	lv_obj_center(l);
	lv_obj_set_style_text_font(l, HPI_M3_FONT_MONO, 0);
	lv_obj_set_style_text_color(l, HPI_M3_ON_SURFACE_VARIANT, 0);
	return l;
}

static void controls_create(lv_obj_t *root)
{
	lv_obj_t *row = lv_obj_create(root);
	lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_pad_hor(row, HPI_M3_SPACE_3, 0);
	lv_obj_set_style_pad_ver(row, HPI_M3_SPACE_1, 0);
	lv_obj_set_style_pad_column(row, HPI_M3_SPACE_2, 0);
	lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);

	/* Pause (functional). */
	lv_obj_t *pause = lv_button_create(row);
	lv_obj_set_height(pause, HPI_M3_TOUCH_MIN);
	hpi_m3_apply_card(pause, HPI_M3_PRIMARY_CONTAINER, HPI_M3_RADIUS_PILL);
	lv_obj_set_style_border_width(pause, 0, 0);
	lv_obj_set_style_pad_hor(pause, HPI_M3_SPACE_3, 0);
	lv_obj_add_event_cb(pause, pause_cb, LV_EVENT_CLICKED, NULL);
	s_l.pause_lbl = lv_label_create(pause);
	lv_label_set_text(s_l.pause_lbl, "PAUSE");
	lv_obj_center(s_l.pause_lbl);
	lv_obj_set_style_text_font(s_l.pause_lbl, HPI_M3_FONT_CAPS_SM, 0);
	lv_obj_set_style_text_color(s_l.pause_lbl, HPI_M3_PRIMARY_LIGHT, 0);

	/* Zoom / sweep — live; both cycle their step on tap. Applied to all three
	 * lanes at once, and only to the display: acquisition, the stream and the
	 * recording are untouched. */
	s_l.zoom_lbl  = pill_cycle(row, ZOOM_LBL[s_l.zoom_i], zoom_cb);
	s_l.sweep_lbl = pill_cycle(row, SWEEP_LBL[s_l.sweep_i], sweep_cb);

	/* Leads-OK indicator (right-aligned, live from lead_off). */
	lv_obj_t *lead = lv_obj_create(row);
	lv_obj_set_size(lead, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(lead, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(lead, 0, 0);
	lv_obj_set_style_pad_all(lead, 0, 0);
	lv_obj_set_style_pad_column(lead, HPI_M3_SPACE_1, 0);
	lv_obj_set_style_margin_left(lead, LV_SIZE_CONTENT, 0);
	lv_obj_set_flex_grow(lead, 1);
	lv_obj_clear_flag(lead, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(lead, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(lead, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	s_l.leads = lv_label_create(lead);
	s_l.leads_shown = 0;   /* matches the text/colour set just below */
	lv_label_set_text(s_l.leads, "LEADS OK");
	lv_obj_set_style_text_font(s_l.leads, HPI_M3_FONT_CAPS_SM, 0);
	lv_obj_set_style_text_color(s_l.leads, HPI_M3_SUCCESS, 0);
}

/* ---- one waveform lane: header (dot/label/value/unit) + waveform ---- */

static void lane_create(struct live_lane *ln, lv_obj_t *parent, const char *label,
			const char *unit, lv_color_t color, uint8_t bl_shift,
			int32_t yseed, bool border_bottom)
{
	lv_obj_t *lane = lv_obj_create(parent);
	lv_obj_set_width(lane, lv_pct(100));
	lv_obj_set_flex_grow(lane, 1);
	lv_obj_set_style_bg_opa(lane, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(lane, border_bottom ? 1 : 0, 0);
	lv_obj_set_style_border_side(lane, LV_BORDER_SIDE_BOTTOM, 0);
	lv_obj_set_style_border_color(lane, HPI_M3_OUTLINE, 0);
	lv_obj_set_style_pad_hor(lane, HPI_M3_SPACE_3, 0);
	lv_obj_set_style_pad_ver(lane, HPI_M3_SPACE_1, 0);
	lv_obj_clear_flag(lane, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(lane, LV_FLEX_FLOW_COLUMN);

	lv_obj_t *hdr = lv_obj_create(lane);
	lv_obj_set_width(hdr, lv_pct(100));
	lv_obj_set_height(hdr, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(hdr, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(hdr, 0, 0);
	lv_obj_set_style_pad_all(hdr, 0, 0);
	lv_obj_set_style_pad_column(hdr, HPI_M3_SPACE_2, 0);
	lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END,
			      LV_FLEX_ALIGN_END);

	lv_obj_t *lbl = lv_label_create(hdr);
	lv_label_set_text(lbl, label);
	lv_obj_set_style_text_font(lbl, HPI_M3_FONT_CAPS, 0);
	lv_obj_set_style_text_color(lbl, color, 0);
	lv_obj_set_style_text_letter_space(lbl, 2, 0);

	ln->value = lv_label_create(hdr);
	lv_label_set_text(ln->value, NA_STR);
	lv_obj_set_style_text_font(ln->value, HPI_M3_FONT_NUMERAL, 0);
	lv_obj_set_style_text_color(ln->value, color, 0);
	lv_obj_set_flex_grow(ln->value, 1);
	lv_obj_set_style_text_align(ln->value, LV_TEXT_ALIGN_RIGHT, 0);

	lv_obj_t *un = lv_label_create(hdr);
	lv_label_set_text(un, unit);
	lv_obj_set_style_text_font(un, HPI_M3_FONT_CAPS_SM, 0);
	lv_obj_set_style_text_color(un, HPI_M3_ON_SURFACE_MUTED, 0);

	hpi_ui_waveform_create(&ln->wf, lane, "", color, HPI_LIVE_WAVE_POINTS,
			       -yseed, yseed, bl_shift);
	lv_obj_set_style_border_width(ln->wf.panel, 0, 0);
	lv_obj_set_style_bg_opa(ln->wf.panel, LV_OPA_TRANSP, 0);
	lv_obj_set_style_pad_all(ln->wf.panel, 0, 0);
	lv_obj_set_flex_grow(ln->wf.panel, 1);
	lv_obj_t *cap = lv_obj_get_child(ln->wf.panel, 0);   /* hide empty caption */
	if (cap) {
		lv_obj_add_flag(cap, LV_OBJ_FLAG_HIDDEN);
	}
}

/* ---- screen ---- */

lv_obj_t *hpi_scr_live_create(lv_obj_t *parent)
{
	lv_obj_t *root = lv_obj_create(parent);
	lv_obj_set_size(root, lv_pct(100), lv_pct(100));
	hpi_m3_apply_card(root, HPI_M3_SURFACE, 0);
	lv_obj_set_style_border_width(root, 0, 0);
	lv_obj_set_style_pad_all(root, 0, 0);
	lv_obj_set_style_pad_row(root, 0, 0);
	lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);

	/* Start at x1 zoom and at the SWEEP_PTS entry matching the window the lanes
	 * are built with — found by search so editing either table cannot desync it. */
	s_l.zoom_i = 0;
	s_l.sweep_i = 0;
	for (int i = 0; i < SWEEP_N; i++) {
		if (SWEEP_PTS[i] == HPI_LIVE_WAVE_POINTS) {
			s_l.sweep_i = (uint8_t)i;
			break;
		}
	}

	hpi_ui_statusbar_create(root, "Live Waveforms");
	controls_create(root);

	lv_obj_t *lanes = lv_obj_create(root);
	lv_obj_set_width(lanes, lv_pct(100));
	lv_obj_set_flex_grow(lanes, 1);
	lv_obj_set_style_bg_opa(lanes, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(lanes, 0, 0);
	lv_obj_set_style_pad_all(lanes, 0, 0);
	lv_obj_set_style_pad_row(lanes, 0, 0);
	lv_obj_clear_flag(lanes, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(lanes, LV_FLEX_FLOW_COLUMN);

	/* ECG steadier baseline -> slow DC-block (9); PPG/Resp wander -> fast (5/6). */
	lane_create(&s_l.ecg,  lanes, "ECG \xC2\xB7 LEAD II", "BPM",   HPI_M3_SIG_ECG,  9, 2000,   true);
	lane_create(&s_l.ppg,  lanes, "PPG \xC2\xB7 IR",      "% SPO2", HPI_M3_SIG_PPG,  5, 200000, true);
	lane_create(&s_l.resp, lanes, "RESP \xC2\xB7 BIOZ",   "/MIN",  HPI_M3_SIG_RESP, 6, 50000,  false);

#if HPI_UI_TEST_VALUES
	lv_label_set_text(s_l.ecg.value,  "72");
	lv_label_set_text(s_l.ppg.value,  "98");
	lv_label_set_text(s_l.resp.value, "16");
#endif

	return root;
}

void hpi_scr_live_push_ecg(int32_t lead_ii_uv, int32_t resp_uv, uint8_t lead_off)
{
	if (s_l.paused) {
		return;
	}
	hpi_ui_waveform_push(&s_l.ecg.wf, lead_ii_uv);
	hpi_ui_waveform_push(&s_l.resp.wf, resp_uv);

	/* Touch LVGL only on a real transition. This runs once per ECG *sample*
	 * (~125/s after decimation); an unconditional set_text/style call per
	 * sample costs more than the producer takes to make one and starves
	 * lv_timer_handler(). The producer debounces the mask, so a transition
	 * here is a real one. */
	if ((int16_t)lead_off == s_l.leads_shown) {
		return;
	}
	s_l.leads_shown = (int16_t)lead_off;

	if (lead_off == 0) {
		lv_label_set_text(s_l.leads, "LEADS OK");
	} else {
		char names[24];
		char line[48];

		hpi_ui_lead_off_text(lead_off, names, sizeof(names));
		snprintf(line, sizeof(line), "LEADS OFF \xC2\xB7 %s", names);
		lv_label_set_text(s_l.leads, line);
	}
	lv_obj_set_style_text_color(s_l.leads,
				    lead_off == 0 ? HPI_M3_SUCCESS : HPI_M3_ERROR, 0);
	/* A floating electrode rails; dim the trace rather than let the artefact
	 * scroll past at full contrast as though it were a signal. */
	lv_obj_set_style_line_opa(s_l.ecg.wf.chart,
				  lead_off ? LV_OPA_20 : LV_OPA_COVER, LV_PART_ITEMS);
}

void hpi_scr_live_push_ppg(int32_t ir)
{
	if (s_l.paused) {
		return;
	}
	hpi_ui_waveform_push(&s_l.ppg.wf, ir);
}

void hpi_scr_live_set_vitals(const struct hp6_vitals *v)
{
	char buf[12];

	if (s_l.ecg.value == NULL || v == NULL) {
		return;
	}

#if HPI_UI_TEST_VALUES
	if (!v->hr_bpm && !v->spo2_x10 && !v->rr_bpm) {
		return;   /* keep seeded samples until real data arrives */
	}
#endif
	s_l.vit_flags = v->flags;

	/* The ECG lane's inline number must be an ECG-derived rate: never show a
	 * rate with HP6_VIT_HR_FROM_PPG set under the "ECG · LEAD II" header. Home
	 * presents the fallback rate with its source marked. */
	if (v->hr_bpm && (v->flags & HP6_VIT_HR_FROM_PPG) == 0) {
		snprintf(buf, sizeof(buf), "%u", v->hr_bpm);
		lv_label_set_text(s_l.ecg.value, buf);
	} else {
		lv_label_set_text(s_l.ecg.value, NA_STR);
	}
	if (v->spo2_x10) {
		snprintf(buf, sizeof(buf), "%u", (v->spo2_x10 + 5u) / 10u);
		lv_label_set_text(s_l.ppg.value, buf);
	} else {
		lv_label_set_text(s_l.ppg.value, NA_STR);
	}
	if (v->rr_bpm) {
		snprintf(buf, sizeof(buf), "%u", v->rr_bpm);
		lv_label_set_text(s_l.resp.value, buf);
	} else {
		lv_label_set_text(s_l.resp.value, NA_STR);
	}
}
