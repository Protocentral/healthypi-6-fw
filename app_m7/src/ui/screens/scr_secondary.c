/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Record screen — a recording_service client: START/STOP drive
 * hpi_recording_start()/stop() and the labels mirror hpi_recording_status.
 * Never touches the sample bus; refreshed from the service by the UI thread
 * (~2 Hz), so every LVGL call here runs on the UI thread.
 */
#include "scr_secondary.h"
#include "../components/hpi_ui_components.h"
#include "../theme/hpi_m3_theme.h"
#include "../ui_module.h"

#include <stdio.h>
#include <errno.h>
#include <stdint.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>

#include "services/recording_service.h"
#include "platform/fs_mount.h"
#include "bus/hpi_events.h"
#include "services/power_service.h"
#include "core/channel_registry.h"
#include "scr_recording.h"

/* ============================ Record screen ============================
 *
 * One screen, two views toggled by the recording state: setup (channels,
 * duration, SD free space, START, BROWSE) and active (elapsed, size/events,
 * PAUSE, MARK EVENT, STOP). Every control drives the recording service; the
 * screen keeps no recording state of its own beyond what the controls show.
 * MARK EVENT publishes HPI_EVT_USER_MARK (the hardware short-press mirrors
 * it). */

/* Selectable channels, in display order. Respiration is part of the ECG
 * sample, so it shares the ECG switch. */
#define REC_CHAN_COUNT 3
static const uint8_t s_rec_chan_id[REC_CHAN_COUNT] = {
	HPI_CH_ECG, HPI_CH_PPG, HPI_CH_VITALS,
};

/* Duration choices; 0 = continuous (no auto-stop). */
#define REC_DUR_COUNT 4
static const uint32_t s_rec_dur_s[REC_DUR_COUNT] = { 30U, 300U, 3600U, 0U };
#define REC_DUR_DEFAULT 3   /* continuous */

static struct {
	lv_obj_t   *setup;       /* idle view container   */
	lv_obj_t   *active;      /* recording view container */
	lv_obj_t   *bar;         /* shared status bar (retitled per state) */
	lv_obj_t   *recdot;      /* status-bar red dot (recording) */
	lv_obj_t   *sd_free;     /* setup: "N / M GB free" */
	lv_obj_t   *sd_bar;      /* setup: used-fraction fill */
	lv_obj_t   *err;         /* setup: start error line */
	lv_obj_t   *elapsed;     /* active: HH:MM:SS */
	lv_obj_t   *meta;        /* active: "N MB · K events" */
	lv_obj_t   *fname;       /* active: filename */
	lv_obj_t   *batt_overlay; /* low-battery overlay */
	lv_obj_t   *err_overlay;  /* recording-error banner (I/O fault / card eject) */
	bool        stop_armed;  /* stop confirm armed */
	lv_timer_t *stop_timer;  /* one-shot confirm timer */
	lv_obj_t   *chan_sw[REC_CHAN_COUNT];  /* switch tracks, visual only */
	bool        chan_on[REC_CHAN_COUNT];
	lv_obj_t   *dur_seg;     /* shared segmented control */
	int         dur_idx;
	bool        mark_long_active; /* suppress click after long-press */
	lv_obj_t   *pause_btn;
	lv_obj_t   *stop_btn;
} s_rec;

/* Modal note-entry overlay (long-press MARK EVENT). NULL when closed. */
static lv_obj_t *s_note_overlay;

static void note_close(void)
{
	if (s_note_overlay) {
		/* async: this can be called from the keyboard's own event callback */
		lv_obj_delete_async(s_note_overlay);
		s_note_overlay = NULL;
	}
	s_rec.mark_long_active = false;
}

static void note_cancel_cb(lv_event_t *e)
{
	ARG_UNUSED(e);
	note_close();   /* no mark is recorded */
}

static void rec_stop_cb(lv_event_t *e);
static void err_overlay_click_cb(lv_event_t *e);
static void setup_err_show(const char *msg);
static void note_kb_cb(lv_event_t *ev);
/* --- small building blocks --- */

static lv_obj_t *rec_card(lv_obj_t *parent)
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

static void section_label(lv_obj_t *parent, const char *text)
{
	lv_obj_t *l = lv_label_create(parent);
	lv_label_set_text(l, text);
	lv_obj_set_style_text_font(l, HPI_M3_FONT_CAPS_SM, 0);
	lv_obj_set_style_text_color(l, HPI_M3_ON_SURFACE_VARIANT, 0);
	lv_obj_set_style_text_letter_space(l, 2, 0);
}

static void pause_btn_cb(lv_event_t *e)
{
	ARG_UNUSED(e);
	struct hpi_recording_status st;

	/* The service owns the pause state (and the auto-stop, which counts
	 * recorded time only); the button label follows it in refresh. */
	hpi_recording_get_status(&st);
	(void)hpi_recording_pause(!st.paused);
	hpi_scr_record_refresh();
}

static void stop_confirm_timeout(lv_timer_t *t)
{
	lv_obj_t *btn = (lv_obj_t *)lv_timer_get_user_data(t);
	s_rec.stop_armed = false;
	if (btn) {
		lv_label_set_text(hpi_ui_button_label(btn), "STOP & SAVE");
	}
	lv_timer_del(t);
	s_rec.stop_timer = NULL;
}

static void stop_btn_cb(lv_event_t *e)
{
	lv_obj_t *btn = lv_event_get_target(e);
	if (!s_rec.stop_armed) {
		s_rec.stop_armed = true;
		lv_label_set_text(hpi_ui_button_label(btn), "CONFIRM");
		s_rec.stop_timer = lv_timer_create(stop_confirm_timeout, 2000, btn);
		lv_timer_set_user_data(s_rec.stop_timer, btn);
		return;
	}
	/* confirmed */
	if (s_rec.stop_timer) {
		lv_timer_del(s_rec.stop_timer);
		s_rec.stop_timer = NULL;
	}
	s_rec.stop_armed = false;
	rec_stop_cb(e);
}

static void channel_toggle_cb(lv_event_t *e);

/* One channel row. The whole row is the touch target -- the switch track is
 * only its indicator -- so it meets HPI_M3_TOUCH_MIN without a 64 px switch.
 * Returns the switch track. */
static lv_obj_t *channel_row(lv_obj_t *parent, const char *icon, lv_color_t icol,
			     const char *name, const char *rate, int idx)
{
	lv_obj_t *row = lv_obj_create(parent);
	lv_obj_set_width(row, lv_pct(100));
	lv_obj_set_height(row, LV_SIZE_CONTENT);
	hpi_m3_apply_touch(row);
	hpi_m3_apply_pressable(row, HPI_M3_SURFACE_CONTAINER);
	lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(row, channel_toggle_cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
	lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_pad_ver(row, HPI_M3_SPACE_1, 0);
	lv_obj_set_style_pad_hor(row, 0, 0);
	lv_obj_set_style_pad_column(row, HPI_M3_SPACE_2, 0);
	lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);

	lv_obj_t *ic = lv_label_create(row);
	lv_label_set_text(ic, icon);
	lv_obj_set_style_text_font(ic, HPI_M3_FONT_ICON, 0);
	lv_obj_set_style_text_color(ic, icol, 0);

	lv_obj_t *nm = lv_label_create(row);
	lv_label_set_text(nm, name);
	lv_obj_set_style_text_font(nm, HPI_M3_FONT_BODY, 0);
	lv_obj_set_style_text_color(nm, HPI_M3_ON_SURFACE, 0);

	lv_obj_t *rt = lv_label_create(row);
	lv_label_set_text(rt, rate);
	lv_obj_set_style_text_font(rt, HPI_M3_FONT_MONO, 0);
	lv_obj_set_style_text_color(rt, HPI_M3_ON_SURFACE_MUTED, 0);
	lv_obj_set_flex_grow(rt, 1);
	lv_obj_set_style_text_align(rt, LV_TEXT_ALIGN_RIGHT, 0);

	lv_obj_t *sw = lv_obj_create(row);      /* switch track */
	lv_obj_set_size(sw, 44, 24);
	lv_obj_set_style_radius(sw, 12, 0);
	lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(sw, 0, 0);
	lv_obj_clear_flag(sw, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_clear_flag(sw, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_t *knob = lv_obj_create(sw);     /* knob */
	lv_obj_set_size(knob, 20, 20);
	lv_obj_set_style_radius(knob, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(knob, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(knob, 0, 0);
	lv_obj_clear_flag(knob, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_clear_flag(knob, LV_OBJ_FLAG_CLICKABLE);
	return sw;
}

/* Paint a switch track for its on/off state. */
static void channel_switch_paint(lv_obj_t *sw, bool on)
{
	lv_obj_set_style_bg_color(sw, on ? HPI_M3_SWITCH_TRACK_ON : HPI_M3_SWITCH_TRACK_OFF, 0);
	lv_obj_t *knob = lv_obj_get_child(sw, 0);
	if (knob) {
		lv_obj_align(knob, on ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, on ? -2 : 2, 0);
		lv_obj_set_style_bg_color(knob, on ? HPI_M3_SWITCH_KNOB_ON : HPI_M3_SWITCH_KNOB_OFF, 0);
	}
}

/* A full-width Rec action. Looks come from the shared button table
 * (HPI_M3_BTN_* in the theme); child 1 is the label the handlers relabel. */
static lv_obj_t *rec_button(lv_obj_t *parent, const char *icon, const char *text,
			    enum hpi_ui_btn_kind kind, lv_event_cb_t cb)
{
	lv_obj_t *b = hpi_ui_button_create(parent, kind, icon, text, cb, NULL);

	lv_obj_set_width(b, lv_pct(100));   /* START/STOP/MARK */
	return b;
}

static void channel_toggle_cb(lv_event_t *e)
{
	int idx = (int)(intptr_t)lv_event_get_user_data(e);

	if (idx < 0 || idx >= REC_CHAN_COUNT) {
		return;
	}
	bool on = !s_rec.chan_on[idx];

	/* Refused while recording (the setup view is hidden then anyway). */
	if (hpi_recording_set_channel_enabled(s_rec_chan_id[idx], on) != 0) {
		return;
	}
	s_rec.chan_on[idx] = on;
	channel_switch_paint(s_rec.chan_sw[idx], on);
}

static void duration_select_cb(lv_event_t *e)
{
	int idx = (int)(intptr_t)lv_event_get_user_data(e);

	if (idx < 0 || idx >= REC_DUR_COUNT) {
		return;
	}
	s_rec.dur_idx = idx;
	hpi_ui_segmented_select(s_rec.dur_seg, idx);
}

/* --- callbacks --- */

static void rec_start_cb(lv_event_t *e)
{
	ARG_UNUSED(e);

	/* Require at least one channel selected -- otherwise the recording
	 * would start and just capture nothing meaningful. Check the UI's
	 * own toggle state rather than hpi_recording_get_channel_mask(),
	 * since that's what the person actually sees checked/unchecked. */
	bool any_channel_on = false;
	for (int i = 0; i < REC_CHAN_COUNT; i++) {
		if (s_rec.chan_on[i]) {
			any_channel_on = true;
			break;
		}
	}
	if (!any_channel_on) {
		setup_err_show("Select at least one channel to record");
		return;   /* don't call hpi_recording_start() at all */
	}

	int ret = hpi_recording_start_ex(NULL, s_rec_dur_s[s_rec.dur_idx]);
	const char *msg = NULL;

	if (ret == -ENODEV) {
		msg = "SD card not ready";
	} else if (ret == -EBUSY) {
		msg = "Busy (Transfer Mode armed?)";
	} else if (ret == -ENOSPC) {
		msg = "Storage full - delete old recordings";
	} else if (ret < 0) {
		msg = "Start failed";
	}
	setup_err_show(msg);
	if (ret >= 0) {
		s_rec.stop_armed = false;
		if (s_rec.stop_timer) {
			lv_timer_del(s_rec.stop_timer);
			s_rec.stop_timer = NULL;
		}
		if (s_rec.stop_btn) {
			lv_obj_t *lbl = hpi_ui_button_label(s_rec.stop_btn);
			if (lbl) {
				lv_label_set_text(lbl, "STOP & SAVE");
			}
		}
	}
	hpi_scr_record_refresh();
}

static void rec_browse_cb(lv_event_t *e)
{
	ARG_UNUSED(e);
	/* Kick a fresh listing before switching screens so the browse view
	 * doesn't show stale results from the last time it was open. */
	hpi_scr_recording_reload();
	hpi_ui_show_screen(HPI_UI_SCREEN_RECORDINGS);
}

static void rec_stop_cb(lv_event_t *e)
{
	ARG_UNUSED(e);
	(void)hpi_recording_stop();
	hpi_scr_record_refresh();
}

/* The service owns the mark and its sequence -- it publishes an hp6_event on
 * HPI_CH_EVENT, which the recording writes in-band and the stream and ESP32
 * link also see -- so the count shown is what was actually stored, never a
 * local tally. */
static void rec_mark_cb(lv_event_t *e)
{
	ARG_UNUSED(e);
	/* If a long-press opened the keyboard just before the CLICK event, ignore
	 * this CLICK to avoid double-marking (long-press should be a single mark
	 * that is handled by the keyboard flow). */
	if (s_rec.mark_long_active) {
		s_rec.mark_long_active = false;
		return;
	}
	/* Marking while paused is supported: the service records events even
	 * while data is paused. */
	int seq = hpi_recording_mark();

	if (seq < 0) {
		return;   /* not recording at all: nothing to mark */
	}
	hpi_events_publish(HPI_EVT_USER_MARK, seq);   /* in-process notification */
	hpi_scr_record_refresh();
}

static void rec_mark_long_cb(lv_event_t *e)
{
	ARG_UNUSED(e);
	if (s_note_overlay) {
		return;   /* already open */
	}
	/* Long-press: the CLICK fired on release must not add a second mark. */
	s_rec.mark_long_active = true;

	/* Full-screen dim layer. Tapping it (outside the box/keyboard) cancels. */
	lv_obj_t *ov = lv_obj_create(lv_scr_act());
	lv_obj_set_size(ov, lv_pct(100), lv_pct(100));
	lv_obj_set_pos(ov, 0, 0);
	lv_obj_set_style_bg_color(ov, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(ov, LV_OPA_60, 0);
	lv_obj_set_style_border_width(ov, 0, 0);
	lv_obj_set_style_radius(ov, 0, 0);
	lv_obj_set_style_pad_all(ov, 0, 0);
	lv_obj_clear_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_event_cb(ov, note_cancel_cb, LV_EVENT_CLICKED, NULL);
	s_note_overlay = ov;

	lv_obj_t *ta = lv_textarea_create(ov);
	lv_obj_set_size(ta, lv_pct(64), HPI_M3_TOUCH_MIN);
	lv_obj_align(ta, LV_ALIGN_TOP_LEFT, 8, 8);
	lv_textarea_set_one_line(ta, true);
	lv_textarea_set_max_length(ta, 63);   /* the service keeps 63 bytes + NUL */
	lv_textarea_set_placeholder_text(ta, "Event note (optional)");

	/* Was a stock LVGL button: default-theme #2196F3, the one control in the
	 * UI that ignored the palette. */
	lv_obj_t *cancel = hpi_ui_button_create(ov, HPI_UI_BTN_SECONDARY, NULL,
						"CANCEL", note_cancel_cb, NULL);
	lv_obj_set_width(cancel, lv_pct(30));
	lv_obj_align(cancel, LV_ALIGN_TOP_RIGHT, -8, 8);

	lv_obj_t *kb = lv_keyboard_create(ov);
	lv_keyboard_set_textarea(kb, ta);
	/* OK saves the mark; the keyboard-icon key cancels. */
	lv_obj_add_event_cb(kb, note_kb_cb, LV_EVENT_READY, ta);
	lv_obj_add_event_cb(kb, note_kb_cb, LV_EVENT_CANCEL, ta);
}

static void note_kb_cb(lv_event_t *ev)
{
	lv_obj_t *ta = (lv_obj_t *)lv_event_get_user_data(ev);

	/* Only OK records a mark. Cancel just closes. */
	if (lv_event_get_code(ev) == LV_EVENT_READY) {
		const char *txt = ta ? lv_textarea_get_text(ta) : NULL;
		int seq = recording_add_event_text(txt ? txt : "");
		if (seq > 0) {
			hpi_events_publish(HPI_EVT_USER_MARK, seq);
			hpi_scr_record_refresh();
		}
	}
	note_close();
}

/* --- setup view --- */

static void build_setup(lv_obj_t *parent)
{
	s_rec.setup = lv_obj_create(parent);
	lv_obj_set_size(s_rec.setup, lv_pct(100), lv_pct(100));
	lv_obj_set_style_bg_opa(s_rec.setup, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(s_rec.setup, 0, 0);
	lv_obj_set_style_pad_all(s_rec.setup, HPI_M3_SPACE_3, 0);
	lv_obj_set_style_pad_row(s_rec.setup, HPI_M3_SPACE_3, 0);
	/* Scrolls: at the 64 px touch floor the stack is ~672 of 680 px, and the
	 * error line below pushes it past the end (pitfall 15). */
	hpi_m3_apply_scroll_v(s_rec.setup);
	lv_obj_set_flex_flow(s_rec.setup, LV_FLEX_FLOW_COLUMN);

	/* Channels. */
	lv_obj_t *ch = rec_card(s_rec.setup);
	section_label(ch, "CHANNELS");
	s_rec.chan_sw[0] = channel_row(ch, HPI_SYM_LIVE, HPI_M3_SIG_ECG,  "ECG + Respiration", "500 Hz", 0);
	s_rec.chan_sw[1] = channel_row(ch, HPI_SYM_SPO2, HPI_M3_SIG_SPO2, "PPG Red + IR",      "250 Hz", 1);
	s_rec.chan_sw[2] = channel_row(ch, HPI_SYM_HRV,  HPI_M3_SIG_HRV,  "Vitals",            "1 Hz",   2);
	/* Show the service's selection, which outlives this screen. */
	uint32_t mask = hpi_recording_get_channel_mask();
	for (int i = 0; i < REC_CHAN_COUNT; i++) {
		s_rec.chan_on[i] = (mask & HPI_CH_BIT(s_rec_chan_id[i])) != 0;
		channel_switch_paint(s_rec.chan_sw[i], s_rec.chan_on[i]);
	}

	/* Duration: auto-stop after this much recorded time. */
	lv_obj_t *du = rec_card(s_rec.setup);
	section_label(du, "DURATION");
	static const char *const dur_label[REC_DUR_COUNT] = { "30 s", "5 min", "1 h", "CONT" };
	s_rec.dur_idx = REC_DUR_DEFAULT;
	s_rec.dur_seg = hpi_ui_segmented_create(du, dur_label, REC_DUR_COUNT,
						REC_DUR_DEFAULT, duration_select_cb);

	/* SD card (live free space). */
	lv_obj_t *sd = rec_card(s_rec.setup);
	lv_obj_t *sdhdr = lv_obj_create(sd);
	lv_obj_set_width(sdhdr, lv_pct(100));
	lv_obj_set_height(sdhdr, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(sdhdr, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(sdhdr, 0, 0);
	lv_obj_set_style_pad_all(sdhdr, 0, 0);
	lv_obj_set_style_pad_column(sdhdr, HPI_M3_SPACE_2, 0);
	lv_obj_clear_flag(sdhdr, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(sdhdr, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(sdhdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	lv_obj_t *sdi = lv_label_create(sdhdr);
	lv_label_set_text(sdi, HPI_SYM_SD);
	lv_obj_set_style_text_font(sdi, HPI_M3_FONT_ICON, 0);
	lv_obj_set_style_text_color(sdi, HPI_M3_ON_SURFACE_VARIANT, 0);
	section_label(sdhdr, "SD CARD");
	s_rec.sd_free = lv_label_create(sdhdr);
	lv_label_set_text(s_rec.sd_free, "--");
	lv_obj_set_style_text_font(s_rec.sd_free, HPI_M3_FONT_MONO, 0);
	lv_obj_set_style_text_color(s_rec.sd_free, HPI_M3_ON_SURFACE_VARIANT, 0);
	lv_obj_set_flex_grow(s_rec.sd_free, 1);
	lv_obj_set_style_text_align(s_rec.sd_free, LV_TEXT_ALIGN_RIGHT, 0);

	lv_obj_t *track = lv_obj_create(sd);
	lv_obj_set_size(track, lv_pct(100), 6);
	lv_obj_set_style_radius(track, 3, 0);
	lv_obj_set_style_bg_color(track, HPI_M3_SURFACE_HIGH, 0);
	lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(track, 0, 0);
	lv_obj_set_style_pad_all(track, 0, 0);
	lv_obj_clear_flag(track, LV_OBJ_FLAG_SCROLLABLE);
	s_rec.sd_bar = lv_obj_create(track);
	lv_obj_set_size(s_rec.sd_bar, 0, 6);
	lv_obj_set_style_radius(s_rec.sd_bar, 3, 0);
	lv_obj_set_style_bg_color(s_rec.sd_bar, HPI_M3_PRIMARY, 0);
	lv_obj_set_style_bg_opa(s_rec.sd_bar, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(s_rec.sd_bar, 0, 0);

	/* Start error, directly above START: the stack is taller than the
	 * screen, so a line after BROWSE sat under the bottom edge and needed a
	 * scroll to read. setup_err_show() also scrolls it into view. */
	s_rec.err = lv_label_create(s_rec.setup);
	lv_label_set_text(s_rec.err, "");
	lv_obj_set_width(s_rec.err, lv_pct(100));
	lv_label_set_long_mode(s_rec.err, LV_LABEL_LONG_WRAP);
	lv_obj_set_style_text_align(s_rec.err, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(s_rec.err, HPI_M3_FONT_LABEL, 0);
	lv_obj_set_style_text_color(s_rec.err, HPI_M3_ERROR, 0);
	lv_obj_add_flag(s_rec.err, LV_OBJ_FLAG_HIDDEN);

	/* START CTA + BROWSE. */
	lv_obj_t *cta = rec_button(s_rec.setup, HPI_SYM_REC, "START RECORDING",
				   HPI_UI_BTN_CTA, rec_start_cb);
	lv_obj_set_style_margin_top(cta, HPI_M3_SPACE_2, 0);

	lv_obj_t *browse = rec_button(s_rec.setup, LV_SYMBOL_LIST,
				      "BROWSE RECORDINGS",
				      HPI_UI_BTN_SECONDARY, rec_browse_cb);
	lv_obj_set_style_margin_top(browse, HPI_M3_SPACE_2, 0);

}

/* --- active view --- */

static void build_active(lv_obj_t *parent)
{
	s_rec.active = lv_obj_create(parent);
	lv_obj_set_size(s_rec.active, lv_pct(100), lv_pct(100));
	lv_obj_set_style_bg_opa(s_rec.active, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(s_rec.active, 0, 0);
	lv_obj_set_style_pad_hor(s_rec.active, HPI_M3_SPACE_6, 0);
	lv_obj_set_style_pad_ver(s_rec.active, 0, 0);
	lv_obj_set_style_pad_row(s_rec.active, HPI_M3_SPACE_1, 0);
	lv_obj_clear_flag(s_rec.active, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(s_rec.active, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(s_rec.active, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);

	lv_obj_t *cap = lv_label_create(s_rec.active);
	lv_label_set_text(cap, "ELAPSED");
	lv_obj_set_style_text_font(cap, HPI_M3_FONT_CAPS, 0);
	lv_obj_set_style_text_color(cap, HPI_M3_ON_SURFACE_VARIANT, 0);
	lv_obj_set_style_text_letter_space(cap, 3, 0);

	s_rec.elapsed = lv_label_create(s_rec.active);
	lv_label_set_text(s_rec.elapsed, "00:00:00");
	lv_obj_set_style_text_font(s_rec.elapsed, HPI_M3_FONT_NUMERAL, 0);
	lv_obj_set_style_text_color(s_rec.elapsed, HPI_M3_ON_SURFACE, 0);

	s_rec.meta = lv_label_create(s_rec.active);
	lv_label_set_text(s_rec.meta, "0 MB");
	lv_obj_set_style_text_font(s_rec.meta, HPI_M3_FONT_MONO, 0);
	lv_obj_set_style_text_color(s_rec.meta, HPI_M3_ON_SURFACE_MUTED, 0);

	s_rec.fname = lv_label_create(s_rec.active);
	lv_label_set_text(s_rec.fname, "");
	lv_obj_set_style_text_font(s_rec.fname, HPI_M3_FONT_MONO, 0);
	lv_obj_set_style_text_color(s_rec.fname, HPI_M3_ON_SURFACE_MUTED, 0);
	lv_label_set_long_mode(s_rec.fname, LV_LABEL_LONG_DOT);
	lv_obj_set_width(s_rec.fname, lv_pct(100));
	lv_obj_set_style_text_align(s_rec.fname, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_margin_top(s_rec.fname, HPI_M3_SPACE_4, 0);

	lv_obj_t *btns = lv_obj_create(s_rec.active);
	lv_obj_set_width(btns, lv_pct(100));
	lv_obj_set_height(btns, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(btns, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(btns, 0, 0);
	lv_obj_set_style_pad_all(btns, 0, 0);
	lv_obj_set_style_pad_row(btns, HPI_M3_SPACE_3, 0);
	lv_obj_set_style_margin_top(btns, HPI_M3_SPACE_6, 0);
	lv_obj_clear_flag(btns, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_COLUMN);

	/* Pause / Resume; the label follows the service in refresh. */
	s_rec.pause_btn = rec_button(btns, LV_SYMBOL_PAUSE, "PAUSE",
				     HPI_UI_BTN_CTA, pause_btn_cb);

	/* Mark button: short press = mark, long press = note keyboard. */
	lv_obj_t *mark_btn = rec_button(btns, HPI_SYM_HRV, "MARK EVENT",
					HPI_UI_BTN_SECONDARY, rec_mark_cb);
	/* Event marks are drawn green in the recording; the icon says so. */
	lv_obj_set_style_text_color(lv_obj_get_child(mark_btn, 0), HPI_M3_SIG_RESP, 0);
	lv_obj_add_event_cb(mark_btn, rec_mark_long_cb, LV_EVENT_LONG_PRESSED, NULL);

	/* Stop with two-tap confirm: first tap arms, second tap confirms. */
	s_rec.stop_btn = rec_button(btns, HPI_SYM_REC, "STOP & SAVE",
				    HPI_UI_BTN_DANGER, stop_btn_cb);

	lv_obj_t *hint = lv_label_create(s_rec.active);
	lv_label_set_text(hint, "Hardware button: short press = mark event");
	lv_obj_set_style_text_font(hint, HPI_M3_FONT_CAPS_SM, 0);
	lv_obj_set_style_text_color(hint, HPI_M3_ON_SURFACE_FAINT, 0);
	lv_obj_set_style_margin_top(hint, HPI_M3_SPACE_3, 0);
}

/* NULL hides the line. */
static void setup_err_show(const char *msg)
{
	if (s_rec.err == NULL) {
		return;
	}
	lv_label_set_text(s_rec.err, msg ? msg : "");
	lv_obj_set_flag(s_rec.err, LV_OBJ_FLAG_HIDDEN, msg == NULL);
	if (msg != NULL) {
		lv_obj_update_layout(s_rec.setup);
		lv_obj_scroll_to_view(s_rec.err, LV_ANIM_ON);
	}
}

/* Why a recording stopped, in words. The codes are enter_error_state()'s in
 * services/recording_service.c. */
static const char *rec_error_text(int code)
{
	switch (code) {
	case -ENODEV:
		return "SD card removed - recording stopped";
	case -ENOSPC:
		return "SD card full - recording stopped";
	case -EIO:
		return "SD card write failed - recording stopped";
	default:
		return NULL;
	}
}

static void err_overlay_click_cb(lv_event_t *e)
{
	ARG_UNUSED(e);
	hpi_recording_clear_error();
	hpi_scr_record_refresh();
}

lv_obj_t *hpi_scr_record_create(lv_obj_t *parent)
{
	lv_obj_t *root = lv_obj_create(parent);
	lv_obj_set_size(root, lv_pct(100), lv_pct(100));
	hpi_m3_apply_card(root, HPI_M3_SURFACE, 0);
	lv_obj_set_style_border_width(root, 0, 0);
	lv_obj_set_style_pad_all(root, 0, 0);
	lv_obj_set_style_pad_row(root, 0, 0);
	lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);

	/* The shared status bar (title + clock + battery), with the recording dot
	 * moved in ahead of the title. */
	s_rec.bar = hpi_ui_statusbar_create(root, "New Recording");

	s_rec.recdot = lv_obj_create(hpi_ui_statusbar_left(s_rec.bar));
	lv_obj_set_size(s_rec.recdot, 8, 8);
	lv_obj_set_style_radius(s_rec.recdot, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(s_rec.recdot, HPI_M3_ERROR, 0);
	lv_obj_set_style_bg_opa(s_rec.recdot, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(s_rec.recdot, 0, 0);
	lv_obj_clear_flag(s_rec.recdot, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_move_to_index(s_rec.recdot, 0);

	/* Recording-error banner (hidden unless hpi_recording_has_error()),
	 * directly under the status bar so it is the first thing on the screen.
	 * It used to be a bare red line after the body, at the bottom edge,
	 * easy to miss. Tap to dismiss -- clears the condition so the person can
	 * try again. */
	s_rec.err_overlay = lv_label_create(root);
	lv_label_set_text(s_rec.err_overlay, "");
	lv_obj_set_width(s_rec.err_overlay, lv_pct(100));
	lv_label_set_long_mode(s_rec.err_overlay, LV_LABEL_LONG_WRAP);
	lv_obj_set_style_text_align(s_rec.err_overlay, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(s_rec.err_overlay, HPI_M3_FONT_LABEL, 0);
	lv_obj_set_style_text_color(s_rec.err_overlay, HPI_M3_ERROR, 0);
	lv_obj_set_style_bg_color(s_rec.err_overlay, HPI_M3_SURFACE_HIGH, 0);
	lv_obj_set_style_bg_opa(s_rec.err_overlay, LV_OPA_COVER, 0);
	lv_obj_set_style_border_color(s_rec.err_overlay, HPI_M3_ERROR, 0);
	lv_obj_set_style_border_width(s_rec.err_overlay, 2, 0);
	lv_obj_set_style_radius(s_rec.err_overlay, HPI_M3_RADIUS_MD, 0);
	lv_obj_set_style_pad_all(s_rec.err_overlay, HPI_M3_SPACE_3, 0);
	lv_obj_set_style_margin_ver(s_rec.err_overlay, HPI_M3_SPACE_2, 0);
	hpi_m3_apply_touch(s_rec.err_overlay);
	lv_obj_add_flag(s_rec.err_overlay, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_flag(s_rec.err_overlay, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(s_rec.err_overlay, err_overlay_click_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *body = lv_obj_create(root);
	lv_obj_set_width(body, lv_pct(100));
	lv_obj_set_flex_grow(body, 1);
	lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(body, 0, 0);
	lv_obj_set_style_pad_all(body, 0, 0);
	lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

	build_setup(body);
	build_active(body);

	/* Refreshed by the UI loop while this screen is up -- no timer of its
	 * own, so nothing here runs on other screens. */
	s_rec.stop_armed = false;
	s_rec.stop_timer = NULL;

	/* Battery overlay (hidden by default) */
	s_rec.batt_overlay = lv_label_create(root);
	lv_label_set_text(s_rec.batt_overlay, "");
	lv_obj_set_style_text_color(s_rec.batt_overlay, HPI_M3_ERROR, 0);
	lv_obj_add_flag(s_rec.batt_overlay, LV_OBJ_FLAG_HIDDEN);


	hpi_scr_record_refresh();
	return root;
}

static uint32_t sd_free_pct;   /* used % for the bar (0..100) */

static void sd_update(void)
{
	struct fs_statvfs st;

	if (s_rec.sd_free == NULL) {
		return;
	}

	/* Never call fs_statvfs() on an unmounted path: Zephyr's FS layer logs
	 * `mount point not found!!` at ERROR level on every call, and this runs
	 * twice a second while the Record tab is up. */
	if (!platform_fs_is_ready()) {
		lv_label_set_text(s_rec.sd_free, "no card");
		sd_free_pct = 0;
		if (s_rec.sd_bar) {
			lv_obj_set_width(s_rec.sd_bar, lv_pct(0));
		}
		return;
	}

	if (fs_statvfs("/SD:", &st) == 0 && st.f_blocks > 0) {
		uint64_t total = (uint64_t)st.f_blocks * st.f_frsize;
		uint64_t freeb = (uint64_t)st.f_bfree * st.f_frsize;
		char buf[40];

		snprintf(buf, sizeof(buf), "%u.%u / %u.%u GB free",
			 (unsigned)(freeb / 1000000000ULL),
			 (unsigned)((freeb / 100000000ULL) % 10U),
			 (unsigned)(total / 1000000000ULL),
			 (unsigned)((total / 100000000ULL) % 10U));
		lv_label_set_text(s_rec.sd_free, buf);
		sd_free_pct = total ? (uint32_t)(((total - freeb) * 100U) / total) : 0;
	} else {
		lv_label_set_text(s_rec.sd_free, "--");
		sd_free_pct = 0;
	}
	if (s_rec.sd_bar) {
		lv_obj_set_width(s_rec.sd_bar, lv_pct(sd_free_pct));
	}
}

void hpi_scr_record_refresh(void)
{
	if (s_rec.setup == NULL) {
		return;
	}

	struct hpi_recording_status st;
	hpi_recording_get_status(&st);

	lv_obj_set_flag(s_rec.setup,  LV_OBJ_FLAG_HIDDEN, st.active);
	lv_obj_set_flag(s_rec.active, LV_OBJ_FLAG_HIDDEN, !st.active);
	lv_obj_set_flag(s_rec.recdot, LV_OBJ_FLAG_HIDDEN, !st.active);
	hpi_ui_statusbar_set_title(s_rec.bar,
				   st.active ? "Recording" : "New Recording");

	if (st.error) {
		char ebuf[96];
		const char *why = rec_error_text(st.error_code);

		if (why != NULL) {
			snprintf(ebuf, sizeof(ebuf), "%s\nTap to dismiss", why);
		} else {
			snprintf(ebuf, sizeof(ebuf),
				 "Recording stopped: error %d\nTap to dismiss",
				 st.error_code);
		}
		lv_label_set_text(s_rec.err_overlay, ebuf);
		lv_obj_clear_flag(s_rec.err_overlay, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_add_flag(s_rec.err_overlay, LV_OBJ_FLAG_HIDDEN);
	}
	if (st.active) {
		uint32_t s = st.duration_ms / 1000U;
		char buf[64];

		snprintf(buf, sizeof(buf), "%02u:%02u:%02u",
			 s / 3600U, (s / 60U) % 60U, s % 60U);
		lv_label_set_text(s_rec.elapsed, buf);
		snprintf(buf, sizeof(buf), "%u.%u MB  \xC2\xB7  %u events%s",
			 st.bytes_written / 1000000U,
			 (st.bytes_written / 100000U) % 10U, st.events,
			 st.paused ? "  \xC2\xB7  PAUSED" : "");
		lv_label_set_text(s_rec.meta, buf);
		lv_label_set_text(s_rec.fname, st.path);
		lv_obj_t *plbl = hpi_ui_button_label(s_rec.pause_btn);
		if (plbl) {
			lv_label_set_text(plbl, st.paused ? "RESUME" : "PAUSE");
		}

		/* Low-battery overlay: show a warning but do not stop recording */
		struct hpi_power_status ps = {0};
		hpi_power_get(&ps);
		if (ps.valid && ps.soc_pct < 10) {
			lv_label_set_text(s_rec.batt_overlay, "Low battery — recording continues");
			lv_obj_clear_flag(s_rec.batt_overlay, LV_OBJ_FLAG_HIDDEN);
		} else {
			lv_obj_add_flag(s_rec.batt_overlay, LV_OBJ_FLAG_HIDDEN);
		}
	} else {
		sd_update();
	}
}
