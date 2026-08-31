#!/usr/bin/env bash
# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT
# HealthyPi 6 — build a shippable release: production images + a .hpifw bundle.
#
#   scripts/release.sh                        dev key, for rehearsing the flow
#   HP6_SIGNING_KEY=/abs/release.pem scripts/release.sh
#   scripts/release.sh --allow-test-vid       bench-only escape (see below)
#
# Output: build/release/
#   m7s/                       the sysbuild tree (MCUboot + signed app)
#   hpi6-<version>.hpifw       the bundle a customer or Studio applies
#
# This is the ONLY supported way to produce firmware for a unit that leaves the
# building. It exists because "which build ships" was previously undefined: the
# documented default (scripts/build.sh m7) produces an image with no bootloader,
# no MCUmgr img group and no recovery entry, so a unit flashed with it could only
# ever be updated over SWD with the case open, and nothing said so.
#
# It therefore refuses to emit a bundle unless tools/ci/check_prod_surface.sh
# --release passes: MCUboot present and signing, serial recovery available,
# downgrade prevention on, no dev/debug surface, and a real USB VID.
set -euo pipefail

# shellcheck source=scripts/env.sh
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"
cd "$HPI_ROOT"

ALLOW_TEST_VID=0
SKIP_ESP32=0
for arg in "$@"; do
    case "$arg" in
        --allow-test-vid) ALLOW_TEST_VID=1 ;;
        --no-esp32)       SKIP_ESP32=1 ;;
        -h|--help)        sed -n '2,25p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option $arg" >&2; exit 1 ;;
    esac
done

OUT="build/release"
M7S="$OUT/m7s"
rm -rf "$OUT"
mkdir -p "$OUT"

version_of() {   # VERSION file -> "2.0.1-dev"
    local f="$1" maj min pat extra
    maj=$(sed -n 's/^VERSION_MAJOR *= *//p' "$f")
    min=$(sed -n 's/^VERSION_MINOR *= *//p' "$f")
    pat=$(sed -n 's/^PATCHLEVEL *= *//p' "$f")
    extra=$(sed -n 's/^EXTRAVERSION *= *//p' "$f")
    printf '%s.%s.%s%s' "$maj" "$min" "$pat" "${extra:+-$extra}"
}

M7_VER="$(version_of app_m7/VERSION)"
M4_VER="$(version_of app_m4/VERSION)"

echo "=== HealthyPi 6 release ==="
echo "  M7 $M7_VER · M4 $M4_VER"
echo "  key $HP6_SIGNING_KEY"
if [ "$HP6_SIGNING_KEY" = "$HPI_ROOT/keys/hp6_dev_ec256.pem" ]; then
    echo "  ⚠️  DEV KEY. Units signed with it accept updates signed only with the"
    echo "     same key. A real release passes HP6_SIGNING_KEY=/abs/path."
fi
echo ""

# --- 1. build ---------------------------------------------------------------
FLAVOR=prod HPI_SIGNED_OUT="$M7S" "$HPI_ROOT/scripts/build.sh" signed prod
"$HPI_ROOT/scripts/build.sh" m4

# The C6 image is OPTIONAL in a bundle: the C6 updates itself over WiFi and has
# no wired path through the M7, so a missing ESP-IDF must not take the whole
# release down with it. Warn and carry on.
if [ "$SKIP_ESP32" = 0 ]; then
    if hb="$(hpi_find_healthybridge 2>/dev/null)"; then
        if ( cd "$hb" && ./hp6.sh build ); then
            ESP_BIN="$hb/build.hp6/healthybridge.bin"
        else
            echo "⚠️  ESP32-C6 build failed (ESP-IDF not sourced?) — the bundle"
            echo "   will carry no C6 image. Source ESP-IDF's export.sh and re-run,"
            echo "   or pass --no-esp32 if that is intentional."
        fi
    else
        echo "ℹ️  HealthyBridge repo not found — the bundle will carry no C6 image."
        echo "   --no-esp32 silences this."
    fi
fi

# --- 2. gate ----------------------------------------------------------------
echo ""
echo "--- shippability check ---"
CHECK_ARGS=(--release "$M7S")
if ! bash tools/ci/check_prod_surface.sh "${CHECK_ARGS[@]}"; then
    if [ "$ALLOW_TEST_VID" = 1 ]; then
        # Narrow escape so the whole pipeline can be rehearsed before the
        # pid.codes allocation exists. It does NOT make the output shippable,
        # and the bundle is renamed so nobody can mistake it for one.
        echo ""
        echo "⚠️  --allow-test-vid: proceeding with a NON-SHIPPABLE build."
        SUFFIX="-TESTVID"
    else
        echo ""
        echo "❌ refusing to build a release bundle."
        echo "   Fix the violations above. If this is the USB VID and you are"
        echo "   only rehearsing the flow, re-run with --allow-test-vid."
        exit 1
    fi
fi

# --- 3. package -------------------------------------------------------------
echo ""
echo "--- bundle ---"
BUNDLE="$OUT/hpi6-${M7_VER}${SUFFIX:-}.hpifw"
CREATED="$(git log -1 --format=%cI 2>/dev/null || echo unknown)"

BUNDLE_ARGS=(
    "$BUNDLE"
    --m7 "build/release/m7s/app_m7/zephyr/zephyr.signed.bin" --m7-version "$M7_VER"
    --m4 "build/m4/zephyr/zephyr.bin"                        --m4-version "$M4_VER"
    --release "$M7_VER" --hw-rev v5
    --key "$HP6_SIGNING_KEY" --created "$CREATED"
)
if [ -n "${ESP_BIN:-}" ] && [ -f "${ESP_BIN:-}" ]; then
    BUNDLE_ARGS+=(--esp32c6 "$ESP_BIN")
fi
healthypi fw bundle create "${BUNDLE_ARGS[@]}"

# --- 4. verify what was just written ---------------------------------------
healthypi fw info --bundle "$BUNDLE" --pubkey "$HP6_SIGNING_KEY"

echo ""
echo "✅ release ready: $BUNDLE"
echo "   apply : healthypi fw update --port <CDC1> --bundle $BUNDLE"
echo "   factory programming: scripts/flash.sh factory"
