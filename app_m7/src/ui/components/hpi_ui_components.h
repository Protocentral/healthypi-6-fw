/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Modular, theme-driven UI components (the building blocks screens compose).
 * Each builder applies Material-3 tokens from theme/hpi_m3_theme.h via direct
 * setters — no static styles, no cross-screen knowledge.
 */
#ifndef HPI_UI_COMPONENTS_H
#define HPI_UI_COMPONENTS_H

#include <lvgl.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* M3 top app bar: a title row on the surface-container. Returns the bar. */
lv_obj_t *hpi_ui_appbar_create(lv_obj_t *parent, const char *title);

/* The HealthyPi 6 brand lockup, `width_px` wide (any size), aspect preserved.
 * Two A8 alpha masks stacked and recoloured from the theme (see ui/assets/).
 * Returns a transparent container sized exactly to the lockup, so it drops
 * into a flex layout. */
lv_obj_t *hpi_ui_logo_create(lv_obj_t *parent, int width_px);

/* The shared top status bar — one implementation for every screen.
 *
 * Left slot: `title` when non-NULL, otherwise the live date ("TUE 15 JUL").
 * Right cluster, identical everywhere: ESP32 link · USB · battery · clock.
 * Every instance is registered here; hpi_ui_statusbar_refresh() updates them
 * all from one RTC read and one power-service read. Screens are built once at
 * boot and never deleted, so there is no unregister path. Returns the bar. */
lv_obj_t *hpi_ui_statusbar_create(lv_obj_t *parent, const char *title);

/* Retitle a bar created with a title (Record: "New Recording" -> "Recording").
 * No-op on a date bar — that slot is owned by the clock. */
void hpi_ui_statusbar_set_title(lv_obj_t *bar, const char *title);

/* The bar's left group, for a screen that puts something ahead of the title
 * (Record's recording dot, the sub-screen back chevron). Children are appended,
 * so move yours to index 0 to place it before the title. */
lv_obj_t *hpi_ui_statusbar_left(lv_obj_t *bar);

/* Refresh every status bar: clock, date, battery, USB and link glyphs. Call from
 * the UI thread (~2 Hz) regardless of which screen is showing — bars on hidden
 * screens are updated too, so switching tabs never reveals a stale clock. */
void hpi_ui_statusbar_refresh(void);

/* Sub-screen header: a status bar with a back chevron (routes to `back_to`)
 * ahead of the title. Used by the More-submenu screens (Link/Settings/Alert/
 * OTA/HRV), so they carry the same clock + battery as the main tabs. */
lv_obj_t *hpi_ui_subbar_create(lv_obj_t *parent, const char *title, int back_to);

/* M3 navigation bar with Home/Record/Status; each item calls
 * hpi_ui_show_screen(). `active` highlights the current destination. */
lv_obj_t *hpi_ui_navbar_create(lv_obj_t *parent, int active);

/* Move the active highlight without rebuilding the bar (safe from a nav click
 * event — never delete the bar from inside its own callback). */
void hpi_ui_navbar_set_active(lv_obj_t *bar, int active);

/* Name the electrodes in an HP6_LEAD_OFF_* mask, e.g. "RA · LL". Writes ""
 * for mask 0. Shared so Home and Live say the same thing. */
void hpi_ui_lead_off_text(uint8_t mask, char *buf, size_t len);

/* [ − ] value [ + ] stepper — the touch-friendly way to set a number.
 *
 * Prefer this to lv_slider: a slider's track is ~10 px tall and emits a
 * value-changed event per touch sample (~119 Hz) on the LVGL thread; the
 * stepper is two HPI_M3_TOUCH_MIN keys and one event per tap.
 *
 * `fmt` is a printf format taking one int, e.g. "%d%%". `on_change` is called
 * from the UI thread after the value moves and must not block. Clamps to
 * [min,max]; `step` is applied per tap. */
struct hpi_ui_stepper {
	lv_obj_t *root;
	lv_obj_t *value;
	int32_t   v, min, max, step;
	const char *fmt;
	void (*on_change)(int32_t v, void *user);
	void *user;
};
void hpi_ui_stepper_create(struct hpi_ui_stepper *s, lv_obj_t *parent,
			   int32_t min, int32_t max, int32_t step, int32_t init,
			   const char *fmt,
			   void (*on_change)(int32_t v, void *user), void *user);
/* Set the value programmatically (clamped, relabels, does NOT call on_change). */
void hpi_ui_stepper_set(struct hpi_ui_stepper *s, int32_t v);

/* Draw an lv_chart series as a bare line, with no per-sample marker. Call this
 * on EVERY chart: LVGL's default theme styles LV_PART_INDICATOR with an 8 px
 * circle, so every point gets a dot otherwise. The markers live on
 * LV_PART_INDICATOR, *not* LV_PART_ITEMS (the line itself) — zeroing the wrong
 * part is a silent no-op — and the series' final point is drawn
 * unconditionally with no size guard, so opacity must be killed too. */
void hpi_ui_chart_hide_points(lv_obj_t *chart);

/* Scrolling waveform panel backed by an lv_chart (shift mode).
 *
 * The chart keeps a FIXED Y range, set once at create and never touched again
 * — lv_chart_set_range per frame flickers the whole screen under full-refresh
 * double-buffering. The component conditions the data instead: a slow EMA
 * baseline (DC-block) removes the offset, then a slowly-adapted gain maps the
 * AC swing into the fixed window, always centred and ~full-height. */
struct hpi_ui_waveform {
	lv_obj_t          *panel;
	lv_obj_t          *chart;
	lv_chart_series_t *series;
	int32_t            baseline;   /* DC estimate (EMA over samples)        */
	int32_t            peak;       /* decaying max |AC| over the window     */
	int32_t            floor;      /* min peak (noise-zoom guard, raw units)*/
	int32_t            gain_q;     /* Q16 gain: display = (ac*gain_q)>>16    */
	uint16_t           recount;    /* samples since last gain update         */
	int32_t            user_q;     /* user zoom on top of gain_q, Q8 (256=x1) */
	uint8_t            bl_shift;   /* DC-block EMA shift: larger = slower     */
	bool               primed;
};

/* Display zoom applied ON TOP of the automatic scaling, Q8 (256 = x1).
 * A multiplier, never a mV/division gain: nothing here maps pixels to
 * millivolts, so an absolute unit would be a fabricated number. */
void hpi_ui_waveform_set_zoom(struct hpi_ui_waveform *w, int32_t mult_q8);

/* Resize the sweep window, in chart points; fewer points is a faster sweep.
 * Label it in seconds (points / push rate), never mm/s — the firmware does not
 * know the panel's physical width. Reallocates the series, so call it on a
 * user action, not per frame. */
void hpi_ui_waveform_set_points(struct hpi_ui_waveform *w, uint16_t points);
/* bl_shift sets the baseline (DC-block) high-pass: cutoff ~= push_rate /
 * (2*pi*2^bl_shift). Use a small shift for signals with heavy baseline wander
 * (PPG ~5 -> ~0.6 Hz) and a larger one for steadier signals (ECG ~9). */
void hpi_ui_waveform_create(struct hpi_ui_waveform *w, lv_obj_t *parent,
			    const char *label, lv_color_t color,
			    uint16_t points, int32_t ymin, int32_t ymax,
			    uint8_t bl_shift);
/* Push one sample (call from the UI thread only). DC-blocks and auto-scales. */
void hpi_ui_waveform_push(struct hpi_ui_waveform *w, int32_t v);

/* Metric tile (HR/SpO2/RR/Temp): caption + big value + unit. */
struct hpi_ui_vitals_tile {
	lv_obj_t *value;
};
void hpi_ui_vitals_tile_create(struct hpi_ui_vitals_tile *t, lv_obj_t *parent,
			       const char *caption, lv_color_t value_color);
void hpi_ui_vitals_tile_set(struct hpi_ui_vitals_tile *t, const char *text);

#ifdef __cplusplus
}
#endif

#endif /* HPI_UI_COMPONENTS_H */
