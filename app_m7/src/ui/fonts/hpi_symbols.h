/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Material Symbols Outlined glyphs used by the UI, as UTF-8 string literals
 * for lv_label_set_text(). Codepoints are baked into the matsym_* LVGL fonts
 * (see convert_fonts.sh / hpi_fonts.h). Apply a matsym font token
 * (HPI_M3_FONT_ICON*) to any label that renders these.
 */
#ifndef HPI_UI_SYMBOLS_H
#define HPI_UI_SYMBOLS_H

#define HPI_SYM_HR         "\xEE\xA1\xBD"   /* favorite U+E87D — HR caption heart */
#define HPI_SYM_SPO2       "\xEF\x9B\x9B"   /* spo2 U+F6DB — SpO2 chip */
#define HPI_SYM_RESP       "\xEE\x84\xA4"   /* pulmonology U+E124 — respiration chip */
#define HPI_SYM_TEMP       "\xEE\x87\xBF"   /* device_thermostat U+E1FF — temperature chip */
#define HPI_SYM_HRV        "\xEE\x89\xA8"   /* scatter_plot U+E268 — HRV chip */
#define HPI_SYM_WIFI       "\xEE\x98\xBE"   /* wifi U+E63E — status wifi */
#define HPI_SYM_BT         "\xEE\x86\xA7"   /* bluetooth U+E1A7 — status bluetooth */
#define HPI_SYM_SD         "\xEE\x87\x82"   /* sd_card U+E1C2 — status SD */
#define HPI_SYM_BATT       "\xEE\x86\xA4"   /* battery_full U+E1A4 — status battery */
#define HPI_SYM_HOME       "\xEE\xA2\x8A"   /* home U+E88A — nav Home */
#define HPI_SYM_LIVE       "\xEE\xAA\xA2"   /* monitor_heart U+EAA2 — nav Live */
#define HPI_SYM_REC        "\xEE\x81\xA1"   /* fiber_manual_record U+E061 — nav Rec */
#define HPI_SYM_TRENDS     "\xEF\x86\x90"   /* monitoring U+F190 — nav Trends */
#define HPI_SYM_MORE       "\xEE\x97\x93"   /* more_horiz U+E5D3 — nav More */
#define HPI_SYM_TREND_FLAT "\xEE\xA3\xA4"   /* trending_flat U+E8E4 — stable trend */
#define HPI_SYM_CHECK      "\xEE\xA1\xAC"   /* check_circle U+E86C — self-test pass */
#define HPI_SYM_PENDING    "\xEE\xA0\xB6"   /* radio_button_unchecked U+E836 — self-test pending */

/* More screens: Link · Settings · Alert limits · Ambient · OTA */
#define HPI_SYM_MQTT       "\xEE\x8B\x83"   /* cloud_upload U+E2C3 */
#define HPI_SYM_USB        "\xEE\x87\xA0"   /* usb U+E1E0 */
#define HPI_SYM_WIFI_FIND  "\xEE\xAC\xB1"   /* wifi_find U+EB31 */
#define HPI_SYM_INFO       "\xEE\xA2\x8E"   /* info U+E88E */
#define HPI_SYM_BRIGHT     "\xEE\x8E\xAB"   /* brightness_6 U+E3AB */
#define HPI_SYM_SLEEP      "\xEE\x87\xB9"   /* bedtime U+E1F9 */
#define HPI_SYM_ALERT      "\xEE\x9F\xB4"   /* notifications U+E7F4 */
#define HPI_SYM_CLOCK      "\xEE\x86\x92"   /* schedule U+E192 */
#define HPI_SYM_CHEV       "\xEE\x90\x89"   /* chevron_right U+E409 */
#define HPI_SYM_WARN       "\xEE\x80\x82"   /* warning U+E002 */
#define HPI_SYM_EMERG      "\xEE\xA0\xAA"   /* emergency_home U+E82A */
#define HPI_SYM_MINUS      "\xEE\x85\x9B"   /* remove U+E15B */
#define HPI_SYM_PLUS       "\xEE\x85\x85"   /* add U+E145 */
#define HPI_SYM_VOL        "\xEE\x81\x90"   /* volume_up U+E050 */
#define HPI_SYM_DOWNLOAD   "\xEE\x85\xB1"   /* download U+E171 */
#define HPI_SYM_POWER      "\xEE\x98\xBC"   /* power U+E63C */
#define HPI_SYM_ARROW      "\xEE\x97\x88"   /* arrow_forward U+E5C8 */

#endif /* HPI_UI_SYMBOLS_H */
