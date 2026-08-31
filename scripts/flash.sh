#!/usr/bin/env bash
# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT
# HealthyPi 6 — flash any target over SWD (or USB, for the ESP32).
#
#   scripts/flash.sh all           M4 then M7, from build/m4 + build/m7 (default)
#   scripts/flash.sh m7            M7 only
#   scripts/flash.sh m4            M4 only
#   scripts/flash.sh signed        MCUboot + signed M7, from build/m7s
#   scripts/flash.sh factory       production programming: signed M7 + M4 + checks
#                                  (--allow-test-vid to rehearse before F7)
#   scripts/flash.sh esp32 [PORT]  ESP32-C6 (external HealthyBridge repo)
#
# DEV AND SIGNED ARE MUTUALLY EXCLUSIVE ON A BOARD. Both link at internal-flash
# addresses: the dev M7 app links at 0x08000000 and overwrites MCUboot, the
# signed one links at 0x08020000 behind it. Switching either way is a full
# reflash — that is the mechanism, not a bug. The M4 is independent of both.
set -euo pipefail

# shellcheck source=scripts/env.sh
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"
cd "$HPI_ROOT"

usage() { sed -n '2,17p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }
# NOTE: keep the sed range above in step with the header block when editing it.

TARGET="${1:-all}"; shift || true
case "$TARGET" in -h|--help|help) usage 0 ;; esac

# Back-compat with the older flag form used across the docs and by muscle memory.
case "$TARGET" in
    --m4-only) TARGET=m4 ;;
    --m7-only) TARGET=m7 ;;
esac

need_build() {
    local dir="$1" what="$2" how="$3"
    if [ ! -f "$dir/zephyr/zephyr.elf" ]; then
        echo "❌ no $what build at $dir — run: $how" >&2
        exit 1
    fi
}

flash_m4() {
    need_build build/m4 "M4" "scripts/build.sh m4"
    echo "--- M4 ---"
    west flash -d build/m4
}

flash_m7() {
    need_build build/m7 "M7" "scripts/build.sh m7"
    echo "--- M7 (dev, no bootloader) ---"
    west flash -d build/m7
    echo "⚠️  This image has no bootloader, no MCUmgr img group and no recovery"
    echo "    entry. A unit shipped like this can only be updated over SWD."
    echo "    Ship scripts/release.sh output instead — docs/ARCHITECTURE.md §2."
}

flash_signed() {
    local out="${HPI_SIGNED_OUT:-build/m7s}"
    if [ ! -f "$out/app_m7/zephyr/zephyr.signed.hex" ]; then
        echo "❌ no signed build at $out — run: scripts/build.sh signed" >&2
        exit 1
    fi
    echo "--- MCUboot + signed M7 ---"
    west flash -d "$out"
    echo "✅ signed M7 flashed. The M4 is NOT an MCUboot image and is not"
    echo "   touched by this — flash it with: scripts/flash.sh m4"
}

flash_factory() {
    # Production programming order matters: the M4 first, so that when the M7
    # comes up and MCUboot hands over, the algorithm core it tries to bind to
    # already holds a real image instead of erased flash.
    local out="${HPI_SIGNED_OUT:-build/release/m7s}"
    [ -d "$out" ] || out="build/m7s"

    local allow_test_vid=0
    for a in "$@"; do
        case "$a" in
            --allow-test-vid) allow_test_vid=1 ;;
            *) echo "unknown factory option '$a'" >&2; exit 1 ;;
        esac
    done

    echo "=== FACTORY PROGRAMMING ==="
    if [ -d "$out" ]; then
        HPI_SIGNED_OUT="$out" bash "$HPI_ROOT/tools/ci/check_prod_surface.sh" \
            --release "$out" || {
            # Same narrow escape release.sh carries, and for the same reason: the
            # gate fails on the unregistered USB VID, whose allocation has
            # external latency. Without this the factory path itself — programming
            # order, the option bytes, the EOL self-test — could not be rehearsed
            # on a bench until the allocation lands, which is exactly backwards.
            if [ "$allow_test_vid" = 1 ]; then
                echo ""
                echo "⚠️  --allow-test-vid: programming a NON-SHIPPABLE build."
                echo "   Bench rehearsal only. Do NOT ship this unit."
            else
                echo "❌ refusing to program: that build is not fit to ship." >&2
                echo "   Rehearsing before the VID allocation? re-run:" >&2
                echo "     scripts/flash.sh factory --allow-test-vid" >&2
                exit 1
            fi
        }
    fi
    flash_m4
    HPI_SIGNED_OUT="$out" flash_signed
    cat <<'EOF'

Remaining line steps:
  1. ESP32-C6:  scripts/flash.sh esp32
  2. Option bytes: confirm BCM4=1 and BOOT_CM4_ADD0=0x0810
  3. EOL self-test over CDC1 (group-64 0x0080) and record the serial
EOF
}

flash_esp32() {
    local hb; hb="$(hpi_find_healthybridge)" || exit 1
    local port="${1:-}"

    # With the STM32 attached there are several usbmodem ports and picking the
    # wrong one is easy — the HealthyPi's own CDC0/CDC1 also enumerate. An
    # earlier version of this script defaulted to a fixed number that was
    # actually HealthyPi CDC0, and flashes went to the wrong device. Probe, and
    # refuse when the answer is ambiguous.
    if [ -z "$port" ]; then
        echo "🔎 probing for an ESP32-C6…"
        local found=()
        for p in /dev/cu.usbmodem* /dev/ttyACM*; do
            [ -e "$p" ] || continue
            if python -m esptool --chip esp32c6 -p "$p" chip_id >/dev/null 2>&1; then
                found+=("$p")
            fi
        done
        case "${#found[@]}" in
            1) port="${found[0]}"; echo "   found $port" ;;
            0) echo "❌ no ESP32-C6 found. Pass the port explicitly:" >&2
               echo "     scripts/flash.sh esp32 /dev/cu.usbmodemXXXXX" >&2; exit 1 ;;
            *) echo "❌ multiple candidates (${found[*]}) — pass one explicitly" >&2; exit 1 ;;
        esac
    fi

    ( cd "$hb" && ./hp6.sh flash "$port" )
    echo "Verify: the log tag must be 'hb_main'/'hb_spi'. 'hl_spi' means the"
    echo "superseded in-tree ESP32 app is still on the device."
}

case "$TARGET" in
    all)     flash_m4; flash_m7 ;;
    m4)      flash_m4 ;;
    m7)      flash_m7 ;;
    signed)  flash_signed ;;
    factory) flash_factory "$@" ;;
    esp32)   flash_esp32 "$@" ;;
    *)       echo "unknown target '$TARGET'" >&2; usage 1 ;;
esac

echo "🎉 done."
