/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Recordings browse/delete screen — a recording_service client.
 *
 * recording_list_async() runs the directory walk on the system workqueue
 * thread, so its callback (reclist_cb) must never touch LVGL. It copies each
 * recording_summary into a staging array and, on the NULL-entry sentinel,
 * flips an atomic "ready" flag. hpi_scr_recording_refresh() is polled from
 * the UI thread and is the only place that ever rebuilds the LVGL list --
 * the same deferred-update split ui_module.c uses for the sample bus.
 */
#include "scr_recording.h"
#include "../components/hpi_ui_components.h"
#include "../theme/hpi_m3_theme.h"

#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/logging/log.h>

#include "services/recording_service.h"
#include "core/channel_registry.h"
#include "platform/fs_mount.h"
#include "../ui_module.h"  /* enum hpi_ui_screen, HPI_UI_SCREEN_REC */
#include <errno.h>

LOG_MODULE_DECLARE(hpi_ui, CONFIG_HPI_APP_LOG_LEVEL);

#define REC_PAGE_SIZE  15
BUILD_ASSERT(REC_PAGE_SIZE <= RECORDING_PAGE_SIZE_MAX,
	     "UI page size exceeds the service's page limit");

static atomic_t   s_index_ready = ATOMIC_INIT(0);
static uint32_t   s_total_count;

static struct recording_summary s_page_stage[REC_PAGE_SIZE];
static uint32_t   s_page_stage_n;
static atomic_t   s_page_ready = ATOMIC_INIT(0);
static bool       s_page_pending_clear;

static bool       s_awaiting_index;
static bool       s_awaiting_page;
static bool       s_loading;
static uint32_t   s_cur_page;

static bool       s_detail_pending;
static uint32_t   s_detail_pending_local;

/* ---- UI state ---- */
static struct {
	lv_obj_t *root;
	lv_obj_t *bar;
	lv_obj_t *list;        /* scrollable column of row cards            */
	lv_obj_t *pager_bar;
	lv_obj_t *pager_label;
	lv_obj_t *pager_prev;
	lv_obj_t *pager_next;
	lv_obj_t *empty_label; /* "No recordings yet" placeholder            */
	lv_obj_t *detail;      /* modal overlay, or NULL when closed         */
	struct recording_summary sel;   /* copy of the row that's open in detail */
	uint32_t  sel_idx;     /* index of `sel` into s_stage[], for paging */
	bool      del_armed;
	lv_timer_t *del_timer;
} s_rb;

static void index_cb(const struct recording_index_entry *entries, size_t n_entries,
		     void *user_data)
{
	ARG_UNUSED(entries);
	ARG_UNUSED(user_data);
	s_total_count = (uint32_t)n_entries;
	atomic_set(&s_index_ready, 1);
}

static void page_cb(const struct recording_summary *entry, void *user_data)
{
	ARG_UNUSED(user_data);
	if (s_page_pending_clear) {
		s_page_stage_n = 0;
		s_page_pending_clear = false;
	}
	if (entry == NULL) {
		atomic_set(&s_page_ready, 1);
		return;
	}
	if (s_page_stage_n < REC_PAGE_SIZE) {
		s_page_stage[s_page_stage_n++] = *entry;
	}
}

static void update_pager_ui(void);
static void pager_prev_cb(lv_event_t *e);
static void pager_next_cb(lv_event_t *e);
static void rebuild_list(void);

static void request_page(uint32_t page)
{
	uint32_t total_pages = (s_total_count + REC_PAGE_SIZE - 1) / REC_PAGE_SIZE;
	if (total_pages == 0) total_pages = 1;
	if (page >= total_pages) page = total_pages - 1;

	s_cur_page = page;
	s_page_pending_clear = true;
	atomic_set(&s_page_ready, 0);
	s_awaiting_page = true;
	s_loading = true;

	if (recording_page_fetch_async(s_cur_page * REC_PAGE_SIZE, REC_PAGE_SIZE,
					page_cb, NULL) != 0) {
		LOG_WRN("recording_page_fetch_async failed to start");
		s_page_stage_n = 0;
		s_awaiting_page = false;
		s_loading = false;
	}
	update_pager_ui();
}

void hpi_scr_recording_reload(void)
{
	s_awaiting_page = false;
	atomic_set(&s_page_ready, 0);
	s_awaiting_index = true;
	atomic_set(&s_index_ready, 0);
	s_loading = true;

	int idx_rc = recording_index_build_async(index_cb, NULL);
	if (idx_rc != 0) {
		LOG_WRN("recording_index_build_async failed to start (%d)", idx_rc);
		s_total_count = 0;
		s_page_stage_n = 0;
		s_awaiting_index = false;
		s_loading = false;
	}
	if (s_rb.list) {
		rebuild_list();
		update_pager_ui();
	}
}

/* ---- formatting helpers ---- */

static void fmt_size(uint32_t bytes, char *buf, size_t n)
{
	if (bytes >= 1000U * 1000U) {
		snprintf(buf, n, "%u.%u MB", bytes / 1000000U, (bytes / 100000U) % 10U);
	} else if (bytes >= 1000U) {
		snprintf(buf, n, "%u KB", bytes / 1000U);
	} else {
		snprintf(buf, n, "%u B", bytes);
	}
}

static void fmt_duration(uint32_t ms, char *buf, size_t n)
{
	uint32_t s = ms / 1000U;
	if (ms == 0) {
		snprintf(buf, n, "--:--:--");
		return;
	}
	snprintf(buf, n, "%02u:%02u:%02u", s / 3600U, (s / 60U) % 60U, s % 60U);
}

static void fmt_date(const struct recording_summary *e, char *buf, size_t n)
{
	if (e->year == 0) {
		snprintf(buf, n, "Undated");
	} else {
		snprintf(buf, n, "%04u-%02u-%02u", e->year, e->month, e->day);
	}
}

static void fmt_time(const struct recording_summary *e, char *buf, size_t n)
{
	if (e->year == 0) {
		buf[0] = '\0';
	} else {
		snprintf(buf, n, "%02u:%02u:%02u", e->hour, e->min, e->sec);
	}
}

/* ---- channel icon row (shared by list rows and the detail overlay) ---- */

static void add_channel_glyph(lv_obj_t *row, uint32_t channels, uint8_t ch,
			      const char *sym, lv_color_t col)
{
	if (!(channels & HPI_CH_BIT(ch))) {
		return;
	}
	lv_obj_t *l = lv_label_create(row);
	lv_label_set_text(l, sym);
	lv_obj_set_style_text_font(l, HPI_M3_FONT_ICON, 0);
	lv_obj_set_style_text_color(l, col, 0);
}

/* EEG has no dedicated glyph elsewhere in the UI yet -- a small text chip is
 * an honest fallback rather than borrowing an unrelated icon. */
static void add_eeg_chip(lv_obj_t *row, uint32_t channels)
{
	if (!(channels & HPI_CH_BIT(HPI_CH_EEG))) {
		return;
	}
	lv_obj_t *l = lv_label_create(row);
	lv_label_set_text(l, "EEG");
	lv_obj_set_style_text_font(l, HPI_M3_FONT_CAPS_SM, 0);
	lv_obj_set_style_text_color(l, HPI_M3_ON_SURFACE_VARIANT, 0);
}

static void channel_icons_row(lv_obj_t *parent, uint32_t channels)
{
	lv_obj_t *row = lv_obj_create(parent);
	lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_pad_all(row, 0, 0);
	lv_obj_set_style_pad_column(row, HPI_M3_SPACE_1, 0);
	lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);

	add_channel_glyph(row, channels, HPI_CH_ECG,  HPI_SYM_LIVE, HPI_M3_SIG_ECG);
	add_channel_glyph(row, channels, HPI_CH_PPG,  HPI_SYM_SPO2, HPI_M3_SIG_SPO2);
	add_channel_glyph(row, channels, HPI_CH_RESP, HPI_SYM_RESP, HPI_M3_SIG_RESP);
	add_channel_glyph(row, channels, HPI_CH_VITALS, HPI_SYM_HRV, HPI_M3_SIG_HRV);
	add_eeg_chip(row, channels);

	if (channels == 0) {
		lv_obj_t *l = lv_label_create(row);
		lv_label_set_text(l, "--");
		lv_obj_set_style_text_font(l, HPI_M3_FONT_CAPS_SM, 0);
		lv_obj_set_style_text_color(l, HPI_M3_ON_SURFACE_FAINT, 0);
	}
}

/* ---- detail overlay (delete with two-tap confirm + swipe paging) ---- */

static void open_detail(const struct recording_summary *e, uint32_t idx);

static void detail_close(void)
{
	if (s_rb.del_timer) {
		lv_timer_del(s_rb.del_timer);
		s_rb.del_timer = NULL;
	}
	if (s_rb.detail) {
		lv_obj_del(s_rb.detail);
		s_rb.detail = NULL;
	}
	s_rb.del_armed = false;
}

static void detail_close_cb(lv_event_t *e)
{
	ARG_UNUSED(e);
	detail_close();
}

static void detail_goto_global(uint32_t global_idx)
{
	if (s_total_count == 0) return;
	global_idx %= s_total_count;

	uint32_t page  = global_idx / REC_PAGE_SIZE;
	uint32_t local = global_idx % REC_PAGE_SIZE;

	if (page == s_cur_page && local < s_page_stage_n) {
		open_detail(&s_page_stage[local], global_idx);
		return;
	}

	s_detail_pending = true;
	s_detail_pending_local = local;
	request_page(page);
}

static void detail_gesture_cb(lv_event_t *e)
{
	ARG_UNUSED(e);
	lv_indev_t *indev = lv_indev_active();
	if (!indev) return;
	switch (lv_indev_get_gesture_dir(indev)) {
	case LV_DIR_LEFT:  detail_goto_global(s_rb.sel_idx + 1); break;
	case LV_DIR_RIGHT: detail_goto_global(s_rb.sel_idx + s_total_count - 1); break;
	default: return;
	}
}

static void page_prev_cb(lv_event_t *e)
{
	ARG_UNUSED(e);
	if (s_total_count) detail_goto_global(s_rb.sel_idx + s_total_count - 1);
}

static void page_next_cb(lv_event_t *e)
{
	ARG_UNUSED(e);
	if (s_total_count) detail_goto_global(s_rb.sel_idx + 1);
}

static void del_confirm_timeout(lv_timer_t *t)
{
	lv_obj_t *btn = (lv_obj_t *)lv_timer_get_user_data(t);
	s_rb.del_armed = false;
	if (btn) {
		lv_label_set_text(lv_obj_get_child(btn, 1), "DELETE");
	}
	s_rb.del_timer = NULL;
	lv_timer_del(t);
}

static void del_btn_cb(lv_event_t *e)
{
	lv_obj_t *btn = lv_event_get_target(e);

	if (!s_rb.del_armed) {
		/* First tap: just arm it. Nothing is deleted here. */
		s_rb.del_armed = true;
		lv_label_set_text(lv_obj_get_child(btn, 1), "CONFIRM DELETE");
		s_rb.del_timer = lv_timer_create(del_confirm_timeout, 2000, btn);
		return;
	}

	/* Second tap within the window: confirmed. */
	if (s_rb.del_timer) {
		lv_timer_del(s_rb.del_timer);
		s_rb.del_timer = NULL;
	}
	int rc = recording_delete(s_rb.sel.path);
	if (rc == 0) {
		detail_close();
		hpi_scr_recording_reload();   /* pull the deleted row out of the list */
	} else {
		lv_label_set_text(lv_obj_get_child(btn, 1),
				  rc == -EBUSY ? "IN USE - CAN'T DELETE" : "DELETE FAILED");
		s_rb.del_armed = false;
	}
}

static lv_obj_t *detail_row(lv_obj_t *parent, const char *label, const char *value)
{
	lv_obj_t *row = lv_obj_create(parent);
	lv_obj_set_width(row, lv_pct(100));
	lv_obj_set_height(row, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_pad_all(row, 0, 0);
	lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);

	lv_obj_t *l = lv_label_create(row);
	lv_label_set_text(l, label);
	lv_obj_set_style_text_font(l, HPI_M3_FONT_LABEL, 0);
	lv_obj_set_style_text_color(l, HPI_M3_ON_SURFACE_VARIANT, 0);

	lv_obj_t *v = lv_label_create(row);
	lv_label_set_text(v, value);
	lv_obj_set_style_text_font(v, HPI_M3_FONT_MONO, 0);
	lv_obj_set_style_text_color(v, HPI_M3_ON_SURFACE, 0);
	return row;
}

static void open_detail(const struct recording_summary *e, uint32_t idx)
{
	detail_close();   /* no-op if nothing was open; also kills the old timer */
	s_rb.sel = *e;
	s_rb.sel_idx = idx;

	lv_obj_t *ov = lv_obj_create(s_rb.root);
	lv_obj_add_flag(ov, LV_OBJ_FLAG_IGNORE_LAYOUT);   /* NEW */
	lv_obj_set_pos(ov, 0, 0);
	lv_obj_set_size(ov, lv_pct(100), lv_pct(100));
	hpi_m3_apply_card(ov, HPI_M3_SURFACE, 0);
	lv_obj_set_style_pad_all(ov, HPI_M3_SPACE_4, 0);
	lv_obj_set_style_pad_row(ov, HPI_M3_SPACE_2, 0);	
	lv_obj_add_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_scroll_dir(ov, LV_DIR_VER);
	lv_obj_set_flex_flow(ov, LV_FLEX_FLOW_COLUMN);
	lv_obj_add_event_cb(ov, detail_gesture_cb, LV_EVENT_GESTURE, NULL);
	s_rb.detail = ov;

	char dbuf[16], tbuf[16], sizebuf[24], durbuf[16];
	fmt_date(e, dbuf, sizeof(dbuf));
	fmt_time(e, tbuf, sizeof(tbuf));
	fmt_size(e->size_bytes, sizebuf, sizeof(sizebuf));
	fmt_duration(e->duration_ms, durbuf, sizeof(durbuf));

	/* Title row: prev chevron -- title + "N / total" -- next chevron. */
	lv_obj_t *titlerow = lv_obj_create(ov);
	lv_obj_set_width(titlerow, lv_pct(100));
	lv_obj_set_height(titlerow, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(titlerow, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(titlerow, 0, 0);
	lv_obj_set_style_pad_all(titlerow, 0, 0);
	lv_obj_clear_flag(titlerow, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(titlerow, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(titlerow, LV_FLEX_ALIGN_SPACE_BETWEEN,
			      LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	lv_obj_t *prev_btn = lv_button_create(titlerow);
	lv_obj_set_size(prev_btn, HPI_M3_TOUCH_MIN, HPI_M3_TOUCH_MIN);
	lv_obj_set_style_bg_opa(prev_btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(prev_btn, 0, 0);
	lv_obj_add_event_cb(prev_btn, page_prev_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *pl = lv_label_create(prev_btn);
	lv_label_set_text(pl, LV_SYMBOL_LEFT);
	lv_obj_set_style_text_font(pl, &lv_font_montserrat_14, 0);
	lv_obj_center(pl);
	lv_obj_set_style_text_color(pl, HPI_M3_ON_SURFACE_VARIANT, 0);

	lv_obj_t *titlebox = lv_obj_create(titlerow);
	lv_obj_set_size(titlebox, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(titlebox, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(titlebox, 0, 0);
	lv_obj_set_style_pad_all(titlebox, 0, 0);
	lv_obj_clear_flag(titlebox, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(titlebox, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(titlebox, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);

	lv_obj_t *title = lv_label_create(titlebox);
	lv_label_set_text(title, tbuf[0] ? tbuf : dbuf);
	lv_obj_set_style_text_font(title, HPI_M3_FONT_HEADLINE, 0);
	lv_obj_set_style_text_color(title, HPI_M3_ON_SURFACE, 0);

	char pagebuf[16];
	snprintf(pagebuf, sizeof(pagebuf), "%u / %u", idx + 1, s_total_count);
	lv_obj_t *pagelbl = lv_label_create(titlebox);
	lv_label_set_text(pagelbl, pagebuf);
	lv_obj_set_style_text_font(pagelbl, HPI_M3_FONT_CAPS_SM, 0);
	lv_obj_set_style_text_color(pagelbl, HPI_M3_ON_SURFACE_FAINT, 0);

	lv_obj_t *next_btn = lv_button_create(titlerow);
	lv_obj_set_size(next_btn, HPI_M3_TOUCH_MIN, HPI_M3_TOUCH_MIN);
	lv_obj_set_style_bg_opa(next_btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(next_btn, 0, 0);
	lv_obj_add_event_cb(next_btn, page_next_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *nl = lv_label_create(next_btn);
	lv_label_set_text(nl, LV_SYMBOL_RIGHT);
	lv_obj_set_style_text_font(nl, &lv_font_montserrat_14, 0);
	lv_obj_center(nl);
	lv_obj_set_style_text_color(nl, HPI_M3_ON_SURFACE_VARIANT, 0);

	detail_row(ov, "DATE", dbuf);
	detail_row(ov, "DURATION", durbuf);
	detail_row(ov, "SIZE", sizebuf);
	char evbuf[12];
	snprintf(evbuf, sizeof(evbuf), "%u", e->event_count);
	detail_row(ov, "EVENTS", evbuf);

	lv_obj_t *chrow = lv_obj_create(ov);
	lv_obj_set_width(chrow, lv_pct(100));
	lv_obj_set_height(chrow, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(chrow, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(chrow, 0, 0);
	lv_obj_set_style_pad_all(chrow, 0, 0);
	lv_obj_clear_flag(chrow, LV_OBJ_FLAG_SCROLLABLE);
	channel_icons_row(chrow, e->channels);

	lv_obj_t *fname = lv_label_create(ov);
	lv_label_set_text(fname, e->path);
	lv_obj_set_style_text_font(fname, HPI_M3_FONT_MONO, 0);
	lv_obj_set_style_text_color(fname, HPI_M3_ON_SURFACE_MUTED, 0);
	lv_obj_set_width(fname, lv_pct(100));
	lv_label_set_long_mode(fname, LV_LABEL_LONG_DOT);
	lv_obj_set_style_margin_top(fname, HPI_M3_SPACE_2, 0);

	/* spacer pushes the buttons to the bottom */
	lv_obj_t *spacer = lv_obj_create(ov);
	lv_obj_set_width(spacer, lv_pct(100));
	lv_obj_set_flex_grow(spacer, 1);
	lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(spacer, 0, 0);
	lv_obj_clear_flag(spacer, LV_OBJ_FLAG_SCROLLABLE);


	lv_obj_t *del_btn = lv_button_create(ov);
	lv_obj_set_size(del_btn, lv_pct(100), HPI_M3_TOUCH_MIN);
	hpi_m3_apply_card(del_btn, HPI_M3_ERROR, HPI_M3_RADIUS_XL);
	lv_obj_set_style_border_width(del_btn, 0, 0);
	lv_obj_set_flex_flow(del_btn, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(del_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(del_btn, HPI_M3_SPACE_2, 0);
	lv_obj_add_event_cb(del_btn, del_btn_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *dic = lv_label_create(del_btn);
	lv_label_set_text(dic, LV_SYMBOL_TRASH);
	lv_obj_set_style_text_font(dic, &lv_font_montserrat_14, 0);
	lv_obj_set_style_text_color(dic, HPI_M3_ON_SURFACE, 0);
	lv_obj_t *dl = lv_label_create(del_btn);
	lv_label_set_text(dl, "DELETE");
	lv_obj_set_style_text_font(dl, HPI_M3_FONT_CAPS, 0);
	lv_obj_set_style_text_color(dl, HPI_M3_ON_SURFACE, 0);

	lv_obj_t *close_btn = lv_button_create(ov);
	lv_obj_set_size(close_btn, lv_pct(100), HPI_M3_TOUCH_MIN);
	hpi_m3_apply_card(close_btn, HPI_M3_SURFACE_CONTAINER, HPI_M3_RADIUS_XL);
	lv_obj_set_style_border_width(close_btn, 0, 0);
	lv_obj_set_flex_flow(close_btn, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(close_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	lv_obj_add_event_cb(close_btn, detail_close_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *cl = lv_label_create(close_btn);
	lv_label_set_text(cl, "CLOSE");
	lv_obj_set_style_text_font(cl, HPI_M3_FONT_CAPS, 0);
	lv_obj_set_style_text_color(cl, HPI_M3_ON_SURFACE, 0);
}

/* ---- row tap ---- */

static void row_click_cb(lv_event_t *e)
{
	uint32_t local = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
	if (local < s_page_stage_n) {
		open_detail(&s_page_stage[local], s_cur_page * REC_PAGE_SIZE + local);
	}
}

static void build_row(lv_obj_t *parent, const struct recording_summary *e, uint32_t idx)
{
	lv_obj_t *row = lv_obj_create(parent);
	lv_obj_set_width(row, lv_pct(100));
	lv_obj_set_height(row, LV_SIZE_CONTENT);
	hpi_m3_apply_card(row, HPI_M3_SURFACE_CONTAINER, HPI_M3_RADIUS_MD);
	lv_obj_set_style_pad_all(row, HPI_M3_SPACE_3, 0);
	lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(row, row_click_cb, LV_EVENT_CLICKED,
			    (void *)(uintptr_t)idx);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(row, HPI_M3_SPACE_1, 0);

	lv_obj_t *top = lv_obj_create(row);
	lv_obj_set_width(top, lv_pct(100));
	lv_obj_set_height(top, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(top, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(top, 0, 0);
	lv_obj_set_style_pad_all(top, 0, 0);
	lv_obj_clear_flag(top, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_clear_flag(top, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(top, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);

	char dbuf[16], tbuf[16], durbuf[16], sizebuf[24];
	fmt_date(e, dbuf, sizeof(dbuf));
	fmt_time(e, tbuf, sizeof(tbuf));
	fmt_duration(e->duration_ms, durbuf, sizeof(durbuf));
	fmt_size(e->size_bytes, sizebuf, sizeof(sizebuf));

	char dt[40];
	if (tbuf[0]) {
		snprintf(dt, sizeof(dt), "%s  %s", dbuf, tbuf);
	} else {
		snprintf(dt, sizeof(dt), "%s", dbuf);
	}
	lv_obj_t *dtl = lv_label_create(top);
	lv_label_set_text(dtl, dt);
	lv_obj_set_style_text_font(dtl, HPI_M3_FONT_BODY, 0);
	lv_obj_set_style_text_color(dtl, HPI_M3_ON_SURFACE, 0);

	lv_obj_t *durl = lv_label_create(top);
	lv_label_set_text(durl, durbuf);
	lv_obj_set_style_text_font(durl, HPI_M3_FONT_MONO, 0);
	lv_obj_set_style_text_color(durl, HPI_M3_ON_SURFACE_VARIANT, 0);

	lv_obj_t *bottom = lv_obj_create(row);
	lv_obj_set_width(bottom, lv_pct(100));
	lv_obj_set_height(bottom, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(bottom, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(bottom, 0, 0);
	lv_obj_set_style_pad_all(bottom, 0, 0);
	lv_obj_clear_flag(bottom, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_clear_flag(bottom, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_flex_flow(bottom, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(bottom, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);

	channel_icons_row(bottom, e->channels);

	char meta[40];
	snprintf(meta, sizeof(meta), "%s  \xC2\xB7  %u events", sizebuf, e->event_count);
	lv_obj_t *ml = lv_label_create(bottom);
	lv_label_set_text(ml, meta);
	lv_obj_set_style_text_font(ml, HPI_M3_FONT_MONO, 0);
	lv_obj_set_style_text_color(ml, HPI_M3_ON_SURFACE_MUTED, 0);
}

static void rebuild_list(void)
{
	lv_obj_clean(s_rb.list);

	if (s_total_count == 0) {
		const char *msg;
		int irc = recording_index_last_error();
		if (s_loading) {
			msg = "Loading recordings...";
		} else if (!platform_fs_is_ready() ||
			   (irc != 0 && irc != -ENOENT)) {
			msg = "SD card not inserted";
		} else {
			msg = "No recordings yet";
		}
		lv_label_set_text(s_rb.empty_label, msg);
		lv_obj_clear_flag(s_rb.empty_label, LV_OBJ_FLAG_HIDDEN);
		return;
	}
	lv_obj_add_flag(s_rb.empty_label, LV_OBJ_FLAG_HIDDEN);

	for (uint32_t i = 0; i < s_page_stage_n; i++) {
		build_row(s_rb.list, &s_page_stage[i], i);
	}
}

static void refresh_btn_cb(lv_event_t *e)
{
	ARG_UNUSED(e);
	hpi_scr_recording_reload();
}

/* ---- public API ---- */

lv_obj_t *hpi_scr_recording_create(lv_obj_t *parent)
{
	lv_obj_t *root = lv_obj_create(parent);
	lv_obj_set_size(root, lv_pct(100), lv_pct(100));
	hpi_m3_apply_card(root, HPI_M3_SURFACE, 0);
	lv_obj_set_style_border_width(root, 0, 0);
	lv_obj_set_style_pad_all(root, 0, 0);
	lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
	s_rb.root = root;

	/* Back chevron returns to the Rec screen, not "More" -- BROWSE was
	 * launched from Rec, so that's where the user expects to land back. */
	s_rb.bar = hpi_ui_subbar_create(root, "Recordings", HPI_UI_SCREEN_REC);

	lv_obj_t *right = lv_obj_create(s_rb.bar);
	lv_obj_set_size(right, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(right, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(right, 0, 0);
	lv_obj_set_style_pad_all(right, 0, 0);
	lv_obj_clear_flag(right, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(right, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(right, refresh_btn_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *ric = lv_label_create(right);
	lv_label_set_text(ric, LV_SYMBOL_REFRESH);
	lv_obj_set_style_text_font(ric, &lv_font_montserrat_14, 0);
	lv_obj_set_style_text_color(ric, HPI_M3_ON_SURFACE_VARIANT, 0);

	s_rb.list = lv_obj_create(root);
	lv_obj_set_width(s_rb.list, lv_pct(100));
	lv_obj_set_flex_grow(s_rb.list, 1);
	lv_obj_set_style_bg_opa(s_rb.list, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(s_rb.list, 0, 0);
	lv_obj_set_style_pad_all(s_rb.list, HPI_M3_SPACE_3, 0);
	lv_obj_set_style_pad_row(s_rb.list, HPI_M3_SPACE_2, 0);
	lv_obj_set_flex_flow(s_rb.list, LV_FLEX_FLOW_COLUMN);

	s_rb.pager_bar = lv_obj_create(root);
	lv_obj_set_width(s_rb.pager_bar, lv_pct(100));
	lv_obj_set_height(s_rb.pager_bar, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(s_rb.pager_bar, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(s_rb.pager_bar, 0, 0);
	lv_obj_set_style_pad_all(s_rb.pager_bar, HPI_M3_SPACE_2, 0);
	lv_obj_clear_flag(s_rb.pager_bar, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(s_rb.pager_bar, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(s_rb.pager_bar, LV_FLEX_ALIGN_SPACE_BETWEEN,
			      LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	s_rb.pager_prev = lv_button_create(s_rb.pager_bar);
	lv_obj_set_size(s_rb.pager_prev, HPI_M3_TOUCH_MIN, HPI_M3_TOUCH_MIN);
	hpi_m3_apply_card(s_rb.pager_prev, HPI_M3_SURFACE_CONTAINER, HPI_M3_RADIUS_MD);
	lv_obj_add_event_cb(s_rb.pager_prev, pager_prev_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *ppl = lv_label_create(s_rb.pager_prev);
	lv_label_set_text(ppl, LV_SYMBOL_LEFT);
	lv_obj_set_style_text_font(ppl, &lv_font_montserrat_14, 0);
	lv_obj_center(ppl);

	s_rb.pager_label = lv_label_create(s_rb.pager_bar);
	lv_label_set_text(s_rb.pager_label, "Page 1 / 1");
	lv_obj_set_style_text_font(s_rb.pager_label, HPI_M3_FONT_CAPS_SM, 0);
	lv_obj_set_style_text_color(s_rb.pager_label, HPI_M3_ON_SURFACE_VARIANT, 0);

	s_rb.pager_next = lv_button_create(s_rb.pager_bar);
	lv_obj_set_size(s_rb.pager_next, HPI_M3_TOUCH_MIN, HPI_M3_TOUCH_MIN);
	hpi_m3_apply_card(s_rb.pager_next, HPI_M3_SURFACE_CONTAINER, HPI_M3_RADIUS_MD);
	lv_obj_add_event_cb(s_rb.pager_next, pager_next_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *pnl = lv_label_create(s_rb.pager_next);
	lv_label_set_text(pnl, LV_SYMBOL_RIGHT);
	lv_obj_set_style_text_font(pnl, &lv_font_montserrat_14, 0);
	lv_obj_center(pnl);

	s_rb.empty_label = lv_label_create(root);
	lv_label_set_text(s_rb.empty_label, "No recordings yet");
	lv_obj_set_style_text_font(s_rb.empty_label, HPI_M3_FONT_BODY, 0);
	lv_obj_set_style_text_color(s_rb.empty_label, HPI_M3_ON_SURFACE_VARIANT, 0);
	lv_obj_align(s_rb.empty_label, LV_ALIGN_CENTER, 0, 0);
	lv_obj_add_flag(s_rb.empty_label, LV_OBJ_FLAG_HIDDEN);

	hpi_scr_recording_reload();
	return root;
}

static void pager_prev_cb(lv_event_t *e)
{
	ARG_UNUSED(e);
	if (s_cur_page > 0) request_page(s_cur_page - 1);
}

static void pager_next_cb(lv_event_t *e)
{
	ARG_UNUSED(e);
	uint32_t total_pages = (s_total_count + REC_PAGE_SIZE - 1) / REC_PAGE_SIZE;
	if (s_cur_page + 1 < total_pages) request_page(s_cur_page + 1);
}

static void update_pager_ui(void)
{
	if (!s_rb.pager_label) return;

	uint32_t total_pages = (s_total_count + REC_PAGE_SIZE - 1) / REC_PAGE_SIZE;
	if (total_pages == 0) total_pages = 1;

	char buf[32];
	if (s_loading) {
		snprintf(buf, sizeof(buf), "Loading...");
	} else {
		snprintf(buf, sizeof(buf), "Page %u / %u", s_cur_page + 1, total_pages);
	}
	lv_label_set_text(s_rb.pager_label, buf);

	bool can_prev = !s_loading && s_cur_page > 0;
	bool can_next = !s_loading && (s_cur_page + 1 < total_pages);
	lv_obj_set_state(s_rb.pager_prev, LV_STATE_DISABLED, !can_prev);
	lv_obj_set_state(s_rb.pager_next, LV_STATE_DISABLED, !can_next);
}

void hpi_scr_recording_refresh(void)
{
	if (s_rb.list == NULL) {
		return;
	}

	if (s_awaiting_index) {
		if (!atomic_get(&s_index_ready)) return;
		s_awaiting_index = false;
		atomic_set(&s_index_ready, 0);
		request_page(s_cur_page);
		return;
	}

	if (s_awaiting_page) {
		if (!atomic_get(&s_page_ready)) return;
		s_awaiting_page = false;
		atomic_set(&s_page_ready, 0);
		s_loading = false;

		if (s_detail_pending) {
			s_detail_pending = false;
			if (s_detail_pending_local < s_page_stage_n) {
				uint32_t global_idx = s_cur_page * REC_PAGE_SIZE +
						      s_detail_pending_local;
				open_detail(&s_page_stage[s_detail_pending_local], global_idx);
			}
		}

		rebuild_list();
		update_pager_ui();
	}
}