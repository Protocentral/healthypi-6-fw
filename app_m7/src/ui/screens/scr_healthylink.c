/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * HealthyLink — expansion module status and slot power.
 *
 * Every line here is a snapshot of something the firmware measured once, and
 * the screen's job is to be exact about which "once" that was:
 *
 *   - Slot detection runs at boot, in hl_framework_init(), off each slot's own
 *     ID EEPROM (slot A @0x50, slot B @0x51). It runs again only when a slot
 *     is switched on from here or over group 64 -- that is the rescan.
 *   - The HLink handshake runs once when a Compute module starts, on its own
 *     work queue.
 *
 * So the 2 Hz refresh below is repainting a picture that changes only when
 * something is switched, not polling a live one, and the footer says so.
 *
 * THREE THINGS THIS SCREEN MUST NOT DO, each of which was a live temptation:
 *   1. Render a slot with no ID EEPROM path as "Empty". hl_get_slot_status()
 *      then returns a DEFAULT, not a measurement (`detectable` is false), and
 *      "empty" would state that a scan happened and found nothing.
 *   2. Paint a switch position the firmware has not reported. A tap queues
 *      the change; the switch moves when hl_get_slot_status() says the load
 *      switch moved, and the slot reads SWITCHING until then.
 *   3. Render "not enabled in this build" or "no module fitted" as a failure.
 */
#include "scr_healthylink.h"
#include "../components/hpi_ui_components.h"
#include "../fonts/hpi_symbols.h"
#include "../theme/hpi_m3_theme.h"
#include "../ui_module.h"

#include "healthylink/healthylink_service.h"
#include "healthylink/hlink_proto.h"
#include "healthylink/mod_npu.h"
#include <healthylink/healthylink.h>   /* HEALTHYLINK_MODULE_ID_COMPUTE */
#include "core/sample_formats.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct hl_slot_widgets {
	lv_obj_t *chip;
	lv_obj_t *name;
	lv_obj_t *id;
	lv_obj_t *sw;
	lv_obj_t *knob;
};

static struct {
	struct hl_slot_widgets slot[HL_NUM_SLOTS];
	/* HLink */
	lv_obj_t *hl_chip;
	lv_obj_t *hl_detail;
	lv_obj_t *hl_engine;
	lv_obj_t *hl_counters;
	lv_obj_t *hl_beat;
	bool built;
} s_hl;

/* ---- local builders (mirroring scr_link.c's card and toggle) ---- */

static lv_obj_t *hl_card(lv_obj_t *parent, const char *icon, lv_color_t icol,
			 const char *name, lv_obj_t **hdr_out)
{
	lv_obj_t *c = lv_obj_create(parent);

	lv_obj_set_width(c, lv_pct(100));
	lv_obj_set_height(c, LV_SIZE_CONTENT);
	hpi_m3_apply_card(c, HPI_M3_SURFACE_CONTAINER, HPI_M3_RADIUS_LG);
	lv_obj_set_style_pad_all(c, HPI_M3_SPACE_4, 0);
	lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(c, HPI_M3_SPACE_2, 0);

	lv_obj_t *hdr = lv_obj_create(c);

	lv_obj_set_width(hdr, lv_pct(100));
	lv_obj_set_height(hdr, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(hdr, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(hdr, 0, 0);
	lv_obj_set_style_pad_all(hdr, 0, 0);
	lv_obj_set_style_pad_column(hdr, HPI_M3_SPACE_2, 0);
	lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);

	lv_obj_t *ic = lv_label_create(hdr);

	lv_label_set_text(ic, icon);
	lv_obj_set_style_text_font(ic, HPI_M3_FONT_ICON, 0);
	lv_obj_set_style_text_color(ic, icol, 0);

	lv_obj_t *nm = lv_label_create(hdr);

	lv_label_set_text(nm, name);
	lv_obj_set_style_text_font(nm, HPI_M3_FONT_CAPS, 0);
	lv_obj_set_style_text_color(nm, HPI_M3_ON_SURFACE, 0);

	lv_obj_t *sp = lv_obj_create(hdr);   /* growable spacer -> chip goes right */

	lv_obj_set_style_bg_opa(sp, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(sp, 0, 0);
	lv_obj_set_style_pad_all(sp, 0, 0);
	lv_obj_set_height(sp, 1);
	lv_obj_set_flex_grow(sp, 1);
	lv_obj_clear_flag(sp, LV_OBJ_FLAG_SCROLLABLE);

	if (hdr_out) {
		*hdr_out = hdr;
	}
	return c;
}

/* A mono detail line inside a card. */
static lv_obj_t *hl_detail_label(lv_obj_t *parent, const char *init, lv_color_t col)
{
	lv_obj_t *l = lv_label_create(parent);

	lv_label_set_text(l, init);
	lv_obj_set_style_text_font(l, HPI_M3_FONT_MONO, 0);
	lv_obj_set_style_text_color(l, col, 0);
	lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(l, lv_pct(100));
	return l;
}

/* Caption text: the small print that keeps a card honest. */
static lv_obj_t *hl_caption(lv_obj_t *parent, const char *text)
{
	lv_obj_t *l = lv_label_create(parent);

	lv_label_set_text(l, text);
	lv_obj_set_style_text_font(l, HPI_M3_FONT_LABEL, 0);
	lv_obj_set_style_text_color(l, HPI_M3_ON_SURFACE_FAINT, 0);
	lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(l, lv_pct(100));
	return l;
}

/*
 * A 44x24 toggle -- the same control as scr_link.c's.
 *
 * Not hpi_m3_apply_touch(): its 64 px minimums would deform the track into a
 * square. The expanded click area meets the touch floor instead (44+2*20 = 84
 * wide, 24+2*20 = 64 tall).
 */
static lv_obj_t *hl_toggle(lv_obj_t *parent, lv_obj_t **knob_out)
{
	lv_obj_t *sw = lv_obj_create(parent);

	lv_obj_set_size(sw, 44, 24);
	lv_obj_set_ext_click_area(sw, 20);
	lv_obj_set_style_radius(sw, 12, 0);
	lv_obj_set_style_bg_color(sw, HPI_M3_OUTLINE, 0);
	lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(sw, 0, 0);
	lv_obj_clear_flag(sw, LV_OBJ_FLAG_SCROLLABLE);

	lv_obj_t *k = lv_obj_create(sw);

	lv_obj_set_size(k, 20, 20);
	lv_obj_align(k, LV_ALIGN_LEFT_MID, 2, 0);
	lv_obj_set_style_radius(k, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(k, HPI_M3_ON_SURFACE_MUTED, 0);
	lv_obj_set_style_bg_opa(k, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(k, 0, 0);
	lv_obj_clear_flag(k, LV_OBJ_FLAG_SCROLLABLE);

	*knob_out = k;
	return sw;
}

static void hl_toggle_set(lv_obj_t *sw, lv_obj_t *knob, bool on)
{
	lv_obj_set_style_bg_color(sw, on ? HPI_M3_PRIMARY : HPI_M3_OUTLINE, 0);
	lv_obj_align(knob, on ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, on ? -2 : 2, 0);
	lv_obj_set_style_bg_color(knob, on ? HPI_M3_SURFACE : HPI_M3_ON_SURFACE_MUTED, 0);
}

/* Dimmed and untouchable when the slot cannot be switched or is mid-change. */
static void hl_toggle_set_enabled(lv_obj_t *sw, bool enabled)
{
	if (enabled) {
		lv_obj_add_flag(sw, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_style_opa(sw, LV_OPA_COVER, 0);
	} else {
		lv_obj_clear_flag(sw, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_style_opa(sw, HPI_M3_DISABLED_OPA, 0);
	}
}

/* A label + trailing widget row, for "Slot power  [toggle]". */
static lv_obj_t *hl_row(lv_obj_t *parent, const char *text)
{
	lv_obj_t *r = lv_obj_create(parent);

	lv_obj_set_width(r, lv_pct(100));
	lv_obj_set_height(r, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(r, 0, 0);
	lv_obj_set_style_pad_all(r, 0, 0);
	lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);

	lv_obj_t *l = lv_label_create(r);

	lv_label_set_text(l, text);
	lv_obj_set_style_text_font(l, HPI_M3_FONT_BODY, 0);
	lv_obj_set_style_text_color(l, HPI_M3_ON_SURFACE_VARIANT, 0);

	lv_obj_t *sp = lv_obj_create(r);

	lv_obj_set_style_bg_opa(sp, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(sp, 0, 0);
	lv_obj_set_style_pad_all(sp, 0, 0);
	lv_obj_set_height(sp, 1);
	lv_obj_set_flex_grow(sp, 1);
	lv_obj_clear_flag(sp, LV_OBJ_FLAG_SCROLLABLE);
	return r;
}

/* Ask for the opposite of the current state and let the refresh render what
 * actually happened. hl_request_slot_power() only queues the change, which is
 * what makes it safe from the LVGL thread. */
static void slot_toggle_cb(lv_event_t *e)
{
	hl_slot_t slot = (hl_slot_t)(uintptr_t)lv_event_get_user_data(e);
	struct hl_slot_status st;

	if (hl_get_slot_status(slot, &st) != 0 || !st.detectable || st.busy) {
		return;
	}
	(void)hl_request_slot_power(slot, !st.powered);
	hpi_scr_healthylink_refresh();
}

static void slot_card_create(lv_obj_t *body, hl_slot_t slot)
{
	struct hl_slot_widgets *w = &s_hl.slot[slot];
	lv_obj_t *hdr;
	lv_obj_t *c = hl_card(body, HPI_SYM_SD, HPI_M3_PRIMARY,
			      slot == HL_SLOT_A ? "SLOT A" : "SLOT B", &hdr);

	w->chip = hpi_ui_chip_create(hdr, "EMPTY", HPI_M3_ON_SURFACE_VARIANT);
	w->name = hl_detail_label(c, "No module detected", HPI_M3_ON_SURFACE_MUTED);
	w->id = hl_detail_label(c, "", HPI_M3_ON_SURFACE_FAINT);

	lv_obj_t *prow = hl_row(c, "Slot power");

	w->sw = hl_toggle(prow, &w->knob);
	lv_obj_add_event_cb(w->sw, slot_toggle_cb, LV_EVENT_CLICKED,
			    (void *)(uintptr_t)slot);
}

lv_obj_t *hpi_scr_healthylink_create(lv_obj_t *parent)
{
	lv_obj_t *root = lv_obj_create(parent);

	lv_obj_set_size(root, lv_pct(100), lv_pct(100));
	hpi_m3_apply_card(root, HPI_M3_SURFACE, 0);
	lv_obj_set_style_border_width(root, 0, 0);
	lv_obj_set_style_pad_all(root, 0, 0);
	lv_obj_set_style_pad_row(root, 0, 0);
	lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);

	hpi_ui_subbar_create(root, "HealthyLink", HPI_UI_SCREEN_MORE);

	lv_obj_t *body = lv_obj_create(root);

	lv_obj_set_width(body, lv_pct(100));
	lv_obj_set_flex_grow(body, 1);
	lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(body, 0, 0);
	lv_obj_set_style_pad_all(body, HPI_M3_SPACE_3, 0);
	lv_obj_set_style_pad_row(body, HPI_M3_SPACE_3, 0);
	lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
	/* Never clip the last card, which would make it unreachable. */
	hpi_m3_apply_scroll_v(body);

	/* ---- Cards 1 + 2: the slots ---- */
	slot_card_create(body, HL_SLOT_A);
	slot_card_create(body, HL_SLOT_B);

	/* ---- Card 3: the Compute module link ---- */
	lv_obj_t *hhdr;
	lv_obj_t *hc = hl_card(body, HPI_SYM_INFO, HPI_M3_ON_SURFACE_VARIANT,
			       "HLINK", &hhdr);

	s_hl.hl_chip = hpi_ui_chip_create(hhdr, "PENDING", HPI_M3_ON_SURFACE_VARIANT);
	s_hl.hl_detail = hl_detail_label(hc, "Handshake has not run yet.",
					 HPI_M3_ON_SURFACE_MUTED);
	s_hl.hl_engine = hl_detail_label(hc, "", HPI_M3_ON_SURFACE_MUTED);
	s_hl.hl_counters = hl_detail_label(hc, "", HPI_M3_ON_SURFACE_MUTED);
	s_hl.hl_beat = hl_detail_label(hc, "Last beat: \xE2\x80\x94",
				       HPI_M3_ON_SURFACE_MUTED);

	/* ---- Footer ---- */
	hl_caption(body, "Modules are detected at boot, and again when a slot is "
			 "switched on here. The switch shows the load-switch "
			 "enable, not a measurement of the rail; a slot stays "
			 "powered only while its module is running.");

	s_hl.built = true;
	hpi_scr_healthylink_refresh();
	return root;
}

/* Name a slot state, and pick the colour that carries it. */
static const char *slot_chip(const struct hl_slot_status *st, lv_color_t *col)
{
	if (!st->detectable) {
		*col = HPI_M3_ON_SURFACE_FAINT;
		return "NOT DETECTABLE";
	}
	if (st->busy) {
		*col = HPI_M3_WARNING;
		return "SWITCHING";
	}
	switch (st->state) {
	case HL_SLOT_ACTIVE:
		*col = HPI_M3_SUCCESS;
		return "ACTIVE";
	case HL_SLOT_UNSUPPORTED:
		*col = HPI_M3_WARNING;
		return "UNSUPPORTED";
	case HL_SLOT_ERROR:
		*col = HPI_M3_ERROR;
		return "ERROR";
	case HL_SLOT_QUARANTINED:
		*col = HPI_M3_ERROR;
		return "QUARANTINED";
	case HL_SLOT_OFF:
		*col = HPI_M3_ON_SURFACE_VARIANT;
		return "OFF";
	case HL_SLOT_EMPTY:
	default:
		*col = HPI_M3_ON_SURFACE_VARIANT;
		return "EMPTY";
	}
}

static void slot_refresh(hl_slot_t slot, const struct hl_slot_status *st)
{
	struct hl_slot_widgets *w = &s_hl.slot[slot];
	lv_color_t col;
	char buf[96];

	hpi_ui_chip_set(w->chip, slot_chip(st, &col), col);

	if (!st->detectable) {
		/* A default, not a measurement -- say why, and show nothing that
		 * reads as a scan result. */
		lv_label_set_text(w->name, "No ID EEPROM reaches this slot on this "
					   "hardware, so the firmware cannot tell "
					   "whether a module is fitted.");
		lv_label_set_text(w->id, "");
		hl_toggle_set(w->sw, w->knob, false);
		hl_toggle_set_enabled(w->sw, false);
		return;
	}

	if (st->name[0]) {
		lv_label_set_text(w->name, st->name);
	} else if (st->module_id != 0) {
		lv_label_set_text(w->name, "Module with no driver in this build");
	} else {
		lv_label_set_text(w->name, "No module detected");
	}
	if (st->module_id != 0) {
		snprintf(buf, sizeof(buf), "ID 0x%04X \xC2\xB7 caps 0x%08X",
			 st->module_id, st->caps);
		lv_label_set_text(w->id, buf);
	} else {
		lv_label_set_text(w->id, "");
	}

	hl_toggle_set(w->sw, w->knob, st->powered);
	hl_toggle_set_enabled(w->sw, !st->busy);
}

static const char *engine_name(uint8_t st)
{
	switch (st) {
	case HLINK_ENGINE_IDLE:
		return "idle";
	case HLINK_ENGINE_BUSY:
		return "busy";
	case HLINK_ENGINE_DOWN:
	default:
		return "down";
	}
}

/*
 * Called ~2 Hz from ui_module on the LVGL thread. All three accessors are a
 * mutex plus a struct copy with no I/O behind them, which is what makes them
 * safe here; none triggers a scan or a transfer.
 */
void hpi_scr_healthylink_refresh(void)
{
	struct hl_slot_status st[HL_NUM_SLOTS];
	struct hpi_npu_link_info li;
	char buf[128];
	int compute_slot = -1;

	if (!s_hl.built) {
		return;   /* not built yet */
	}

	/* ---- the slots ---- */
	for (int i = 0; i < HL_NUM_SLOTS; i++) {
		if (hl_get_slot_status((hl_slot_t)i, &st[i]) != 0) {
			memset(&st[i], 0, sizeof(st[i]));
		}
		slot_refresh((hl_slot_t)i, &st[i]);
		if (st[i].detectable && st[i].state == HL_SLOT_ACTIVE &&
		    st[i].module_id == HEALTHYLINK_MODULE_ID_COMPUTE) {
			compute_slot = i;
		}
	}

	/* ---- the module link ----
	 *
	 * Gate on a running Compute module first. "No module fitted" and "the
	 * handshake failed" are completely different things, and the link state
	 * alone cannot tell them apart: with no module the handshake is never
	 * scheduled, so it sits at NOT_RUN, which would otherwise read as a
	 * pending check that is never going to complete.
	 */
	int rc = hpi_npu_link_get(&li);
	char where = compute_slot >= 0 ? (char)('A' + compute_slot) : '?';

	if (rc == -ENOTSUP) {
		hpi_ui_chip_set(s_hl.hl_chip, "DISABLED", HPI_M3_ON_SURFACE_VARIANT);
		lv_label_set_text(s_hl.hl_detail,
				  "Module link not enabled in this build.");
	} else if (compute_slot < 0) {
		hpi_ui_chip_set(s_hl.hl_chip, "IDLE", HPI_M3_ON_SURFACE_FAINT);
		lv_label_set_text(s_hl.hl_detail, "No Compute module running in "
						  "either slot.");
	} else {
		switch (li.link_state) {
		case HPI_NPU_LINK_IN_FLIGHT:
			hpi_ui_chip_set(s_hl.hl_chip, "CHECKING", HPI_M3_WARNING);
			snprintf(buf, sizeof(buf),
				 "Slot %c: module booting, then handshake...", where);
			lv_label_set_text(s_hl.hl_detail, buf);
			break;
		case HPI_NPU_LINK_NOT_RUN:
			hpi_ui_chip_set(s_hl.hl_chip, "PENDING",
					HPI_M3_ON_SURFACE_VARIANT);
			snprintf(buf, sizeof(buf), "Slot %c: handshake has not run yet.",
				 where);
			lv_label_set_text(s_hl.hl_detail, buf);
			break;
		case HPI_NPU_LINK_NO_SIGNATURE:
			hpi_ui_chip_set(s_hl.hl_chip, "NO LINK", HPI_M3_ERROR);
			snprintf(buf, sizeof(buf), "Slot %c: no HLNK signature on SPI4.",
				 where);
			lv_label_set_text(s_hl.hl_detail, buf);
			break;
		case HPI_NPU_LINK_PROTO_MISMATCH:
			hpi_ui_chip_set(s_hl.hl_chip, "OLD FIRMWARE", HPI_M3_WARNING);
			snprintf(buf, sizeof(buf),
				 "Slot %c: module speaks HLink v%u; this device speaks "
				 "v%u. Update the module firmware.",
				 where, li.proto_major, HLINK_PROTO_MAJOR);
			lv_label_set_text(s_hl.hl_detail, buf);
			break;
		case HPI_NPU_LINK_NO_REPLY:
			hpi_ui_chip_set(s_hl.hl_chip, "NO REPLY", HPI_M3_ERROR);
			snprintf(buf, sizeof(buf),
				 "Slot %c: module is alive but did not answer.", where);
			lv_label_set_text(s_hl.hl_detail, buf);
			break;
		case HPI_NPU_LINK_UP:
			hpi_ui_chip_set(s_hl.hl_chip, "UP", HPI_M3_SUCCESS);
			snprintf(buf, sizeof(buf),
				 "Slot %c \xC2\xB7 HLink %u.%u \xC2\xB7 fw %u.%u.%u "
				 "\xC2\xB7 frame %u B",
				 where, li.proto_major, li.proto_minor, li.fw_major,
				 li.fw_minor, li.fw_patch, li.frame_size);
			lv_label_set_text(s_hl.hl_detail, buf);
			break;
		default:
			hpi_ui_chip_set(s_hl.hl_chip, "DISABLED",
					HPI_M3_ON_SURFACE_VARIANT);
			lv_label_set_text(s_hl.hl_detail,
					  "Module link not enabled in this build.");
			break;
		}
	}

	/*
	 * The engine and counter lines exist ONLY when a STATUS reply was
	 * actually decoded, from a module that is running now. Every field they
	 * show is zero by default, and zero is a plausible reading for all of
	 * them -- "engine down, 0 models, no errors" is a sentence this screen
	 * must never say on the strength of a reply that never arrived.
	 */
	if (rc == 0 && compute_slot >= 0 && li.status_valid) {
		snprintf(buf, sizeof(buf), "engine %s \xC2\xB7 models %u \xC2\xB7 %s",
			 engine_name(li.engine_state), li.n_models,
			 li.active_name[0] ? li.active_name : "no model loaded");
		lv_label_set_text(s_hl.hl_engine, buf);

		/* err_overrun mixes units by design on the module side, so it is
		 * shown as a flag-like count and never labelled "samples lost". */
		snprintf(buf, sizeof(buf),
			 "crc %u \xC2\xB7 disp %u \xC2\xB7 ovr %u \xC2\xB7 up %u s",
			 li.err_crc, li.err_dispatch, li.err_overrun,
			 li.uptime_ms / 1000U);
		lv_label_set_text(s_hl.hl_counters, buf);
		lv_obj_set_style_text_color(s_hl.hl_counters,
					    (li.err_crc || li.err_dispatch ||
					     li.err_overrun)
						    ? HPI_M3_WARNING
						    : HPI_M3_ON_SURFACE_MUTED,
					    0);
	} else {
		lv_label_set_text(s_hl.hl_engine, "");
		lv_label_set_text(s_hl.hl_counters, "");
	}

	if (compute_slot < 0 && s_hl.hl_beat != NULL) {
		lv_label_set_text(s_hl.hl_beat, "Last beat: \xE2\x80\x94");
		lv_obj_set_style_text_color(s_hl.hl_beat, HPI_M3_ON_SURFACE_MUTED, 0);
	}
}

void hpi_scr_healthylink_set_infer(const struct hp6_infer_sample *s)
{
	static const char letters[] = "NSVFQ";

	if (s_hl.hl_beat == NULL || s == NULL) {
		return;
	}
	if (s->flags & HP6_INF_STUB) {
		lv_label_set_text(s_hl.hl_beat, "Last beat: \xE2\x80\x94");
		lv_obj_set_style_text_color(s_hl.hl_beat, HPI_M3_ON_SURFACE_MUTED, 0);
		return;
	}

	char buf[24];
	char letter = (s->class_id < 5) ? letters[s->class_id] : '?';

	snprintf(buf, sizeof(buf), "Last beat: %c", letter);
	lv_label_set_text(s_hl.hl_beat, buf);
	lv_obj_set_style_text_color(s_hl.hl_beat,
				    (s->flags & HP6_INF_LOW_CONF) ? HPI_M3_WARNING
								 : HPI_M3_ON_SURFACE,
				    0);
}
