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

#include <stdio.h>
#include <errno.h>
#include <zephyr/fs/fs.h>

#include "services/recording_service.h"
#include "platform/fs_mount.h"
#include "bus/hpi_events.h"

/* ============================ Record screen ============================
 *
 * One screen, two views toggled by the recording state: setup (channels,
 * duration, SD estimate, START CTA) and active (elapsed, size/events,
 * MARK EVENT, STOP). The recording_service records a fixed channel set with no
 * duration limit, so the channel/duration controls are visual design chrome;
 * START/STOP/elapsed/size + the SD free readout are live. MARK EVENT publishes
 * HPI_EVT_USER_MARK (the hardware short-press mirrors it). */

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
	uint32_t    events;      /* marks this session */
} s_rec;

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

/* Visual M3 switch (indicator only — the service records a fixed channel set). */
static void channel_row(lv_obj_t *parent, const char *icon, lv_color_t icol,
			const char *name, const char *rate, bool on)
{
	lv_obj_t *row = lv_obj_create(parent);
	lv_obj_set_width(row, lv_pct(100));
	lv_obj_set_height(row, LV_SIZE_CONTENT);
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
	lv_obj_set_style_text_color(ic, on ? icol : HPI_M3_ON_SURFACE_FAINT, 0);

	lv_obj_t *nm = lv_label_create(row);
	lv_label_set_text(nm, name);
	lv_obj_set_style_text_font(nm, HPI_M3_FONT_BODY, 0);
	lv_obj_set_style_text_color(nm, on ? HPI_M3_ON_SURFACE
					   : HPI_M3_ON_SURFACE_VARIANT, 0);

	lv_obj_t *rt = lv_label_create(row);
	lv_label_set_text(rt, rate);
	lv_obj_set_style_text_font(rt, HPI_M3_FONT_MONO, 0);
	lv_obj_set_style_text_color(rt, HPI_M3_ON_SURFACE_MUTED, 0);
	lv_obj_set_flex_grow(rt, 1);
	lv_obj_set_style_text_align(rt, LV_TEXT_ALIGN_RIGHT, 0);

	lv_obj_t *sw = lv_obj_create(row);      /* switch track */
	lv_obj_set_size(sw, 44, 24);
	lv_obj_set_style_radius(sw, 12, 0);
	lv_obj_set_style_bg_color(sw, on ? HPI_M3_PRIMARY : HPI_M3_OUTLINE, 0);
	lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(sw, 0, 0);
	lv_obj_clear_flag(sw, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_t *knob = lv_obj_create(sw);     /* knob */
	lv_obj_set_size(knob, 20, 20);
	lv_obj_align(knob, on ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, on ? -2 : 2, 0);
	lv_obj_set_style_radius(knob, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(knob, on ? HPI_M3_SURFACE : HPI_M3_ON_SURFACE_MUTED, 0);
	lv_obj_set_style_bg_opa(knob, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(knob, 0, 0);
	lv_obj_clear_flag(knob, LV_OBJ_FLAG_SCROLLABLE);
}

/* Filled M3 CTA/button with an icon + caps label. */
static lv_obj_t *rec_button(lv_obj_t *parent, const char *icon, const char *text,
			    lv_color_t bg, lv_color_t fg, lv_event_cb_t cb)
{
	lv_obj_t *b = lv_button_create(parent);
	lv_obj_set_size(b, lv_pct(100), HPI_M3_TOUCH_MIN);   /* START/STOP/MARK */
	hpi_m3_apply_card(b, bg, HPI_M3_RADIUS_XL);
	lv_obj_set_style_border_width(b, 0, 0);
	lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(b, HPI_M3_SPACE_2, 0);
	lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *ic = lv_label_create(b);
	lv_label_set_text(ic, icon);
	lv_obj_set_style_text_font(ic, HPI_M3_FONT_ICON, 0);
	lv_obj_set_style_text_color(ic, fg, 0);
	lv_obj_t *l = lv_label_create(b);
	lv_label_set_text(l, text);
	lv_obj_set_style_text_font(l, HPI_M3_FONT_CAPS, 0);
	lv_obj_set_style_text_color(l, fg, 0);
	return b;
}

static void seg_item(lv_obj_t *bar, const char *text, bool on, bool last)
{
	lv_obj_t *s = lv_obj_create(bar);
	lv_obj_set_height(s, lv_pct(100));
	lv_obj_set_flex_grow(s, 1);
	lv_obj_set_style_bg_color(s, HPI_M3_PRIMARY_CONTAINER, 0);
	lv_obj_set_style_bg_opa(s, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
	lv_obj_set_style_radius(s, 0, 0);
	lv_obj_set_style_border_width(s, last ? 0 : 1, 0);
	lv_obj_set_style_border_side(s, LV_BORDER_SIDE_RIGHT, 0);
	lv_obj_set_style_border_color(s, HPI_M3_OUTLINE, 0);
	lv_obj_set_style_pad_all(s, 0, 0);
	lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_t *l = lv_label_create(s);
	lv_label_set_text(l, text);
	lv_obj_center(l);
	lv_obj_set_style_text_font(l, HPI_M3_FONT_CAPS_SM, 0);
	lv_obj_set_style_text_color(l, on ? HPI_M3_PRIMARY_LIGHT
					  : HPI_M3_ON_SURFACE_VARIANT, 0);
}

/* --- callbacks --- */

static void rec_start_cb(lv_event_t *e)
{
	ARG_UNUSED(e);
	int ret = hpi_recording_start(NULL);
	const char *msg = NULL;

	if (ret == -ENODEV) {
		msg = "SD card not ready";
	} else if (ret == -EBUSY) {
		msg = "Busy (Transfer Mode armed?)";
	} else if (ret < 0) {
		msg = "Start failed";
	} else {
		s_rec.events = 0;
	}
	if (s_rec.err) {
		lv_label_set_text(s_rec.err, msg ? msg : "");
		lv_obj_set_flag(s_rec.err, LV_OBJ_FLAG_HIDDEN, msg == NULL);
	}
	hpi_scr_record_refresh();
}

static void rec_stop_cb(lv_event_t *e)
{
	ARG_UNUSED(e);
	hpi_recording_stop();
	hpi_scr_record_refresh();
}

/* The service owns the mark and its sequence -- it publishes an hp6_event on
 * HPI_CH_EVENT, which the recording writes in-band and the stream and ESP32
 * link also see -- so the count shown is what was actually stored, never a
 * local tally. */
static void rec_mark_cb(lv_event_t *e)
{
	ARG_UNUSED(e);
	int seq = hpi_recording_mark();

	if (seq < 0) {
		return;   /* not recording: nothing to mark */
	}
	s_rec.events = (uint32_t)seq;
	hpi_events_publish(HPI_EVT_USER_MARK, seq);   /* in-process notification */
	hpi_scr_record_refresh();
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
	lv_obj_clear_flag(s_rec.setup, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(s_rec.setup, LV_FLEX_FLOW_COLUMN);

	/* Channels. */
	lv_obj_t *ch = rec_card(s_rec.setup);
	section_label(ch, "CHANNELS");
	channel_row(ch, HPI_SYM_LIVE, HPI_M3_SIG_ECG,  "ECG",              "500 Hz", true);
	channel_row(ch, HPI_SYM_SPO2, HPI_M3_SIG_SPO2, "PPG Red + IR",     "125 Hz", true);
	channel_row(ch, HPI_SYM_RESP, HPI_M3_SIG_RESP, "Respiration (BioZ)", "125 Hz", true);
	channel_row(ch, HPI_SYM_TEMP, HPI_M3_SIG_TEMP, "Temperature",      "1 Hz",   false);

	/* Duration (visual). */
	lv_obj_t *du = rec_card(s_rec.setup);
	section_label(du, "DURATION");
	lv_obj_t *seg = lv_obj_create(du);
	lv_obj_set_size(seg, lv_pct(100), 44);
	lv_obj_set_style_bg_opa(seg, LV_OPA_TRANSP, 0);
	lv_obj_set_style_radius(seg, HPI_M3_RADIUS_PILL, 0);
	lv_obj_set_style_border_width(seg, 1, 0);
	lv_obj_set_style_border_color(seg, HPI_M3_OUTLINE, 0);
	lv_obj_set_style_pad_all(seg, 0, 0);
	lv_obj_set_style_clip_corner(seg, true, 0);
	lv_obj_clear_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(seg, LV_FLEX_FLOW_ROW);
	seg_item(seg, "30 s",  false, false);
	seg_item(seg, "5 min", true,  false);
	seg_item(seg, "1 h",   false, false);
	seg_item(seg, "CONT",  false, true);

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

	/* START CTA + error line, pinned to the bottom. */
	lv_obj_t *cta = rec_button(s_rec.setup, HPI_SYM_REC, "START RECORDING",
				   HPI_M3_CTA, HPI_M3_ON_CTA, rec_start_cb);
	lv_obj_set_style_margin_top(cta, HPI_M3_SPACE_2, 0);
	s_rec.err = lv_label_create(s_rec.setup);
	lv_label_set_text(s_rec.err, "");
	lv_obj_set_style_text_font(s_rec.err, HPI_M3_FONT_LABEL, 0);
	lv_obj_set_style_text_color(s_rec.err, HPI_M3_ERROR, 0);
	lv_obj_add_flag(s_rec.err, LV_OBJ_FLAG_HIDDEN);
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
	lv_obj_set_style_text_font(s_rec.elapsed, HPI_M3_FONT_NUMERAL_XL, 0);
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
	rec_button(btns, HPI_SYM_HRV, "MARK EVENT", HPI_M3_SURFACE_CONTAINER,
		   HPI_M3_SIG_RESP, rec_mark_cb);
	rec_button(btns, HPI_SYM_REC, "STOP & SAVE", HPI_M3_ERROR,
		   HPI_M3_ON_SURFACE, rec_stop_cb);

	lv_obj_t *hint = lv_label_create(s_rec.active);
	lv_label_set_text(hint, "Hardware button: short press = mark event");
	lv_obj_set_style_text_font(hint, HPI_M3_FONT_CAPS_SM, 0);
	lv_obj_set_style_text_color(hint, HPI_M3_ON_SURFACE_FAINT, 0);
	lv_obj_set_style_margin_top(hint, HPI_M3_SPACE_3, 0);
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

	lv_obj_t *body = lv_obj_create(root);
	lv_obj_set_width(body, lv_pct(100));
	lv_obj_set_flex_grow(body, 1);
	lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(body, 0, 0);
	lv_obj_set_style_pad_all(body, 0, 0);
	lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

	build_setup(body);
	build_active(body);

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

	if (st.active) {
		uint32_t s = st.duration_ms / 1000U;
		char buf[64];

		snprintf(buf, sizeof(buf), "%02u:%02u:%02u",
			 s / 3600U, (s / 60U) % 60U, s % 60U);
		lv_label_set_text(s_rec.elapsed, buf);
		snprintf(buf, sizeof(buf), "%u.%u MB  \xC2\xB7  %u events",
			 st.bytes_written / 1000000U,
			 (st.bytes_written / 100000U) % 10U, s_rec.events);
		lv_label_set_text(s_rec.meta, buf);
		lv_label_set_text(s_rec.fname, st.path);
	} else {
		sd_update();
	}
}
