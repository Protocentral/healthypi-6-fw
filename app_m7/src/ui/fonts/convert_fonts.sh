#!/bin/bash
# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT
# HealthyPi 6 display-redesign fonts -> LVGL bitmap fonts.
#
# The design specifies the real type stack: Rubik (all
# numerals), Manrope (labels/caps/units), Saira (screen titles), JetBrains Mono
# (clock/filenames/versions). The design HTML references them via Google Fonts;
# the actual open-source TTFs (OFL) are vendored in ./ttf. This converts them to
# LVGL C fonts at the sizes the redesign uses. Re-run after changing sizes/ranges.
#
# Deps: lv_font_conv (via `npx lv_font_conv`). Output: ./lvgl/*.c (checked in so a
# normal build needs no Node). Declarations: ./hpi_fonts.h.
set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
# lv_font_conv echoes its argv into an "Opts:" comment in every generated file,
# so run from the repo root and pass RELATIVE paths -- otherwise each file
# records the absolute path of whoever last ran this, and every regeneration
# churns nine files with one developer's home directory.
ROOT="$(cd "$DIR/../../../.." && pwd)"
cd "$ROOT"
REL="app_m7/src/ui/fonts"
TTF="$REL/ttf"
OUT="$REL/lvgl"
CONV="npx --no-install lv_font_conv"
mkdir -p "$OUT"

# Numerals only: space % + , - . / 0-9 : (Rubik). Small subset -> tiny flash.
NUM="0x20,0x25,0x2B-0x3A"
# Text: printable ASCII + degree sign (0xB0, "°C") + middle dot (0xB7, "·" sep).
TXT="0x20-0x7E,0xB0,0xB7"
# Material Symbols subset — only the icons the redesign uses (see hpi_symbols.h).
# Home/nav/status: favorite spo2 pulmonology device_thermostat scatter_plot wifi
# bluetooth sd_card battery_full home monitor_heart fiber_manual_record monitoring
# more_horiz trending_flat check_circle radio_button_unchecked
# More screens (P8): cloud_upload usb wifi_find info brightness_6 bedtime
# notifications schedule chevron_right warning emergency_home remove add volume_up
# download power arrow_forward
# Status bar: bolt (battery charging; shown next to the percentage)
SYM="0xE87D,0xF6DB,0xE124,0xE1FF,0xE268,0xE63E,0xE1A7,0xE1C2,0xE1A4,0xE88A,0xEAA2,0xE061,0xF190,0xE5D3,0xE8E4,0xE86C,0xE836,0xE2C3,0xE1E0,0xEB31,0xE88E,0xE3AB,0xE1F9,0xE7F4,0xE192,0xE409,0xE002,0xE82A,0xE15B,0xE145,0xE050,0xE171,0xE63C,0xE5C8,0xEA0B"

# The upstream font's copyright + SPDX line. lv_font_conv does not emit one, and
# these arrays are MODIFIED versions in OFL terms (subset + rasterised), so the
# notice has to be re-attached on every regeneration or the tree quietly loses
# it -- which is exactly what happened before this function existed. See
# ./README.md and THIRD_PARTY_NOTICES.md.
font_copyright() { # <ttf>
	case "$1" in
	Rubik-*)            echo "Copyright 2015 The Rubik Project Authors (https://github.com/googlefonts/rubik)|OFL-1.1" ;;
	Manrope-*)          echo "Copyright 2019 The Manrope Project Authors (https://github.com/sharanda/manrope)|OFL-1.1" ;;
	Saira-*)            echo "Copyright 2020 The Saira Project Authors (https://github.com/Omnibus-Type/Saira)|OFL-1.1" ;;
	JetBrainsMono-*)    echo "Copyright 2020 The JetBrains Mono Project Authors (https://github.com/JetBrains/JetBrainsMono)|OFL-1.1" ;;
	MaterialSymbols-*)  echo "Copyright 2026 Google LLC. All Rights Reserved.|Apache-2.0" ;;
	*) echo "UNKNOWN FONT -- add it to font_copyright()|UNKNOWN" ;;
	esac
}

prepend_notice() { # <ttf> <out.c>
	local meta copyright spdx tmp
	meta="$(font_copyright "$1")"
	copyright="${meta%|*}"
	spdx="${meta##*|}"
	tmp="$(mktemp)"
	{
		echo "/*"
		echo " * $copyright"
		echo " * SPDX-License-Identifier: $spdx"
		echo " *"
		echo " * Generated from the bundled .ttf by lv_font_conv (see ../convert_fonts.sh)."
		echo " * A subset/rasterised copy of the font above -- covered by the font's licence,"
		echo " * NOT by this project's MIT licence. Licence texts in ../LICENSES/;"
		echo " * summary in THIRD_PARTY_NOTICES.md at the repo root."
		echo " */"
		cat "$2"
	} > "$tmp"
	mv "$tmp" "$2"
	chmod 644 "$2"   # mktemp makes 0600; these are checked-in sources
}

gen() { # <ttf> <size> <bpp> <range> <name>
	echo "  $5 (${2}px)"
	$CONV --no-compress --font "$TTF/$1" --size "$2" --bpp "$3" \
	      --range "$4" --format lvgl --lv-include lvgl.h \
	      --lv-font-name "$5" --output "$OUT/$5.c"
	prepend_notice "$1" "$OUT/$5.c"
}

echo "Rubik-500 (numerals)"
gen Rubik-500.ttf          148 4 "$NUM" rubik_500_148   # focal HR hero
gen Rubik-500.ttf           28 4 "$NUM" rubik_500_28    # vital chips

echo "Manrope-700 (labels / units / caps)"
gen Manrope-700.ttf         16 4 "$TXT" manrope_700_16  # caption / unit / body / label
gen Manrope-700.ttf         12 4 "$TXT" manrope_700_12  # small labels / nav / chip role

echo "Saira (titles)"
gen Saira-700.ttf           28 4 "$TXT" saira_700_28    # headline / display
gen Saira-600.ttf           18 4 "$TXT" saira_600_18    # title

echo "JetBrains Mono (clock / mono)"
gen JetBrainsMono-Regular.ttf 14 4 "$TXT" jetbrains_mono_14

echo "Material Symbols Outlined (icon subset)"
gen MaterialSymbols-Outlined.ttf 20 4 "$SYM" matsym_20   # chips / status / caption
gen MaterialSymbols-Outlined.ttf 24 4 "$SYM" matsym_24   # nav

echo "done -> $OUT"
ls -la "$OUT"/*.c
