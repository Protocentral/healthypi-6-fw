/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * HealthyLink — expansion module status.
 *
 * Everything here is a snapshot of something the firmware measured once, and
 * the screen's job is to be exact about which "once" that was:
 *
 *   - Slot detection runs at boot, in hl_framework_init(), off the module
 *     EEPROM. There is no rescan, so a module fitted after boot is invisible
 *     until the next one.
 *   - The HLink handshake runs once when a Compute module starts, on its own
 *     work queue.
 *
 * So the 2 Hz refresh below is repainting a fixed picture, not polling a live
 * one, and the footer says so. That matters more here than anywhere else in the
 * UI, because a status screen is read as current by definition.
 *
 * THREE THINGS THIS SCREEN MUST NOT DO, each of which was a live temptation:
 *   1. Render slot B as "Empty". Slot B has no EEPROM wired on this hardware,
 *      so hl_get_slot_status() returns a DEFAULT, not a measurement. Reporting
 *      "empty" would state that a scan happened and found nothing.
 *   2. Present the power toggle as a control. hl_slot_power() is a stub.
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

#include <errno.h>
#include <stdio.h>

static struct {
	/* Slot A */
	lv_obj_t *a_chip;
	lv_obj_t *a_name;
	lv_obj_t *a_id;
	lv_obj_t *a_sw;
	/* HLink */
	lv_obj_t *hl_chip;
	lv_obj_t *hl_detail;
	lv_obj_t *hl_engine;
	lv_obj_t *hl_counters;
} s_hl;

/* ---- local builders (mirroring scr_link.c's card, minus the live toggle) ---- */

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
 * A 44x24 toggle drawn in a fixed position and then made inert.
 *
 * Deliberately NOT the live toggle from scr_link.c, and deliberately carrying
 * no event callback at all: hpi_m3_apply_inert() clears CLICKABLE, so a
 * callback here would be dormant code that springs to life the day somebody
 * deletes the inert call. When the EN_MOD_A LDO GPIO is wired
 * (healthylink_service.c hl_slot_power()), give this a callback and drop the
 * hpi_m3_apply_inert() line together.
 *
 * Not hpi_m3_apply_touch() either -- its 64 px minimums would deform the track.
 */
static lv_obj_t *hl_inert_toggle(lv_obj_t *parent, bool on)
{
	lv_obj_t *sw = lv_obj_create(parent);

	lv_obj_set_size(sw, 44, 24);
	lv_obj_set_style_radius(sw, 12, 0);
	lv_obj_set_style_bg_color(sw, on ? HPI_M3_PRIMARY : HPI_M3_OUTLINE, 0);
	lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(sw, 0, 0);
	lv_obj_clear_flag(sw, LV_OBJ_FLAG_SCROLLABLE);

	lv_obj_t *k = lv_obj_create(sw);

	lv_obj_set_size(k, 20, 20);
	lv_obj_align(k, on ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, on ? -2 : 2, 0);
	lv_obj_set_style_radius(k, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(k, HPI_M3_ON_SURFACE_MUTED, 0);
	lv_obj_set_style_bg_opa(k, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(k, 0, 0);
	lv_obj_clear_flag(k, LV_OBJ_FLAG_SCROLLABLE);

	hpi_m3_apply_inert(sw);
	return sw;
}

/* Redraw the inert toggle from the framework's flag. The knob is the track's
 * only child. */
static void hl_toggle_set(lv_obj_t *sw, bool on)
{
	lv_obj_t *k = lv_obj_get_child(sw, 0);

	lv_obj_set_style_bg_color(sw, on ? HPI_M3_PRIMARY : HPI_M3_OUTLINE, 0);
	if (k != NULL) {
		lv_obj_align(k, on ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID,
			     on ? -2 : 2, 0);
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
	/* The HLink card grows a line when counters appear; never clip the last
	 * card, which would make it unreachable. */
	hpi_m3_apply_scroll_v(body);

	/* ---- Card 1: slot A ---- */
	lv_obj_t *ahdr;
	lv_obj_t *ac = hl_card(body, HPI_SYM_SD, HPI_M3_PRIMARY, "SLOT A", &ahdr);

	s_hl.a_chip = hpi_ui_chip_create(ahdr, "EMPTY", HPI_M3_ON_SURFACE_VARIANT);
	s_hl.a_name = hl_detail_label(ac, "No module detected", HPI_M3_ON_SURFACE_MUTED);
	s_hl.a_id = hl_detail_label(ac, "", HPI_M3_ON_SURFACE_FAINT);

	lv_obj_t *prow = hl_row(ac, "Slot power");

	s_hl.a_sw = hl_inert_toggle(prow, false);
	hl_caption(ac, "Not wired \xE2\x80\x94 the slot EN_MOD_A LDO enable is a "
		       "bring-up TODO, so this reports the framework's intent, "
		       "not the rail.");

	/* ---- Card 2: the module link ---- */
	lv_obj_t *hhdr;
	lv_obj_t *hc = hl_card(body, HPI_SYM_INFO, HPI_M3_ON_SURFACE_VARIANT,
			       "HLINK", &hhdr);

	s_hl.hl_chip = hpi_ui_chip_create(hhdr, "PENDING", HPI_M3_ON_SURFACE_VARIANT);
	s_hl.hl_detail = hl_detail_label(hc, "Handshake has not run yet.",
					 HPI_M3_ON_SURFACE_MUTED);
	s_hl.hl_engine = hl_detail_label(hc, "", HPI_M3_ON_SURFACE_MUTED);
	s_hl.hl_counters = hl_detail_label(hc, "", HPI_M3_ON_SURFACE_MUTED);

	/* ---- Card 3: slot B ----
	 *
	 * Built once and never refreshed, because there is nothing to refresh:
	 * no EEPROM reaches this slot, so the firmware has no way to know. It
	 * deliberately does NOT call hl_get_slot_status(HL_SLOT_B) -- that
	 * returns a zeroed default, and rendering a default as EMPTY would claim
	 * a scan that never happened.
	 */
	lv_obj_t *bhdr;
	lv_obj_t *bc = hl_card(body, HPI_SYM_SD, HPI_M3_ON_SURFACE_FAINT,
			       "SLOT B", &bhdr);

	hpi_ui_chip_create(bhdr, "NOT DETECTABLE", HPI_M3_ON_SURFACE_FAINT);
	hl_detail_label(bc, "Slot B has no EEPROM on this hardware, so the "
			    "firmware cannot tell whether a module is fitted.",
			HPI_M3_ON_SURFACE_FAINT);

	/* ---- Footer ---- */
	hl_caption(body, "Read-only. Modules are detected once at boot and the "
			 "link is checked once after that \xE2\x80\x94 this page "
			 "does not re-scan.");

	hpi_scr_healthylink_refresh();
	return root;
}

/* Name a slot state, and pick the colour that carries it. */
static const char *slot_chip(uint8_t state, lv_color_t *col)
{
	switch (state) {
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
	case HL_SLOT_EMPTY:
	default:
		*col = HPI_M3_ON_SURFACE_VARIANT;
		return "EMPTY";
	}
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
 * Called ~2 Hz from ui_module on the LVGL thread. Both accessors are a mutex
 * plus a struct copy with no I/O behind them, which is what makes them safe
 * here; neither triggers a scan or a transfer.
 */
void hpi_scr_healthylink_refresh(void)
{
	struct hl_slot_status a;
	struct hpi_npu_link_info li;
	lv_color_t col;
	char buf[96];

	if (s_hl.a_chip == NULL) {
		return;   /* not built yet */
	}

	/* ---- slot A ---- */
	if (hl_get_slot_status(HL_SLOT_A, &a) != 0) {
		memset(&a, 0, sizeof(a));
	}
	hpi_ui_chip_set(s_hl.a_chip, slot_chip(a.state, &col), col);
	lv_label_set_text(s_hl.a_name,
			  a.name[0] ? a.name : "No module detected");
	if (a.module_id != 0) {
		snprintf(buf, sizeof(buf), "ID 0x%04X \xC2\xB7 caps 0x%08X",
			 a.module_id, a.caps);
		lv_label_set_text(s_hl.a_id, buf);
	} else {
		lv_label_set_text(s_hl.a_id, "");
	}
	/* The framework's intent flag, not a measured rail -- the caption under
	 * the toggle says which. */
	hl_toggle_set(s_hl.a_sw, a.powered);

	/* ---- the module link ----
	 *
	 * Gate on the SLOT first. "No module fitted" and "the handshake failed"
	 * are completely different things, and the link state alone cannot tell
	 * them apart: with no module the handshake is never scheduled, so it
	 * sits at NOT_RUN, which would otherwise read as a pending check that
	 * is never going to complete.
	 */
	int rc = hpi_npu_link_get(&li);

	if (rc == -ENOTSUP) {
		hpi_ui_chip_set(s_hl.hl_chip, "DISABLED", HPI_M3_ON_SURFACE_VARIANT);
		lv_label_set_text(s_hl.hl_detail,
				  "Module link not enabled in this build.");
	} else if (a.state != HL_SLOT_ACTIVE) {
		hpi_ui_chip_set(s_hl.hl_chip, "IDLE", HPI_M3_ON_SURFACE_FAINT);
		lv_label_set_text(s_hl.hl_detail, "No active module in slot A.");
	} else {
		switch (li.link_state) {
		case HPI_NPU_LINK_IN_FLIGHT:
			hpi_ui_chip_set(s_hl.hl_chip, "CHECKING", HPI_M3_WARNING);
			lv_label_set_text(s_hl.hl_detail, "Handshake in progress...");
			break;
		case HPI_NPU_LINK_NOT_RUN:
			hpi_ui_chip_set(s_hl.hl_chip, "PENDING",
					HPI_M3_ON_SURFACE_VARIANT);
			lv_label_set_text(s_hl.hl_detail, "Handshake has not run yet.");
			break;
		case HPI_NPU_LINK_NO_SIGNATURE:
			hpi_ui_chip_set(s_hl.hl_chip, "NO LINK", HPI_M3_ERROR);
			lv_label_set_text(s_hl.hl_detail,
					  "No HLNK signature on SPI4.");
			break;
		case HPI_NPU_LINK_PROTO_MISMATCH:
			hpi_ui_chip_set(s_hl.hl_chip, "OLD FIRMWARE", HPI_M3_WARNING);
			snprintf(buf, sizeof(buf),
				 "Module speaks HLink v%u; this device speaks v%u. "
				 "Update the module firmware.",
				 li.proto_major, HLINK_PROTO_MAJOR);
			lv_label_set_text(s_hl.hl_detail, buf);
			break;
		case HPI_NPU_LINK_NO_REPLY:
			hpi_ui_chip_set(s_hl.hl_chip, "NO REPLY", HPI_M3_ERROR);
			lv_label_set_text(s_hl.hl_detail,
					  "Module is alive but did not answer.");
			break;
		case HPI_NPU_LINK_UP:
			hpi_ui_chip_set(s_hl.hl_chip, "UP", HPI_M3_SUCCESS);
			snprintf(buf, sizeof(buf),
				 "HLink %u.%u \xC2\xB7 fw %u.%u.%u \xC2\xB7 frame %u B",
				 li.proto_major, li.proto_minor, li.fw_major,
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
	 * actually decoded. Every field they show is zero by default, and zero
	 * is a plausible reading for all of them -- "engine down, 0 models, no
	 * errors" is a sentence this screen must never say on the strength of a
	 * reply that never arrived.
	 */
	if (rc == 0 && li.status_valid) {
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
}
