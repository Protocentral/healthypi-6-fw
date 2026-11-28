/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * The colour palettes. THIS is the file to edit to recolour the UI: one table
 * per theme, the same fields in the same order. What each field is for is in
 * struct hpi_m3_palette (hpi_m3_theme.h); which button kind uses which field
 * is the HPI_M3_BTN_* table there.
 *
 * Rules both tables keep (measure at RGB565 -- the UI renders 16-bit):
 *   - text and accents >= 4.5:1 on surface and surface_container
 *   - outline >= ~2.4:1 on surface, so control edges survive the panel
 *   - each on_* colour >= 4.5:1 on the fill it sits on
 */
#include "hpi_m3_theme.h"

#include <zephyr/toolchain.h>
#include "services/config_service.h"

/* The stored setting is the palette index. */
BUILD_ASSERT((int)HPI_M3_THEME_DARK == (int)HPI_CONFIG_THEME_DARK &&
	     (int)HPI_M3_THEME_LIGHT == (int)HPI_CONFIG_THEME_LIGHT &&
	     (int)HPI_M3_THEME_COUNT == (int)HPI_CONFIG_THEME_COUNT,
	     "theme ids must match the config values");

/* DARK -- "navy ink". A saturated dark hue rather than black or neutral grey:
 * on this backlit TFT, backlight bleed turns near-black and neutral greys into
 * one washed-out field. Bright traces on a darker plot well. */
static const struct hpi_m3_palette PAL_DARK = {
	.surface_dim          = 0x0A1826,
	.plot_well            = 0x0A1826,
	.surface              = 0x0F2233,
	.surface_container    = 0x18324A,
	.surface_high         = 0x23425E,
	.outline              = 0x3A5C7C,
	.divider              = 0x1F3B55,

	.on_surface           = 0xECEFF1,
	.on_surface_variant   = 0x9AA6AD,
	.on_surface_muted     = 0x8A9BAA,
	.on_surface_faint     = 0x5E7287,

	.primary              = 0x6FB3CC,
	.on_primary           = 0x0E1114,
	.primary_container    = 0x24566B,
	.on_primary_container = 0xA8D4E4,
	.error                = 0xF87171,
	.on_error             = 0x2A0A0A,
	.warning              = 0xFB923C,
	.success              = 0x4ADE80,
	.cta                  = 0xFBBF24,
	.on_cta               = 0x1F1300,

	.sig_ecg              = 0xFBBF24,
	.sig_ppg              = 0x6FB3CC,
	.sig_resp             = 0x4CC38A,
	.sig_temp             = 0xFB923C,
	.sig_hrv              = 0x9A94F5,
};

/* LIGHT -- "mist". What an LCD renders best: no backlight bleed to fight,
 * readable in bright rooms and at an angle. The page is a soft blue-grey
 * (#D3DDE4, ~20 % less luminous than near-white #EEF2F5, which glared at
 * full backlight); cards sit a step lighter, and the waveform plot well is
 * the BRIGHTEST surface, so traces get the most contrast where they are read.
 * Every accent and signal is a deep version of its dark-theme counterpart:
 * the bright ones fail on light (amber #FBBF24 is ~1.7:1). Signals are
 * >= 4.5:1 on cards and >= 5.2:1 on the plot well. The CTA stays a filled
 * amber with dark text. */
static const struct hpi_m3_palette PAL_LIGHT = {
	.surface_dim          = 0xC9D4DC,
	.plot_well            = 0xF5F8FA,
	.surface              = 0xD3DDE4,
	.surface_container    = 0xE6ECF0,
	.surface_high         = 0xC3CFD8,
	.outline              = 0x6F7F8C,
	.divider              = 0xBFCBD4,

	.on_surface           = 0x111820,
	.on_surface_variant   = 0x34414C,
	.on_surface_muted     = 0x4A5661,
	.on_surface_faint     = 0x7D8A96,

	.primary              = 0x0A6483,
	.on_primary           = 0xFFFFFF,
	.primary_container    = 0xBFE0EC,
	.on_primary_container = 0x0A4F66,
	.error                = 0xB71C1C,
	.on_error             = 0xFFFFFF,
	.warning              = 0x9A4A06,
	.success              = 0x0F6B35,
	.cta                  = 0xF59E0B,
	.on_cta               = 0x1F1300,

	.sig_ecg              = 0x9C4305,
	.sig_ppg              = 0x0369A1,
	.sig_resp             = 0x047857,
	.sig_temp             = 0xB93A0B,
	.sig_hrv              = 0x6D28D9,
};

static const struct hpi_m3_palette *const PALETTES[HPI_M3_THEME_COUNT] = {
	[HPI_M3_THEME_DARK]  = &PAL_DARK,
	[HPI_M3_THEME_LIGHT] = &PAL_LIGHT,
};

const struct hpi_m3_palette *hpi_m3_pal = &PAL_DARK;
static enum hpi_m3_theme_id s_active = HPI_M3_THEME_DARK;

void hpi_m3_theme_select(enum hpi_m3_theme_id id)
{
	if ((unsigned int)id >= HPI_M3_THEME_COUNT) {
		id = HPI_M3_THEME_DARK;
	}
	s_active = id;
	hpi_m3_pal = PALETTES[id];
}

enum hpi_m3_theme_id hpi_m3_theme_active(void)
{
	return s_active;
}
