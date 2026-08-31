#!/usr/bin/env bash
# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT
# HealthyPi 6 — build any firmware target.
#
#   scripts/build.sh m7 [dev|prod]     M7 application + display UI  -> build/m7
#   scripts/build.sh m4                M4 algorithm core            -> build/m4
#   scripts/build.sh signed [dev|prod] MCUboot + signed M7          -> build/m7s
#   scripts/build.sh esp32 [args…]     ESP32-C6 (external repo)
#   scripts/build.sh all               m7 + m4
#
# Board is fixed to healthypi6_v5 (see scripts/env.sh). Flavor defaults to dev.
#
# WHICH ONE SHIPS: `signed`, built with FLAVOR=prod — and in practice you want
# scripts/release.sh, which builds it, packages a bundle and refuses to produce
# one that could not be updated in the field. A `m7` build has no bootloader, no
# MCUmgr img group and no recovery entry: a unit flashed with it can only ever be
# updated over SWD, with the case open. See docs/ARCHITECTURE.md §2.
set -euo pipefail

# shellcheck source=scripts/env.sh
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"
cd "$HPI_ROOT"

usage() { sed -n '2,18p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

TARGET="${1:-}"; shift || true
[ -z "$TARGET" ] && usage 1
case "$TARGET" in -h|--help|help) usage 0 ;; esac

# The display conf fragment + overlays ADD to the build set, and `west -p auto`
# keys its pristine decision off board/app only — it would silently drop them.
# Hence -p always for anything that carries overlays.
# HPI_EXTRA_CONF appends further app-relative fragments (';'-separated) on top
# of the flavor + display set — e.g. HPI_EXTRA_CONF=disp_debug.conf for the UI
# debug build. Empty by default, so a normal build is unchanged.
M7_CONF_BASE="disp_conf.conf${HPI_EXTRA_CONF:+;$HPI_EXTRA_CONF}"
M7_OVERLAYS="display-gc9503v.overlay;$HPI_BOARD_DIR/healthylink-compute.overlay"


build_m7() {
    local flavor="${1:-dev}"
    echo "▶ M7 ($flavor) + M3 UI + HealthyLink Compute → build/m7"
    west build -p always -b "$HPI_BOARD_M7" -d build/m7 app_m7 \
        -- -DEXTRA_CONF_FILE="prj.${flavor}.conf;$M7_CONF_BASE" \
           -DEXTRA_DTC_OVERLAY_FILE="$M7_OVERLAYS"
}

build_m4() {
    echo "▶ M4 (algorithms) → build/m4"
    west build -p auto -b "$HPI_BOARD_M4" -d build/m4 app_m4
}

build_signed() {
    local flavor="${1:-dev}"
    local out="${HPI_SIGNED_OUT:-build/m7s}"

    if [ ! -f "$HP6_SIGNING_KEY" ]; then
        echo "=============================================================="
        echo "No signing key at $HP6_SIGNING_KEY — generating a LOCAL DEV key."
        echo "Never committed. A device flashed with one dev key will REFUSE an"
        echo "image signed with another; see keys/README.md."
        echo "=============================================================="
        imgtool keygen -k "$HP6_SIGNING_KEY" -t ecdsa-p256
    fi

    echo "▶ SIGNED M7 ($flavor): MCUboot + image 0 → $out"
    echo "  key: $HP6_SIGNING_KEY"
    # The key must be passed as an ABSOLUTE path: a relative Kconfig key path
    # resolves against the west topdir / MCUboot repo, not this repo.
    west build -p always --sysbuild -b "$HPI_BOARD_M7" -d "$out" app_m7 \
        -- -DSB_EXTRA_CONF_FILE="$HPI_ROOT/app_m7/sysbuild-mcuboot.conf" \
           -DSB_CONFIG_BOOT_SIGNATURE_KEY_FILE=\""$HP6_SIGNING_KEY"\" \
           -Dapp_m7_EXTRA_CONF_FILE="prj.${flavor}.conf;$M7_CONF_BASE;prj.signed.conf" \
           -Dapp_m7_CONFIG_HPI_M4_SIGNING_KEY=\""$HP6_SIGNING_KEY"\" \
           -Dapp_m7_EXTRA_DTC_OVERLAY_FILE="$M7_OVERLAYS;$HPI_BOARD_DIR/healthypi6_v5_mcuboot_app.overlay" \
           -Dmcuboot_EXTRA_DTC_OVERLAY_FILE="$HPI_BOARD_DIR/healthypi6_v5_mcuboot_boot.overlay"

    echo ""
    echo "✅ signed build in $out"
    echo "   bootloader : $out/mcuboot/zephyr/zephyr.hex        @ 0x08000000"
    echo "   app (slot0): $out/app_m7/zephyr/zephyr.signed.hex  @ 0x08020000"
    echo "   OTA payload: $out/app_m7/zephyr/zephyr.signed.bin"
    echo "   flash with : scripts/flash.sh signed   (M4 via scripts/flash.sh m4)"
}

build_esp32() {
    local hb; hb="$(hpi_find_healthybridge)" || exit 1
    echo "▶ ESP32-C6 via $hb/hp6.sh"
    ( cd "$hb" && ./hp6.sh "${@:-build}" )
}

case "$TARGET" in
    m7)     build_m7 "${1:-${FLAVOR:-dev}}" ;;
    m4)     build_m4 ;;
    signed) build_signed "${1:-${FLAVOR:-dev}}" ;;
    esp32)  build_esp32 "$@" ;;
    all)    build_m7 "${FLAVOR:-dev}"; build_m4 ;;
    *)      echo "unknown target '$TARGET'" >&2; usage 1 ;;
esac
