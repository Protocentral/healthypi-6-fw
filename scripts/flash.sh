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
#   scripts/flash.sh esp32 --full [PORT]
#                                  ESP32-C6 full install: erase the whole chip, write
#                                  the merged A/B image. The FIRST flash of any unit
#                                  (required to reach the A/B partition layout).
#                                  ERASES stored Wi-Fi credentials and settings.
#   scripts/flash.sh esp32 --app-only [PORT]
#                                  ESP32-C6 app only: keeps stored Wi-Fi; refuses a
#                                  unit that is not already on the A/B layout.
#
# ESP32-C6: the M7 holds the C6 in reset from boot, and a C6 in reset does not
# enumerate its USB port. Release it first, after every board boot:
#     healthypi conn enable --radios none      (dev shell: hpi link on)
# Both modes run healthybridge-esp32's flash_hp6.sh on its build.hp6
# (build it with: scripts/build.sh esp32).
#
# DEV AND SIGNED ARE MUTUALLY EXCLUSIVE ON A BOARD. Both link at internal-flash
# addresses: the dev M7 app links at 0x08000000 and overwrites MCUboot, the
# signed one links at 0x08020000 behind it. Switching either way is a full
# reflash — that is the mechanism, not a bug. The M4 is independent of both.
set -euo pipefail

# shellcheck source=scripts/env.sh
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"
cd "$HPI_ROOT"

usage() { sed -n '2,29p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }
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

    for a in "$@"; do
        echo "unknown factory option '$a'" >&2; exit 1
    done

    echo "=== FACTORY PROGRAMMING ==="
    # Say which build is about to be programmed, and when it was built. The
    # default prefers build/release/m7s (release.sh output) over build/m7s
    # (build.sh signed), so a stale release tree silently won over a fresh
    # signed build -- the gate then refused it with no hint why.
    local img="$out/app_m7/zephyr/zephyr.signed.bin"
    if [ -f "$img" ]; then
        echo "  programming: $out (built $(date -r "$img" '+%Y-%m-%d %H:%M'))"
        local other=""
        [ "$out" != "build/m7s" ] && other="build/m7s"
        [ "$out" != "build/release/m7s" ] && other="${other:-build/release/m7s}"
        local other_img="$other/app_m7/zephyr/zephyr.signed.bin"
        if [ -z "${HPI_SIGNED_OUT:-}" ] && [ -f "$other_img" ] && [ "$other_img" -nt "$img" ]; then
            echo "  ⚠️  $other is NEWER (built $(date -r "$other_img" '+%Y-%m-%d %H:%M'))."
            echo "     To program that one: HPI_SIGNED_OUT=$other scripts/flash.sh factory"
            echo "     (or run scripts/release.sh to refresh $out)."
        fi
    fi
    if [ -d "$out" ]; then
        HPI_SIGNED_OUT="$out" bash "$HPI_ROOT/tools/ci/check_prod_surface.sh" \
            --release "$out" || {
            # No escape hatch: --allow-test-vid existed only to rehearse before
            # the pid.codes allocation, which is now 1209/FF91.
            echo "❌ refusing to program: that build is not fit to ship." >&2
            exit 1
        }
    fi
    flash_m4
    HPI_SIGNED_OUT="$out" flash_signed
    cat <<'EOF'

Remaining line steps:
  1. ESP32-C6:  scripts/flash.sh esp32 --full [PORT]
                The M7 now holds the C6 in reset, so its USB port is absent until
                released: healthypi conn enable --radios none (dev: hpi link on).
                --full is the first-flash path onto the A/B layout; it erases the
                C6 and any Wi-Fi credentials stored on it.
  2. Option bytes: confirm BCM4=1 and BOOT_CM4_ADD0=0x0810
  3. EOL self-test over CDC1 (group-64 0x0080) and record the serial
EOF
}

esp32_modes() {
    cat >&2 <<'EOF'
   scripts/flash.sh esp32 --full [PORT]      first flash of a unit: erase the whole
                                             chip, write the merged A/B image.
                                             ERASES stored Wi-Fi credentials.
   scripts/flash.sh esp32 --app-only [PORT]  later re-flash: app only, keeps stored
                                             Wi-Fi; refuses a unit not yet on A/B.
EOF
}

flash_esp32() {
    # The mode is explicit, never defaulted: one choice erases the C6's stored
    # Wi-Fi credentials and the other cannot reach the A/B partition layout.
    local mode="" port="" a
    for a in "$@"; do
        case "$a" in
            --full|--app-only)
                if [ -n "$mode" ] && [ "$mode" != "$a" ]; then
                    echo "❌ --full and --app-only are exclusive — pick one:" >&2
                    esp32_modes; exit 1
                fi
                mode="$a" ;;
            -*) echo "❌ unknown esp32 option '$a'" >&2; esp32_modes; exit 1 ;;
            *)  if [ -n "$port" ]; then
                    echo "❌ more than one port given ('$port', '$a')" >&2; exit 1
                fi
                port="$a" ;;
        esac
    done
    if [ -z "$mode" ]; then
        echo "❌ say how to flash the ESP32-C6:" >&2
        esp32_modes
        echo "   A unit whose C6 was last flashed with the old single-factory table" >&2
        echo "   needs --full once; it cannot reach the A/B layout any other way." >&2
        exit 1
    fi

    local hb; hb="$(hpi_find_healthybridge)" || exit 1

    # ./hp6.sh flash does not erase, so it cannot move a unit onto the A/B
    # table; flash_hp6.sh can. An older flash_hp6.sh writes the app at the old
    # 0x10000 factory offset — on the A/B table that is otadata. Refuse both.
    if [ ! -f "$hb/flash_hp6.sh" ] || [ ! -f "$hb/partitions.hp6.csv" ] \
        || ! grep -q -- '--erase-all' "$hb/flash_hp6.sh" \
        || ! grep -q 'otadata' "$hb/flash_hp6.sh"; then
        echo "❌ $hb has no flash_hp6.sh with A/B partition support." >&2
        echo "   Check out a healthybridge-esp32 branch that has partitions.hp6.csv" >&2
        echo "   (hp6/c6-ota-receiver or later), then: scripts/build.sh esp32" >&2
        exit 1
    fi
    local fa="$hb/build.hp6/flasher_args.json"
    if [ ! -f "$fa" ]; then
        echo "❌ no HealthyPi 6 build at $hb/build.hp6 — run: scripts/build.sh esp32" >&2
        exit 1
    fi
    if ! grep -q '"otadata"' "$fa"; then
        echo "❌ $hb/build.hp6 was built with the old single-factory partition table." >&2
        echo "   Rebuild it: (cd $hb && ./hp6.sh clean) && scripts/build.sh esp32" >&2
        exit 1
    fi

    echo "ℹ️  The M7 holds the ESP32-C6 in reset from boot; its USB port only"
    echo "   enumerates after: healthypi conn enable --radios none"
    echo "   (or 'hpi link on' on the dev shell). Every board reset holds it again."
    if [ "$mode" = "--full" ]; then
        echo "⚠️  --full erases the whole C6, including stored Wi-Fi credentials and"
        echo "   settings — re-provision them afterwards."
    fi

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
            0) echo "❌ no ESP32-C6 found. Is it released from reset (above)? Or pass" >&2
               echo "   the port: scripts/flash.sh esp32 $mode /dev/cu.usbmodemXXXXX" >&2; exit 1 ;;
            *) echo "❌ multiple candidates (${found[*]}) — pass one explicitly" >&2; exit 1 ;;
        esac
    fi

    if [ "$mode" = "--app-only" ]; then
        ( cd "$hb" && ./flash_hp6.sh "$port" --app-only )
    else
        ( cd "$hb" && ./flash_hp6.sh "$port" )
    fi
    cat <<'EOF'
Verify on the C6's USB console (healthybridge-esp32: ./hp6.sh monitor PORT),
these lines in order:
  hb_main:  HealthyBridge <version> for HealthyPi 6 (esp32c6) starting
  hb_uart:  UART1 up @ 2000000 (TX.. RX.. RTS.. CTS..)
  hb_link:  HealthyBridge link up (transport=uart)
  ota_test: running ota_0 @ 0x20000: VALID — no rollback pending
A tag 'hb_spi' or 'hl_spi' means an obsolete SPI-link image is still on the
device.
EOF
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
