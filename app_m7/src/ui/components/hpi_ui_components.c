/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Implementation of the modular UI components. LVGL 9 API; direct style setters
 * only (no static lv_style_t — they hard-fault after VDB realloc).
 */
#include "hpi_ui_components.h"
#include "../assets/hpi_logo.h"
#include "../fonts/hpi_symbols.h"
#include "../theme/hpi_m3_theme.h"

#include "core/sample_formats.h"   /* HP6_LEAD_OFF_* electrode mask */
#include "services/power_service.h"
#include "services/connectivity/healthybridge_service.h"
#include "../ui_module.h"

#include <stdio.h>
#include <string.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(hpi_ui, CONFIG_HPI_APP_LOG_LEVEL);

/* "No value yet" placeholder. ASCII only: the design faces carry ASCII, but the
 * truthful-data rule matters more than the glyph — never fabricate a reading. */
#define SB_NA_STR  "--"

/* Waveform display scaling (fixed chart range + data gain). The chart Y range is
 * a constant ±WF_CHART_HALF; the gain maps the signal's windowed peak to
 * ±WF_TARGET (~80% height, leaving headroom). WF_GAIN_Q is the Q-shift of the
 * fixed-point gain. */
#define WF_CHART_HALF  1000
#define WF_TARGET      800
#define WF_GAIN_Q      16

/* ---- top app bar ---- */
lv_obj_t *hpi_ui_appbar_create(lv_obj_t *parent, const char *title)
{
	lv_obj_t *bar = lv_obj_create(parent);
	lv_obj_set_size(bar, lv_pct(100), 48);
	hpi_m3_apply_card(bar, HPI_M3_SURFACE_CONTAINER, 0);
	lv_obj_set_style_pad_hor(bar, HPI_M3_SPACE_4, 0);
	lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN,
			      LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	lv_obj_t *t = lv_label_create(bar);
	lv_label_set_text(t, title);
	lv_obj_set_style_text_font(t, HPI_M3_FONT_TITLE, 0);
	lv_obj_set_style_text_color(t, HPI_M3_ON_SURFACE, 0);
	return bar;
}

/* ---- brand lockup ---- */

/* One A8 layer of the lockup, filling the parent and tinted `c`. A8 carries
 * alpha only, so the colour comes from the theme recolor, not the asset.
 * LV_IMAGE_ALIGN_STRETCH scales both layers from the same source geometry, so
 * they cannot drift out of registration. */
static void logo_layer(lv_obj_t *parent, const lv_image_dsc_t *src, lv_color_t c,
		       int w, int h)
{
	lv_obj_t *img = lv_image_create(parent);

	lv_image_set_src(img, src);
	lv_obj_set_size(img, w, h);
	lv_image_set_inner_align(img, LV_IMAGE_ALIGN_STRETCH);
	lv_obj_set_style_image_recolor(img, c, 0);
	lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
	lv_obj_align(img, LV_ALIGN_TOP_LEFT, 0, 0);
}

lv_obj_t *hpi_ui_logo_create(lv_obj_t *parent, int width_px)
{
	int h = (width_px * HPI_LOGO_LOCKUP_H + HPI_LOGO_LOCKUP_W / 2) /
		HPI_LOGO_LOCKUP_W;

	lv_obj_t *box = lv_obj_create(parent);

	lv_obj_set_size(box, width_px, h);
	lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(box, 0, 0);
	lv_obj_set_style_pad_all(box, 0, 0);
	lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

	logo_layer(box, &hpi_logo_lockup_light, HPI_M3_ON_SURFACE, width_px, h);
	logo_layer(box, &hpi_logo_lockup_accent, HPI_M3_SIG_ECG, width_px, h);
	return box;
}

/* ---- shared top status bar --------------------------------------------------
 *
 * On every screen except the splash, the boot self-test and the ambient clock.
 * One transparent bar, one registry, one refresh pass. Bars belong to screens,
 * which are built once during ui_build_screens() and never deleted, so entries
 * are never removed -- a bar outliving its screen would be a bug elsewhere.
 */
#define SB_MAX  16

static struct statusbar_inst {
	lv_obj_t *bar;
	lv_obj_t *left;      /* title/date + anything a screen puts ahead of it */
	lv_obj_t *title;     /* the title label, or the date label when `date`  */
	lv_obj_t *link;      /* ESP32 link glyph (hidden while the link is down) */
	lv_obj_t *usb;       /* USB glyph (hidden while no host is attached)     */
	lv_obj_t *batt;      /* battery percentage                              */
	lv_obj_t *clock;     /* HH:MM                                           */
	bool      date;      /* left slot carries the live date, not a title     */
} s_sb[SB_MAX];
static uint8_t s_sb_n;

static struct statusbar_inst *sb_find(lv_obj_t *bar)
{
	for (uint8_t i = 0; i < s_sb_n; i++) {
		if (s_sb[i].bar == bar) {
			return &s_sb[i];
		}
	}
	return NULL;
}

/* lv_label_set_text() reallocates the string and invalidates the label whether
 * or not the text changed, and the refresh below walks every screen's bar twice
 * a second. Only touch a label whose text actually moved. */
static void sb_label_set(lv_obj_t *l, const char *text)
{
	if (l == NULL || strcmp(lv_label_get_text(l), text) == 0) {
		return;
	}
	lv_label_set_text(l, text);
}

static lv_obj_t *sb_text(lv_obj_t *parent, const char *init, lv_color_t color)
{
	lv_obj_t *l = lv_label_create(parent);

	lv_label_set_text(l, init);
	lv_obj_set_style_text_font(l, HPI_M3_FONT_LABEL, 0);
	lv_obj_set_style_text_color(l, color, 0);
	return l;
}

static lv_obj_t *sb_icon(lv_obj_t *parent, const char *glyph, lv_color_t color)
{
	lv_obj_t *l = lv_label_create(parent);

	lv_label_set_text(l, glyph);
	lv_obj_set_style_text_font(l, HPI_M3_FONT_ICON, 0);
	lv_obj_set_style_text_color(l, color, 0);
	return l;
}

/* A transparent, content-sized flex row (the bar's left and right groups). */
static lv_obj_t *sb_group(lv_obj_t *bar, lv_flex_align_t justify)
{
	lv_obj_t *g = lv_obj_create(bar);

	lv_obj_set_size(g, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(g, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(g, 0, 0);
	lv_obj_set_style_pad_all(g, 0, 0);
	lv_obj_set_style_pad_column(g, HPI_M3_SPACE_2, 0);
	lv_obj_clear_flag(g, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(g, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(g, justify, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	return g;
}

lv_obj_t *hpi_ui_statusbar_create(lv_obj_t *parent, const char *title)
{
	lv_obj_t *bar = lv_obj_create(parent);

	lv_obj_set_size(bar, lv_pct(100), HPI_M3_APPBAR_H);
	lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(bar, 0, 0);
	lv_obj_set_style_pad_hor(bar, HPI_M3_SPACE_3, 0);
	lv_obj_set_style_pad_ver(bar, 0, 0);
	lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);

	lv_obj_t *left = sb_group(bar, LV_FLEX_ALIGN_START);
	lv_obj_t *t;

	if (title != NULL) {
		t = lv_label_create(left);
		lv_label_set_text(t, title);
		lv_obj_set_style_text_font(t, HPI_M3_FONT_TITLE, 0);
		lv_obj_set_style_text_color(t, HPI_M3_ON_SURFACE, 0);
	} else {
		t = sb_text(left, SB_NA_STR, HPI_M3_ON_SURFACE_VARIANT);
	}

	lv_obj_t *right = sb_group(bar, LV_FLEX_ALIGN_END);

	/* Link glyph: the ESP32 co-processor LINK, not the WiFi association
	 * state. hpi_connectivity_wifi_status() can block on a co-processor
	 * round-trip once its 1.5 s cache goes stale, which the LVGL thread must
	 * never do -- the bar reads only hpi_connectivity_ready() (a flag) and
	 * the Link screen owns the real WiFi detail. */
	struct statusbar_inst *b = (s_sb_n < SB_MAX) ? &s_sb[s_sb_n++] : NULL;

	lv_obj_t *link = sb_icon(right, HPI_SYM_WIFI, HPI_M3_PRIMARY);
	lv_obj_add_flag(link, LV_OBJ_FLAG_HIDDEN);
	/* Shown only while a USB host is attached; hidden otherwise. */
	lv_obj_t *usb = sb_icon(right, HPI_SYM_USB, HPI_M3_SUCCESS);
	lv_obj_add_flag(usb, LV_OBJ_FLAG_HIDDEN);
	sb_icon(right, HPI_SYM_BATT, HPI_M3_SUCCESS);
	lv_obj_t *batt = sb_text(right, SB_NA_STR, HPI_M3_ON_SURFACE_VARIANT);
	lv_obj_t *clock = sb_text(right, SB_NA_STR, HPI_M3_ON_SURFACE);

	if (b == NULL) {
		/* Bar still renders; its clock/battery just never tick. Say so
		 * rather than leaving a dead bar to be discovered on the panel. */
		LOG_WRN("status bar registry full (%d) — this bar will not update",
			SB_MAX);
		return bar;
	}
	*b = (struct statusbar_inst){
		.bar = bar, .left = left, .title = t, .link = link,
		.usb = usb, .batt = batt, .clock = clock, .date = (title == NULL),
	};
	return bar;
}

void hpi_ui_statusbar_set_title(lv_obj_t *bar, const char *title)
{
	struct statusbar_inst *b = sb_find(bar);

	if (b != NULL && !b->date && title != NULL) {
		sb_label_set(b->title, title);
	}
}

lv_obj_t *hpi_ui_statusbar_left(lv_obj_t *bar)
{
	struct statusbar_inst *b = sb_find(bar);

	return b ? b->left : NULL;
}

/* Battery + USB, from one power snapshot. Attachment (from enumeration) and
 * charging (from the BQ24074's CHG pin) are separate signals: charging is
 * shown as a COLOUR on the percentage, attachment as the USB glyph. */
static void sb_render_power(struct statusbar_inst *b,
			    const struct hpi_power_status *p)
{
	char pct[8];

	lv_obj_set_flag(b->usb, LV_OBJ_FLAG_HIDDEN, !p->usb_present);

	if (!p->valid) {
		sb_label_set(b->batt, SB_NA_STR);
		lv_obj_set_style_text_color(b->batt, HPI_M3_ON_SURFACE_MUTED, 0);
		return;
	}
	snprintf(pct, sizeof(pct), "%u%%", p->soc_pct);
	sb_label_set(b->batt, pct);
	lv_obj_set_style_text_color(b->batt,
		(p->charge_state == HPI_CHG_CHARGING || p->charge_state == HPI_CHG_FULL)
			? HPI_M3_SUCCESS : HPI_M3_ON_SURFACE_VARIANT, 0);
}

void hpi_ui_statusbar_refresh(void)
{
	static const char *const WD[7] = { "SUN", "MON", "TUE", "WED",
					   "THU", "FRI", "SAT" };
	static const char *const MO[12] = { "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
					    "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };
	const struct device *rtc = DEVICE_DT_GET_OR_NULL(DT_ALIAS(rtc));
	char hhmm[8] = SB_NA_STR;
	char date[16] = SB_NA_STR;
	struct hpi_power_status p;
	struct rtc_time t;

	/* One read of each source for the whole set of bars. rtc_get_time() fails
	 * with -ENODATA until the calendar has been set once (the STM32 INITS
	 * flag), which is why an un-set device shows "--" for both. */
	if (rtc != NULL && device_is_ready(rtc) && rtc_get_time(rtc, &t) == 0) {
		int wd = (t.tm_wday >= 0 && t.tm_wday < 7) ? t.tm_wday : 0;
		int mo = (t.tm_mon >= 0 && t.tm_mon < 12) ? t.tm_mon : 0;

		snprintf(hhmm, sizeof(hhmm), "%02d:%02d", t.tm_hour, t.tm_min);
		snprintf(date, sizeof(date), "%s %02d %s", WD[wd], t.tm_mday, MO[mo]);
	}
	hpi_power_get(&p);

	bool link_up = hpi_connectivity_ready();

	for (uint8_t i = 0; i < s_sb_n; i++) {
		struct statusbar_inst *b = &s_sb[i];

		sb_label_set(b->clock, hhmm);
		if (b->date) {
			sb_label_set(b->title, date);
		}
		lv_obj_set_flag(b->link, LV_OBJ_FLAG_HIDDEN, !link_up);
		sb_render_power(b, &p);
	}
}

/* ---- sub-screen header (status bar + back chevron) ---- */
static void subbar_back_cb(lv_event_t *e)
{
	intptr_t back = (intptr_t)lv_event_get_user_data(e);
	hpi_ui_show_screen((enum hpi_ui_screen)back);
}

lv_obj_t *hpi_ui_subbar_create(lv_obj_t *parent, const char *title, int back_to)
{
	lv_obj_t *bar = hpi_ui_statusbar_create(parent, title);

	/* Taller than a plain status bar on purpose: the back chevron is a full
	 * HPI_M3_TOUCH_MIN target. */
	lv_obj_set_height(bar, HPI_M3_TOUCH_MIN);

	lv_obj_t *left = hpi_ui_statusbar_left(bar);
	lv_obj_t *back = lv_button_create(left ? left : bar);

	lv_obj_set_size(back, HPI_M3_TOUCH_MIN, HPI_M3_TOUCH_MIN);
	lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(back, 0, 0);
	lv_obj_set_style_pad_all(back, 0, 0);
	lv_obj_add_event_cb(back, subbar_back_cb, LV_EVENT_CLICKED,
			    (void *)(intptr_t)back_to);
	lv_obj_move_to_index(back, 0);   /* ahead of the title */

	lv_obj_t *ba = lv_label_create(back);

	lv_label_set_text(ba, LV_SYMBOL_LEFT);
	lv_obj_center(ba);
	lv_obj_set_style_text_font(ba, HPI_M3_FONT_SYMBOL, 0);
	lv_obj_set_style_text_color(ba, HPI_M3_ON_SURFACE_VARIANT, 0);
	return bar;
}

/* ---- navigation bar (5-slot M3 bottom nav) ---- */
static const char *const NAV_LABEL[HPI_UI_MAIN_TAB_COUNT] = {
	"Home", "Live", "Rec", "Trends", "More",
};
static const char *const NAV_ICON[HPI_UI_MAIN_TAB_COUNT] = {
	HPI_SYM_HOME, HPI_SYM_LIVE, HPI_SYM_REC, HPI_SYM_TRENDS, HPI_SYM_MORE,
};

static void nav_event_cb(lv_event_t *e)
{
	intptr_t scr = (intptr_t)lv_event_get_user_data(e);
	hpi_ui_show_screen((enum hpi_ui_screen)scr);
}

/* Item = a transparent button (icon over label). Active gets a brand-tinted pill
 * fill + brand-blue icon/label; inactive is transparent + muted. */
static void style_nav_item(lv_obj_t *btn, bool on)
{
	lv_obj_set_style_bg_color(btn, HPI_M3_PRIMARY_CONTAINER, 0);
	lv_obj_set_style_bg_opa(btn, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
	lv_obj_set_style_radius(btn, HPI_M3_RADIUS_LG, 0);
	lv_obj_set_style_border_width(btn, 0, 0);

	lv_color_t c = on ? HPI_M3_PRIMARY : HPI_M3_ON_SURFACE_VARIANT;
	uint32_t n = lv_obj_get_child_count(btn);
	for (uint32_t i = 0; i < n; i++) {
		lv_obj_set_style_text_color(lv_obj_get_child(btn, i), c, 0);
	}
}

void hpi_ui_navbar_set_active(lv_obj_t *bar, int active)
{
	/* A "More" submenu screen (>= the first non-tab screen) keeps the More
	 * tab lit so the user knows where they are. */
	int tab = (active >= HPI_UI_MAIN_TAB_COUNT) ? HPI_UI_SCREEN_MORE : active;

	for (int i = 0; i < HPI_UI_MAIN_TAB_COUNT; i++) {
		lv_obj_t *btn = lv_obj_get_child(bar, i);
		if (btn) {
			style_nav_item(btn, i == tab);
		}
	}
}

lv_obj_t *hpi_ui_navbar_create(lv_obj_t *parent, int active)
{
	lv_obj_t *bar = lv_obj_create(parent);
	lv_obj_set_size(bar, lv_pct(100), HPI_M3_NAVBAR_H);
	hpi_m3_apply_card(bar, HPI_M3_SURFACE_CONTAINER, 0);
	lv_obj_set_style_pad_all(bar, HPI_M3_SPACE_1, 0);
	/* Real gap between adjacent destinations, so a near-miss on one tab does
	 * not register as the next one over. */
	lv_obj_set_style_pad_column(bar, HPI_M3_SPACE_2, 0);
	lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
	lv_obj_set_style_border_width(bar, 1, 0);
	lv_obj_set_style_border_color(bar, HPI_M3_OUTLINE, 0);
	lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_EVENLY,
			      LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	for (int i = 0; i < HPI_UI_MAIN_TAB_COUNT; i++) {
		lv_obj_t *btn = lv_button_create(bar);
		lv_obj_set_flex_grow(btn, 1);        /* 5 equal-width items */
		lv_obj_set_height(btn, lv_pct(100));
		hpi_m3_apply_touch(btn);
		lv_obj_set_style_pad_all(btn, HPI_M3_SPACE_1, 0);
		lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
				      LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_row(btn, 2, 0);
		lv_obj_add_event_cb(btn, nav_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

		lv_obj_t *ic = lv_label_create(btn);         /* icon (child 0) */
		lv_label_set_text(ic, NAV_ICON[i]);
		lv_obj_set_style_text_font(ic, HPI_M3_FONT_ICON_LG, 0);

		lv_obj_t *l = lv_label_create(btn);          /* label (child 1) */
		lv_label_set_text(l, NAV_LABEL[i]);
		lv_obj_set_style_text_font(l, HPI_M3_FONT_CAPS_SM, 0);

		style_nav_item(btn, i == active);
	}
	return bar;
}

/* ---- ECG lead-off naming ---- */
void hpi_ui_lead_off_text(uint8_t mask, char *buf, size_t len)
{
	static const struct {
		uint8_t bit;
		const char *name;
	} E[] = {
		{ HP6_LEAD_OFF_RA, "RA" },
		{ HP6_LEAD_OFF_LA, "LA" },
		{ HP6_LEAD_OFF_LL, "LL" },
		{ HP6_LEAD_OFF_V1, "V1" },
	};
	size_t used = 0;

	if (buf == NULL || len == 0) {
		return;
	}
	buf[0] = '\0';
	for (size_t i = 0; i < ARRAY_SIZE(E) && used + 8 < len; i++) {
		if ((mask & E[i].bit) == 0) {
			continue;
		}
		/* "\xC2\xB7" is the middle dot the rest of the UI separates with. */
		used += (size_t)snprintf(buf + used, len - used, "%s%s",
					 used ? " \xC2\xB7 " : "", E[i].name);
	}
}

/* ---- stepper ---- */

/* One key of the stepper: a HPI_M3_TOUCH_MIN square, so the tap area is the
 * whole key and not just the glyph. */
static lv_obj_t *stepper_key(lv_obj_t *parent, const char *sym,
			     lv_event_cb_t cb, struct hpi_ui_stepper *s)
{
	lv_obj_t *b = lv_button_create(parent);

	lv_obj_set_size(b, HPI_M3_TOUCH_MIN, HPI_M3_TOUCH_MIN);
	lv_obj_set_style_bg_color(b, HPI_M3_SURFACE_HIGH, 0);
	lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(b, HPI_M3_RADIUS_PILL, 0);
	lv_obj_set_style_border_width(b, 0, 0);
	lv_obj_set_style_pad_all(b, 0, 0);
	lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, s);

	lv_obj_t *l = lv_label_create(b);

	lv_label_set_text(l, sym);
	lv_obj_center(l);
	lv_obj_set_style_text_font(l, HPI_M3_FONT_ICON_LG, 0);
	lv_obj_set_style_text_color(l, HPI_M3_ON_SURFACE, 0);
	return b;
}

void hpi_ui_stepper_set(struct hpi_ui_stepper *s, int32_t v)
{
	char b[16];

	if (v < s->min) {
		v = s->min;
	} else if (v > s->max) {
		v = s->max;
	}
	s->v = v;
	if (s->value) {
		snprintf(b, sizeof(b), s->fmt, (int)v);
		lv_label_set_text(s->value, b);
	}
}

static void stepper_bump(struct hpi_ui_stepper *s, int32_t d)
{
	int32_t before = s->v;

	hpi_ui_stepper_set(s, s->v + d);
	if (s->v != before && s->on_change) {
		s->on_change(s->v, s->user);
	}
}

static void stepper_dec_cb(lv_event_t *e)
{
	struct hpi_ui_stepper *s = lv_event_get_user_data(e);

	stepper_bump(s, -s->step);
}

static void stepper_inc_cb(lv_event_t *e)
{
	struct hpi_ui_stepper *s = lv_event_get_user_data(e);

	stepper_bump(s, s->step);
}

void hpi_ui_stepper_create(struct hpi_ui_stepper *s, lv_obj_t *parent,
			   int32_t min, int32_t max, int32_t step, int32_t init,
			   const char *fmt,
			   void (*on_change)(int32_t v, void *user), void *user)
{
	s->min = min;
	s->max = max;
	s->step = step ? step : 1;
	s->fmt = fmt ? fmt : "%d";
	s->on_change = on_change;
	s->user = user;
	s->value = NULL;

	/* Content-width by default so it drops into a row; a caller that wants it
	 * to span a card sets lv_obj_set_width(s->root, lv_pct(100)). */
	s->root = lv_obj_create(parent);
	lv_obj_set_size(s->root, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(s->root, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(s->root, 0, 0);
	lv_obj_set_style_pad_all(s->root, 0, 0);
	lv_obj_set_style_pad_column(s->root, HPI_M3_TOUCH_GAP, 0);
	lv_obj_clear_flag(s->root, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(s->root, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(s->root, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);

	stepper_key(s->root, HPI_SYM_MINUS, stepper_dec_cb, s);

	/* Fixed width, centred. The number must not resize its own row as it
	 * changes -- a content-sized label reflows the whole parent chain on
	 * every tap, and does it from inside LVGL's input processing. */
	s->value = lv_label_create(s->root);
	lv_obj_set_width(s->value, 96);
	lv_obj_set_style_text_align(s->value, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(s->value, HPI_M3_FONT_NUMERAL, 0);
	lv_obj_set_style_text_color(s->value, HPI_M3_ON_SURFACE, 0);

	stepper_key(s->root, HPI_SYM_PLUS, stepper_inc_cb, s);

	hpi_ui_stepper_set(s, init);
}

/* ---- charts ---- */
void hpi_ui_chart_hide_points(lv_obj_t *chart)
{
	/* See the header: the markers live on LV_PART_INDICATOR, and the last
	 * point of each series is drawn whatever its size, so kill the opacity
	 * as well as the size. */
	lv_obj_set_style_width(chart, 0, LV_PART_INDICATOR);
	lv_obj_set_style_height(chart, 0, LV_PART_INDICATOR);
	lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, LV_PART_INDICATOR);
}

/* ---- waveform panel ---- */
void hpi_ui_waveform_create(struct hpi_ui_waveform *w, lv_obj_t *parent,
			    const char *label, lv_color_t color,
			    uint16_t points, int32_t ymin, int32_t ymax,
			    uint8_t bl_shift)
{
	w->panel = lv_obj_create(parent);
	lv_obj_set_width(w->panel, lv_pct(100));
	lv_obj_set_flex_grow(w->panel, 1);
	hpi_m3_apply_card(w->panel, HPI_M3_SURFACE_CONTAINER, HPI_M3_RADIUS_MD);
	lv_obj_set_style_pad_all(w->panel, HPI_M3_SPACE_2, 0);
	lv_obj_clear_flag(w->panel, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(w->panel, LV_FLEX_FLOW_COLUMN);

	lv_obj_t *cap = lv_label_create(w->panel);
	lv_label_set_text(cap, label);
	lv_obj_set_style_text_font(cap, HPI_M3_FONT_LABEL_SM, 0);
	lv_obj_set_style_text_color(cap, color, 0);

	w->chart = lv_chart_create(w->panel);
	lv_obj_set_width(w->chart, lv_pct(100));
	lv_obj_set_flex_grow(w->chart, 1);
	lv_chart_set_type(w->chart, LV_CHART_TYPE_LINE);
	lv_chart_set_update_mode(w->chart, LV_CHART_UPDATE_MODE_SHIFT);
	lv_chart_set_point_count(w->chart, points);
	lv_chart_set_div_line_count(w->chart, 0, 0);
	lv_obj_set_style_bg_opa(w->chart, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(w->chart, 0, 0);
	/* Line only, no per-sample markers; set the width explicitly rather than
	 * inheriting it. */
	lv_obj_set_style_line_width(w->chart, 3, LV_PART_ITEMS);
	hpi_ui_chart_hide_points(w->chart);
	w->series = lv_chart_add_series(w->chart, color, LV_CHART_AXIS_PRIMARY_Y);

	/* FIXED chart range -- set once, never changed (a per-frame range change
	 * flickers the whole screen). Auto-scaling is a gain on the data (see
	 * push): the windowed peak is mapped to WF_TARGET display units. The
	 * passed ±half only seeds the noise floor + the initial gain. */
	int32_t half = (ymax - ymin) / 2;
	if (half < 2) {
		half = 2;
	}
	w->floor    = half / 64 > 0 ? half / 64 : 1;
	w->peak     = 0;
	w->baseline = 0;
	w->recount  = 0;
	w->user_q   = 256;   /* x1 -- the auto-scaled amplitude, unmodified */
	w->bl_shift = bl_shift ? bl_shift : 9;
	w->primed   = false;

	int32_t init_peak = half / 2 > w->floor ? half / 2 : w->floor;

	w->gain_q = (int32_t)(((int64_t)WF_TARGET << WF_GAIN_Q) / init_peak);
	lv_chart_set_range(w->chart, LV_CHART_AXIS_PRIMARY_Y, -WF_CHART_HALF, WF_CHART_HALF);
}

void hpi_ui_waveform_set_zoom(struct hpi_ui_waveform *w, int32_t mult_q8)
{
	w->user_q = mult_q8 > 0 ? mult_q8 : 256;
}

void hpi_ui_waveform_set_points(struct hpi_ui_waveform *w, uint16_t points)
{
	if (w->chart == NULL || points == 0) {
		return;
	}
	lv_chart_set_point_count(w->chart, points);
}

void hpi_ui_waveform_push(struct hpi_ui_waveform *w, int32_t v)
{
	if (!w->chart || !w->series) {
		return;
	}

	/* Seed the baseline on the first sample so it converges immediately
	 * instead of sweeping up from zero. */
	if (!w->primed) {
		w->baseline = v;
		w->primed = true;
	}

	/* DC-block: EMA baseline subtracted -> high-pass. The per-waveform shift
	 * sets the cutoff: small (PPG) tracks heavy baseline wander, large (ECG)
	 * preserves the slow components of a steadier trace. */
	w->baseline += (v - w->baseline) >> w->bl_shift;
	int32_t ac = v - w->baseline;

	int32_t a = ac < 0 ? -ac : ac;
	if (a > w->peak) {
		w->peak = a;
	}

	/* Map AC -> fixed display window via the current gain and the user's zoom,
	 * then clamp. The chart range never moves, so there is no per-frame range
	 * change. Clamping is what makes over-zoom safe: the trace flattens against
	 * the window edges rather than corrupting the chart. */
	int32_t disp = (int32_t)((((int64_t)ac * w->gain_q) >> WF_GAIN_Q) *
				 w->user_q >> 8);

	if (disp > WF_CHART_HALF) {
		disp = WF_CHART_HALF;
	} else if (disp < -WF_CHART_HALF) {
		disp = -WF_CHART_HALF;
	}
	lv_chart_set_next_value(w->chart, w->series, disp);

	/* Adapt the gain a few times/sec so the windowed peak fills ~WF_TARGET of
	 * the window; damped to avoid visible amplitude jumps, with the peak
	 * decayed so the gain can also rise when the signal shrinks. floor caps the
	 * gain so a flat/noisy input is not zoomed to full height. */
	if (++w->recount >= 128) {
		w->recount = 0;
		int32_t p = w->peak > w->floor ? w->peak : w->floor;
		int32_t target_gain = (int32_t)(((int64_t)WF_TARGET << WF_GAIN_Q) / p);

		w->gain_q += (target_gain - w->gain_q) >> 2;
		if (w->gain_q < 1) {
			w->gain_q = 1;
		}
		w->peak -= w->peak >> 2;
	}
}

/* ---- vitals tile ---- */
void hpi_ui_vitals_tile_create(struct hpi_ui_vitals_tile *t, lv_obj_t *parent,
			       const char *caption, lv_color_t value_color)
{
	lv_obj_t *tile = lv_obj_create(parent);
	lv_obj_set_flex_grow(tile, 1);
	lv_obj_set_height(tile, LV_SIZE_CONTENT);
	hpi_m3_apply_card(tile, HPI_M3_SURFACE_HIGH, HPI_M3_RADIUS_SM);
	lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);

	lv_obj_t *cap = lv_label_create(tile);
	lv_label_set_text(cap, caption);
	lv_obj_set_style_text_font(cap, HPI_M3_FONT_LABEL_SM, 0);
	lv_obj_set_style_text_color(cap, HPI_M3_ON_SURFACE_VARIANT, 0);

	t->value = lv_label_create(tile);
	lv_label_set_text(t->value, "--");
	lv_obj_set_style_text_font(t->value, HPI_M3_FONT_HEADLINE, 0);
	lv_obj_set_style_text_color(t->value, value_color, 0);
}

void hpi_ui_vitals_tile_set(struct hpi_ui_vitals_tile *t, const char *text)
{
	if (t->value) {
		lv_label_set_text(t->value, text);
	}
}
