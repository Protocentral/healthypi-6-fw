/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Material 3 design tokens for the HealthyPi 6 LVGL UI — the single source of
 * truth for color roles, the type scale, shape (corner radii) and the 4 dp
 * spacing grid. Screens and components reference ONLY these tokens.
 *
 * HealthyPi 6 palette (dark): brand blue #6FB3CC for nav/selection/primary
 * actions; Signal Amber #FBBF24 for ECG/HR and the single big CTA per screen.
 * Surfaces step #14171A -> #1C2126 -> #232A31 with 1 px outlines (no pure
 * black); all accents target >=4.5:1 contrast on these surfaces.
 *
 * No `static lv_style_t` anywhere in the UI — apply tokens with direct
 * lv_obj_set_style_* setters (see helpers below).
 */
#ifndef HPI_UI_M3_THEME_H
#define HPI_UI_M3_THEME_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- M3 color roles (HealthyPi 6 palette, dark) ---- */
#define HPI_M3_PRIMARY            lv_color_hex(0x6FB3CC)  /* brand blue: nav, selection, primary actions */
#define HPI_M3_ON_PRIMARY         lv_color_hex(0x0E1114)  /* dark text/icon on brand blue */
#define HPI_M3_PRIMARY_CONTAINER  lv_color_hex(0x1E3A45)  /* brand-tinted container fill */
#define HPI_M3_PRIMARY_LIGHT      lv_color_hex(0xA8D4E4)  /* brand blue hover/emphasis */
#define HPI_M3_SURFACE            lv_color_hex(0x14171A)  /* base surface */
#define HPI_M3_SURFACE_CONTAINER  lv_color_hex(0x1C2126)  /* card / container surface */
#define HPI_M3_SURFACE_HIGH       lv_color_hex(0x232A31)  /* raised surface */
#define HPI_M3_SURFACE_DIM        lv_color_hex(0x0E1114)  /* splash / dimmed backdrop */
#define HPI_M3_ON_SURFACE         lv_color_hex(0xECEFF1)  /* primary text (bright) */
#define HPI_M3_ON_SURFACE_VARIANT lv_color_hex(0x9AA6AD)  /* secondary text / labels */
#define HPI_M3_ON_SURFACE_MUTED   lv_color_hex(0x6B767E)  /* units, captions */
#define HPI_M3_ON_SURFACE_FAINT   lv_color_hex(0x4A555C)  /* disabled / pending */
#define HPI_M3_OUTLINE            lv_color_hex(0x2C343B)  /* 1 px card outline */
#define HPI_M3_DIVIDER            lv_color_hex(0x1B2126)  /* hairline row divider */
#define HPI_M3_ERROR              lv_color_hex(0xF87171)
#define HPI_M3_WARNING            lv_color_hex(0xFB923C)
#define HPI_M3_SUCCESS            lv_color_hex(0x4ADE80)

/* ---- brand CTA (the single big call-to-action per screen = Signal Amber) ---- */
#define HPI_M3_CTA                lv_color_hex(0xFBBF24)
#define HPI_M3_ON_CTA             lv_color_hex(0x1F1300)  /* dark text/icon on amber */

/* ---- signal colors (HealthyPi 6 palette) ---- */
#define HPI_M3_SIG_ECG            lv_color_hex(0xFBBF24)  /* Signal Amber (ECG waveform + HR) */
#define HPI_M3_SIG_HR             lv_color_hex(0xFBBF24)  /* heart rate (= ECG amber) */
#define HPI_M3_SIG_PPG            lv_color_hex(0x6FB3CC)  /* PPG / SpO2 blue */
#define HPI_M3_SIG_SPO2           lv_color_hex(0x6FB3CC)  /* SpO2 (= PPG blue) */
#define HPI_M3_SIG_RESP           lv_color_hex(0x4CC38A)  /* respiration green */
#define HPI_M3_SIG_TEMP           lv_color_hex(0xFB923C)  /* temperature orange */
#define HPI_M3_SIG_HRV            lv_color_hex(0x8B84F0)  /* HRV violet */

/* ---- M3 type scale ----------------------------------------------------------
 * The design fonts (LVGL bitmap fonts generated from the design TTFs by
 * ui/fonts/convert_fonts.sh): Rubik (all numerals) · Manrope (labels/caps/
 * units) · Saira (screen titles) · JetBrains Mono (clock/filenames/versions).
 * Screens reference these role tokens only, so a size/face change is a
 * one-file edit here + a re-run of convert_fonts.sh. */
#include "ui/fonts/hpi_fonts.h"

#define HPI_M3_FONT_DISPLAY       (&saira_700_28)
#define HPI_M3_FONT_HEADLINE      (&saira_700_28)
#define HPI_M3_FONT_TITLE         (&saira_600_18)
#define HPI_M3_FONT_BODY          (&manrope_700_16)
#define HPI_M3_FONT_LABEL         (&manrope_700_16)
#define HPI_M3_FONT_LABEL_SM      (&manrope_700_12)

/* Intent-named roles used by the screens. */
#define HPI_M3_FONT_NUMERAL_XL    (&rubik_500_148)   /* focal HR / hero numerals */
#define HPI_M3_FONT_NUMERAL       (&rubik_500_28)    /* vital-chip numerals */
#define HPI_M3_FONT_CAPS          (&manrope_700_16)  /* caps labels / units */
#define HPI_M3_FONT_CAPS_SM       (&manrope_700_12)
#define HPI_M3_FONT_MONO          (&jetbrains_mono_14)  /* clock / filenames / versions */
#define HPI_M3_FONT_MONO_SM       (&jetbrains_mono_14)

/* LVGL's built-in glyphs (LV_SYMBOL_*) are baked into the Montserrat faces, not
 * the design faces — use this for any label that renders an LV_SYMBOL_*. */
#define HPI_M3_FONT_SYMBOL        (&lv_font_montserrat_16)

/* Material Symbols icons (HPI_SYM_* string literals from hpi_symbols.h). */
#define HPI_M3_FONT_ICON          (&matsym_20)   /* chips / status / caption */
#define HPI_M3_FONT_ICON_LG       (&matsym_24)   /* nav */

/* ---- shape (corner radii, px) ---- */
#define HPI_M3_RADIUS_XS  4
#define HPI_M3_RADIUS_SM  8
#define HPI_M3_RADIUS_MD  12
#define HPI_M3_RADIUS_LG  16
#define HPI_M3_RADIUS_XL  28
#define HPI_M3_RADIUS_PILL 22   /* fully-rounded 44 px pill button / chip */

/* ---- spacing (4 dp grid) + min touch target ---- */
#define HPI_M3_SPACE_1  4
#define HPI_M3_SPACE_2  8
#define HPI_M3_SPACE_3  12
#define HPI_M3_SPACE_4  16
#define HPI_M3_SPACE_6  24

/* Touch floor, device px on the 480x800 panel: every interactive object must
 * be at least this in both axes. Apply with hpi_m3_apply_touch() — never size
 * a control by content. Screens that stack controls must scroll to afford it. */
#define HPI_M3_TOUCH_MIN 64

/* Minimum clear space BETWEEN two adjacent targets, so a near-miss lands on
 * nothing rather than the neighbour. Use as pad_row/pad_column wherever
 * targets are stacked. */
#define HPI_M3_TOUCH_GAP 16

/* ---- layout constants ---- */
#define HPI_M3_NAVBAR_H   80   /* M3 bottom navigation bar height (5 slots) */
#define HPI_M3_APPBAR_H   40   /* top status bar height */

/* ---- helpers: apply tokens with direct setters (no static styles) ---- */

/* Paint the root screen with the M3 surface + default on-surface text. */
static inline void hpi_m3_apply_screen(lv_obj_t *scr)
{
	lv_obj_set_style_bg_color(scr, HPI_M3_SURFACE, 0);
	lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
	lv_obj_set_style_text_color(scr, HPI_M3_ON_SURFACE, 0);
	lv_obj_set_style_text_font(scr, HPI_M3_FONT_BODY, 0);
}

/* Give an interactive object the touch floor. Sets a MINIMUM, not a size, so
 * it composes with LV_SIZE_CONTENT and flex. Call it on every object that
 * takes a tap -- buttons, list rows, segments, stepper keys. */
static inline void hpi_m3_apply_touch(lv_obj_t *o)
{
	lv_obj_set_style_min_height(o, HPI_M3_TOUCH_MIN, 0);
	lv_obj_set_style_min_width(o, HPI_M3_TOUCH_MIN, 0);
}

/* Mark a control as design chrome that is not wired to anything yet: dim to
 * the M3 disabled opacity and clear CLICKABLE so it does not take the press.
 * Delete the call when the control gets its backing -- an inert control that
 * looks live is the failure this exists to prevent. */
#define HPI_M3_DISABLED_OPA  LV_OPA_40

static inline void hpi_m3_apply_inert(lv_obj_t *o)
{
	lv_obj_set_style_opa(o, HPI_M3_DISABLED_OPA, 0);
	lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
}

/* Make a column of controls scroll vertically -- required on any screen whose
 * stack can exceed the 800 px panel, or the overflow is unreachable. Vertical
 * only: horizontal drag must stay available to the swipe-between-tabs gesture. */
static inline void hpi_m3_apply_scroll_v(lv_obj_t *o)
{
	lv_obj_add_flag(o, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_scroll_dir(o, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_AUTO);
}

/* Style an object as an M3 tonal surface card (radius md, 1 px outline). */
static inline void hpi_m3_apply_card(lv_obj_t *o, lv_color_t fill, uint8_t radius)
{
	lv_obj_set_style_bg_color(o, fill, 0);
	lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(o, radius, 0);
	lv_obj_set_style_border_width(o, 1, 0);
	lv_obj_set_style_border_color(o, HPI_M3_OUTLINE, 0);
	lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_all(o, HPI_M3_SPACE_3, 0);
	lv_obj_set_style_shadow_width(o, 0, 0);
}

#ifdef __cplusplus
}
#endif

#endif /* HPI_UI_M3_THEME_H */
