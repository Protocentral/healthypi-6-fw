/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Material 3 design tokens for the HealthyPi 6 LVGL UI — the single source of
 * truth for color roles, the type scale, shape (corner radii) and the 4 dp
 * spacing grid. Screens and components reference ONLY these tokens.
 *
 * HealthyPi 6 palette ("LCD slate"): brand blue #6FB3CC for nav/selection/
 * primary actions; Signal Amber #FBBF24 for ECG/HR and the single big CTA per
 * screen.
 *
 * ---------------------------------------------------------------------------
 * HOW TO RESTYLE THE UI -- every colour, radius and state lives in this file.
 *
 *   1. Background / cards / outlines ..... "Surfaces" below
 *   2. Text greys ........................ "Text"
 *   3. Brand + status accents ............ "Accents"
 *   4. What each button kind looks like .. "Buttons" (HPI_M3_BTN_*)
 *   5. Pressed / disabled feedback ....... "Interaction states"
 *
 * Screens never name a hex value; components never name a colour that is not
 * a token here. Change a value, rebuild, done.
 * ---------------------------------------------------------------------------
 *
 * Why slate and not near-black: this is a backlit TFT, not an OLED. Backlight
 * bleed lifts the darkest greys toward one shade, so the previous ramp
 * (#14171A/#1C2126/#232A31, 1.12:1 between levels, 1.45:1 for the outline)
 * merged into one flat field and outlined controls vanished. The surfaces now
 * start above the bleed floor and step further apart; waveforms keep a darker
 * PLOT_WELL so traces stay at maximum contrast. Keep every accent and text
 * token >= 4.5:1 on SURFACE and SURFACE_CONTAINER if you change them (measure
 * at RGB565 -- the UI renders 16-bit).
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

/* ==== 1. Surfaces (darkest -> lightest) ==== */
#define HPI_M3_SURFACE_DIM        lv_color_hex(0x161B20)  /* splash / ambient / power backdrop */
#define HPI_M3_PLOT_WELL          lv_color_hex(0x161B20)  /* behind waveforms: recessed, max trace contrast */
#define HPI_M3_SURFACE            lv_color_hex(0x1E252C)  /* screen background */
#define HPI_M3_SURFACE_CONTAINER  lv_color_hex(0x29323B)  /* card / list row / nav bar */
#define HPI_M3_SURFACE_HIGH       lv_color_hex(0x35404B)  /* raised: secondary buttons, stepper keys */
#define HPI_M3_OUTLINE            lv_color_hex(0x4A5763)  /* 1 px card / button edge */
#define HPI_M3_DIVIDER            lv_color_hex(0x323C46)  /* hairline row divider, chart grid */

/* ==== 2. Text ==== */
#define HPI_M3_ON_SURFACE         lv_color_hex(0xECEFF1)  /* primary text (bright) */
#define HPI_M3_ON_SURFACE_VARIANT lv_color_hex(0x9AA6AD)  /* secondary text / labels */
#define HPI_M3_ON_SURFACE_MUTED   lv_color_hex(0x7F8B93)  /* units, captions (4.5:1 on SURFACE) */
#define HPI_M3_ON_SURFACE_FAINT   lv_color_hex(0x5C6870)  /* disabled / pending -- decorative only */

/* ==== 3. Accents ==== */
#define HPI_M3_PRIMARY            lv_color_hex(0x6FB3CC)  /* brand blue: nav, selection, primary actions */
#define HPI_M3_ON_PRIMARY         lv_color_hex(0x0E1114)  /* dark text/icon on brand blue */
#define HPI_M3_PRIMARY_CONTAINER  lv_color_hex(0x1E4352)  /* brand-tinted fill: selected tab/segment */
#define HPI_M3_PRIMARY_LIGHT      lv_color_hex(0xA8D4E4)  /* text/icon on PRIMARY_CONTAINER */
#define HPI_M3_ERROR              lv_color_hex(0xF87171)
#define HPI_M3_ON_ERROR           lv_color_hex(0x2A0A0A)  /* dark text on red (6.9:1; white was 2.3:1) */
#define HPI_M3_WARNING            lv_color_hex(0xFB923C)
#define HPI_M3_SUCCESS            lv_color_hex(0x4ADE80)

/* Brand CTA: the single big call-to-action per screen = Signal Amber. */
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
