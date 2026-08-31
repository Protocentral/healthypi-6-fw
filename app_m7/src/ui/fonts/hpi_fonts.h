/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * HealthyPi 6 design fonts — LVGL declarations. The bitmaps are generated from
 * the design TTFs (Rubik / Manrope / Saira / JetBrains Mono, all OFL) by
 * convert_fonts.sh into lvgl/*.c. The theme (hpi_m3_theme.h) maps its role
 * tokens onto these; screens reference the tokens, never these names.
 */
#ifndef HPI_UI_FONTS_H
#define HPI_UI_FONTS_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

LV_FONT_DECLARE(rubik_500_148);   /* focal HR / hero numerals */
LV_FONT_DECLARE(rubik_500_28);    /* vital-chip numerals      */
LV_FONT_DECLARE(manrope_700_16);  /* caption / unit / body / label */
LV_FONT_DECLARE(manrope_700_12);  /* small labels / nav / chip role */
LV_FONT_DECLARE(saira_700_28);    /* headline / display titles */
LV_FONT_DECLARE(saira_600_18);    /* screen titles */
LV_FONT_DECLARE(jetbrains_mono_14); /* clock / filenames / versions */
LV_FONT_DECLARE(matsym_20);       /* Material Symbols — chips/status/caption */
LV_FONT_DECLARE(matsym_24);       /* Material Symbols — nav */

#ifdef __cplusplus
}
#endif

/* Icon glyph string literals (HPI_SYM_*), paired with the matsym_* fonts. */
#include "ui/fonts/hpi_symbols.h"

#endif /* HPI_UI_FONTS_H */
