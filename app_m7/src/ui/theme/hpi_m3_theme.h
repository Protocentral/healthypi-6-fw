/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Material 3 design tokens for the HealthyPi 6 LVGL UI — the single source of
 * truth for color roles, the type scale, shape (corner radii) and the 4 dp
 * spacing grid. Screens and components reference ONLY these tokens.
 *
 * Two palettes, DARK (navy ink) and LIGHT, chosen once at boot from
 * Settings -> Theme (hpi_config_display_theme()). Screens are built once and
 * never rebuilt, so a change applies at the next restart.
 *
 * ---------------------------------------------------------------------------
 * HOW TO RESTYLE THE UI
 *
 *   Colours ........ theme/hpi_m3_theme.c -- one table per theme, the same
 *                    fields in the same order, so the two read side by side.
 *   Which colour each button kind uses, switches, radius, pressed/disabled
 *                    feedback ...... "Buttons" / "Interaction states" below.
 *   Fonts, spacing, touch floor .... further down this file.
 *
 * Screens never name a hex value; components never name a colour that is not
 * a token. Change a value, rebuild, done.
 * ---------------------------------------------------------------------------
 *
 * The colour tokens (HPI_M3_SURFACE, ...) read the active palette, so they are
 * runtime values: they cannot initialise a file-scope `static const`.
 *
 * No `static lv_style_t` anywhere in the UI — apply tokens with direct
 * lv_obj_set_style_* setters (see helpers below).
 */
#ifndef HPI_UI_M3_THEME_H
#define HPI_UI_M3_THEME_H

#include <lvgl.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==== Colour tokens ========================================================
 * Each is one field of struct hpi_m3_palette (hex RGB), filled per theme in
 * hpi_m3_theme.c. The comments say what the token is FOR; the values live in
 * the .c file. */
struct hpi_m3_palette {
	/* Surfaces */
	uint32_t surface_dim;        /* splash / ambient / power backdrop        */
	uint32_t plot_well;          /* the plot rectangle behind every trace    */
	uint32_t surface;            /* screen background                        */
	uint32_t surface_container;  /* card / list row / nav bar                */
	uint32_t surface_high;       /* raised: secondary buttons, stepper keys  */
	uint32_t outline;            /* 1 px card / button edge                  */
	uint32_t divider;            /* hairline row divider, chart grid         */
	/* Text */
	uint32_t on_surface;         /* primary text                             */
	uint32_t on_surface_variant; /* secondary text / labels                  */
	uint32_t on_surface_muted;   /* units, captions                          */
	uint32_t on_surface_faint;   /* disabled / pending -- decorative only    */
	/* Accents */
	uint32_t primary;            /* brand blue: nav, selection, actions      */
	uint32_t on_primary;         /* text/icon on primary                     */
	uint32_t primary_container;  /* tonal fill: TONAL buttons, active nav    */
	uint32_t on_primary_container; /* text/icon on primary_container        */
	uint32_t error, on_error;
	uint32_t warning, success;
	uint32_t cta, on_cta;        /* the one big call-to-action per screen    */
	/* Signals (waveforms, vitals) */
	uint32_t sig_ecg;            /* ECG trace + heart rate                   */
	uint32_t sig_ppg;            /* PPG trace + SpO2                         */
	uint32_t sig_resp;
	uint32_t sig_temp;
	uint32_t sig_hrv;
	/* Traces: the line colour INSIDE plot_well. Separate from sig_*, which
	 * colour labels and numbers on cards: a black plot well needs bright
	 * traces even on the light theme, whose sig_* are deep for light cards. */
	uint32_t trace_ecg;
	uint32_t trace_ppg;
	uint32_t trace_resp;
	/* Boot splash + self-test, and the two logos. */
	uint32_t splash_bg;          /* splash / self-test page                  */
	uint32_t logo_text;          /* "HealthyPi" wordmark                     */
	uint32_t logo_accent;        /* the 6 / hexagon / ECG mark               */
	uint32_t pc_mark;            /* ProtoCentral round mark                  */
	uint32_t pc_text;            /* "PROTOCENTRAL" wordmark                  */
};

enum hpi_m3_theme_id {
	HPI_M3_THEME_DARK  = 0,   /* == HPI_CONFIG_THEME_DARK  */
	HPI_M3_THEME_LIGHT = 1,   /* == HPI_CONFIG_THEME_LIGHT */
	HPI_M3_THEME_COUNT
};

/* The active palette. Set by hpi_m3_theme_select(), once, before any screen
 * is built; never NULL (defaults to LIGHT). */
extern const struct hpi_m3_palette *hpi_m3_pal;
void hpi_m3_theme_select(enum hpi_m3_theme_id id);
enum hpi_m3_theme_id hpi_m3_theme_active(void);

#define HPI_M3_C(field)           lv_color_hex(hpi_m3_pal->field)

#define HPI_M3_SURFACE_DIM        HPI_M3_C(surface_dim)
#define HPI_M3_PLOT_WELL          HPI_M3_C(plot_well)
#define HPI_M3_SURFACE            HPI_M3_C(surface)
#define HPI_M3_SURFACE_CONTAINER  HPI_M3_C(surface_container)
#define HPI_M3_SURFACE_HIGH       HPI_M3_C(surface_high)
#define HPI_M3_OUTLINE            HPI_M3_C(outline)
#define HPI_M3_DIVIDER            HPI_M3_C(divider)

#define HPI_M3_ON_SURFACE         HPI_M3_C(on_surface)
#define HPI_M3_ON_SURFACE_VARIANT HPI_M3_C(on_surface_variant)
#define HPI_M3_ON_SURFACE_MUTED   HPI_M3_C(on_surface_muted)
#define HPI_M3_ON_SURFACE_FAINT   HPI_M3_C(on_surface_faint)

#define HPI_M3_PRIMARY            HPI_M3_C(primary)
#define HPI_M3_ON_PRIMARY         HPI_M3_C(on_primary)
#define HPI_M3_PRIMARY_CONTAINER  HPI_M3_C(primary_container)
/* Historical name: "light" was right only on the dark theme. It is the text
 * colour that sits on PRIMARY_CONTAINER. */
#define HPI_M3_PRIMARY_LIGHT      HPI_M3_C(on_primary_container)
#define HPI_M3_ERROR              HPI_M3_C(error)
#define HPI_M3_ON_ERROR           HPI_M3_C(on_error)
#define HPI_M3_WARNING            HPI_M3_C(warning)
#define HPI_M3_SUCCESS            HPI_M3_C(success)
#define HPI_M3_CTA                HPI_M3_C(cta)
#define HPI_M3_ON_CTA             HPI_M3_C(on_cta)

#define HPI_M3_SIG_ECG            HPI_M3_C(sig_ecg)
#define HPI_M3_SIG_HR             HPI_M3_C(sig_ecg)   /* heart rate = ECG colour */
#define HPI_M3_SIG_PPG            HPI_M3_C(sig_ppg)
#define HPI_M3_SIG_SPO2           HPI_M3_C(sig_ppg)   /* SpO2 = PPG colour */
#define HPI_M3_SIG_RESP           HPI_M3_C(sig_resp)
#define HPI_M3_SIG_TEMP           HPI_M3_C(sig_temp)
#define HPI_M3_SIG_HRV            HPI_M3_C(sig_hrv)

#define HPI_M3_TRACE_ECG          HPI_M3_C(trace_ecg)
#define HPI_M3_TRACE_PPG          HPI_M3_C(trace_ppg)
#define HPI_M3_TRACE_RESP         HPI_M3_C(trace_resp)

#define HPI_M3_SPLASH_BG          HPI_M3_C(splash_bg)
#define HPI_M3_LOGO_TEXT          HPI_M3_C(logo_text)
#define HPI_M3_LOGO_ACCENT        HPI_M3_C(logo_accent)
#define HPI_M3_PC_MARK            HPI_M3_C(pc_mark)
#define HPI_M3_PC_TEXT            HPI_M3_C(pc_text)

/* ---- M3 type scale ----------------------------------------------------------
 * The design fonts (LVGL bitmap fonts generated from the design TTFs by
 * ui/fonts/convert_fonts.sh): Rubik (all numerals) · Manrope (labels/caps/
 * units) · Saira (screen titles) · JetBrains Mono (clock/filenames/versions).
 * Screens reference these role tokens only, so a size/face change is a
 * one-file edit here + a re-run of convert_fonts.sh. */
#include "ui/fonts/hpi_fonts.h"

#define HPI_M3_FONT_DISPLAY       (&saira_700_30)
#define HPI_M3_FONT_HEADLINE      (&saira_700_30)
#define HPI_M3_FONT_TITLE         (&saira_600_20)
#define HPI_M3_FONT_BODY          (&manrope_700_18)
#define HPI_M3_FONT_LABEL         (&manrope_700_18)
#define HPI_M3_FONT_LABEL_SM      (&manrope_700_14)

/* Intent-named roles used by the screens. */
#define HPI_M3_FONT_NUMERAL_XL    (&rubik_500_148)   /* focal HR / hero numerals */
#define HPI_M3_FONT_NUMERAL       (&rubik_500_32)    /* vital-chip numerals */
#define HPI_M3_FONT_CAPS          (&manrope_700_18)  /* caps labels / units */
#define HPI_M3_FONT_CAPS_SM       (&manrope_700_14)
#define HPI_M3_FONT_MONO          (&jetbrains_mono_16)  /* clock / filenames / versions */
#define HPI_M3_FONT_MONO_SM       (&jetbrains_mono_16)

/* LVGL's built-in glyphs (LV_SYMBOL_*) are baked into the Montserrat faces, not
 * the design faces — use this for any label that renders an LV_SYMBOL_*. */
#define HPI_M3_FONT_SYMBOL        (&lv_font_montserrat_18)

/* Material Symbols icons (HPI_SYM_* string literals from hpi_symbols.h). */
#define HPI_M3_FONT_ICON          (&matsym_20)   /* chips / status / caption */
#define HPI_M3_FONT_ICON_LG       (&matsym_24)   /* nav */

/* ---- shape (corner radii, px) ---- */
#define HPI_M3_RADIUS_XS  4
#define HPI_M3_RADIUS_SM  8
#define HPI_M3_RADIUS_MD  12
#define HPI_M3_RADIUS_LG  16
#define HPI_M3_RADIUS_XL  28
#define HPI_M3_RADIUS_PILL LV_RADIUS_CIRCLE   /* fully rounded at any height (was 22: sized for 44 px) */

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

/* ==== 4. Buttons ===========================================================
 * One table for every button in the UI (hpi_ui_button_create() in
 * components/). Each kind = fill / text+icon / 1 px edge. Every kind has a
 * SOLID fill -- keep it that way: on this LCD a transparent button with a
 * hairline edge reads as floating text, which is what Live/Trends had.
 *
 *   CTA        the one main action on a screen (START RECORDING, PAUSE)
 *   FILLED     a primary action that is not the CTA
 *   TONAL      a quieter primary action (Live PAUSE)
 *   SECONDARY  everything else: BROWSE, CLOSE, MARK, pager, zoom/sweep, tabs
 *   DANGER     destructive: STOP & SAVE, DELETE
 *
 * SELECTED is the look of a SECONDARY button in its selected state (tabs,
 * segmented controls): hpi_ui_button_set_selected().
 */
#define HPI_M3_BTN_CTA_BG         HPI_M3_CTA
#define HPI_M3_BTN_CTA_FG         HPI_M3_ON_CTA
#define HPI_M3_BTN_CTA_EDGE       HPI_M3_CTA

#define HPI_M3_BTN_FILLED_BG      HPI_M3_PRIMARY
#define HPI_M3_BTN_FILLED_FG      HPI_M3_ON_PRIMARY
#define HPI_M3_BTN_FILLED_EDGE    HPI_M3_PRIMARY

#define HPI_M3_BTN_TONAL_BG       HPI_M3_PRIMARY_CONTAINER
#define HPI_M3_BTN_TONAL_FG       HPI_M3_PRIMARY_LIGHT
#define HPI_M3_BTN_TONAL_EDGE     HPI_M3_PRIMARY_CONTAINER

#define HPI_M3_BTN_SECONDARY_BG   HPI_M3_SURFACE_HIGH
#define HPI_M3_BTN_SECONDARY_FG   HPI_M3_ON_SURFACE
#define HPI_M3_BTN_SECONDARY_EDGE HPI_M3_OUTLINE

#define HPI_M3_BTN_DANGER_BG      HPI_M3_ERROR
#define HPI_M3_BTN_DANGER_FG      HPI_M3_ON_ERROR
#define HPI_M3_BTN_DANGER_EDGE    HPI_M3_ERROR

/* Selected = a SOLID accent fill (4.5:1 against an unselected SECONDARY).
 * A tinted container was tried: 1.04:1 luminance against SECONDARY, so on
 * this panel the choice was readable only by hue. */
#define HPI_M3_BTN_SELECTED_BG    HPI_M3_PRIMARY
#define HPI_M3_BTN_SELECTED_FG    HPI_M3_ON_PRIMARY
#define HPI_M3_BTN_SELECTED_EDGE  HPI_M3_PRIMARY

/* On/off switch (Rec channels, Link radios, HealthyLink slots). The off knob
 * is a light grey so "off" still reads as a control, not as missing. */
#define HPI_M3_SWITCH_TRACK_ON    HPI_M3_PRIMARY
#define HPI_M3_SWITCH_KNOB_ON     HPI_M3_ON_PRIMARY
#define HPI_M3_SWITCH_TRACK_OFF   HPI_M3_OUTLINE
#define HPI_M3_SWITCH_KNOB_OFF    HPI_M3_ON_SURFACE_VARIANT

#define HPI_M3_BTN_RADIUS         HPI_M3_RADIUS_PILL  /* list rows/cards keep RADIUS_MD/LG */
#define HPI_M3_BTN_EDGE_W         1
#define HPI_M3_BTN_PAD_HOR        HPI_M3_SPACE_4
#define HPI_M3_BTN_FONT           HPI_M3_FONT_CAPS

/* ==== 5. Interaction states ================================================
 * PRESSED: the fill is mixed toward white by this much while a finger is
 * down. Lightening (not darkening) because darkening a dark fill is
 * invisible; LVGL's default-theme 35 % black recolor is switched off on our
 * controls so the two don't fight. DISABLED_OPA is below with the helpers. */
#define HPI_M3_PRESSED_LIGHTEN    LV_OPA_20

/* Waveform trace line width, px (Live lanes, Home ECG; Trends draws its own). */
#define HPI_M3_TRACE_W  3

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

/* Visible press feedback for anything tappable that is NOT built with
 * hpi_ui_button_create() -- list rows, settings entries, chips. `fill` is the
 * object's resting fill; while pressed it lightens by HPI_M3_PRESSED_LIGHTEN.
 * Also cancels the default theme's black recolor, which only lv_button gets
 * and which is invisible on dark fills anyway. */
static inline void hpi_m3_apply_pressable(lv_obj_t *o, lv_color_t fill)
{
	lv_obj_set_style_bg_color(o, lv_color_lighten(fill, HPI_M3_PRESSED_LIGHTEN),
				  LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_STATE_PRESSED);
	lv_obj_set_style_recolor_opa(o, LV_OPA_TRANSP, LV_STATE_PRESSED);
}

#ifdef __cplusplus
}
#endif

#endif /* HPI_UI_M3_THEME_H */
