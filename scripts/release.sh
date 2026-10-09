#!/usr/bin/env bash
# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT
# HealthyPi 6 — build a shippable release: production images + a firmware bundle (zip).
#
#   scripts/release.sh                        dev key, for rehearsing the flow
#   HP6_SIGNING_KEY=/abs/release.pem scripts/release.sh
#   HP6_SIGNING_KEY=/abs/my_key.pem scripts/release.sh --own-key   (a re-keyed unit)
#
# Output: build/release/
#   m7s/                       the sysbuild tree (MCUboot + signed app)
#   hpi6-firmware-<version>.zip  the bundle a customer or Studio applies
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

SKIP_ESP32=0
OWN_KEY=0
for arg in "$@"; do
    case "$arg" in
        --no-esp32)       SKIP_ESP32=1 ;;
        --own-key)        OWN_KEY=1 ;;
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

# The C6 image is OPTIONAL in a bundle. Nothing applies it yet -- `fw update`
# skips the C6, and the C6 has no OTA slots until its A/B partition table lands
# -- so a missing ESP-IDF must not take the whole release down. Carry it when it
# builds, and say loudly when it does not.
#
# The image's file name is read from ESP-IDF's own build metadata rather than
# guessed: the project name (and so the .bin name) is per-target, and a guessed
# "healthybridge.bin" never existed, so the image was silently left out of
# every bundle.
c6_image() {   # <healthybridge dir> -> path of the HP6 (esp32c6) app image
    python3 - "$1/build.hp6/project_description.json" <<'PY'
import json, pathlib, sys
desc = pathlib.Path(sys.argv[1])
d = json.loads(desc.read_text())
if d.get("target") != "esp32c6":
    sys.exit(f"build.hp6 was built for {d.get('target')!r}, not esp32c6")
img = pathlib.Path(d["build_dir"]) / d["app_bin"]
if not img.is_file():
    sys.exit(f"{img} is missing")
print(img)
PY
}
if [ "$SKIP_ESP32" = 0 ]; then
    if hb="$(hpi_find_healthybridge 2>/dev/null)"; then
        if ( cd "$hb" && ./hp6.sh build ); then
            if ! ESP_BIN="$(c6_image "$hb")"; then
                ESP_BIN=""
                echo "⚠️  ESP32-C6 built, but its image could not be located (above)."
            fi
        else
            echo "⚠️  ESP32-C6 build failed (ESP-IDF not sourced?)."
        fi
    else
        echo "ℹ️  HealthyBridge repo not found."
    fi
fi

# --- 2. gate ----------------------------------------------------------------
echo ""
echo "--- shippability check ---"
CHECK_ARGS=(--release "$M7S")
# No escape hatch. --allow-test-vid existed to rehearse the pipeline before the
# pid.codes allocation existed; it is allocated (1209/FF91), and the flag
# bypassed *any* gate failure, not only the VID.
if ! bash tools/ci/check_prod_surface.sh "${CHECK_ARGS[@]}"; then
    echo ""
    echo "❌ refusing to build a release bundle. Fix the violations above."
    exit 1
fi

# --- 3. package -------------------------------------------------------------
echo ""
echo "--- bundle ---"
BUNDLE="$OUT/hpi6-firmware-${M7_VER}.zip"
CREATED="$(git log -1 --format=%cI 2>/dev/null || echo unknown)"

BUNDLE_ARGS=(
    "$BUNDLE"
    --m7 "build/release/m7s/app_m7/zephyr/zephyr.signed.bin" --m7-version "$M7_VER"
    --m4 "build/m4/zephyr/zephyr.bin"                        --m4-version "$M4_VER"
    --release "$M7_VER" --hw-rev v5
    --key "$HP6_SIGNING_KEY" --created "$CREATED"
)
if [ -n "${ESP_BIN:-}" ]; then
    echo "  esp32c6: $ESP_BIN"
    BUNDLE_ARGS+=(--esp32c6 "$ESP_BIN")
elif [ "$SKIP_ESP32" = 0 ]; then
    echo "⚠️  This bundle carries NO ESP32-C6 image. Source ESP-IDF's export.sh"
    echo "   and re-run, or pass --no-esp32 if that is intentional."
fi
healthypi fw bundle create "${BUNDLE_ARGS[@]}"

# --- 4. verify what was just written ---------------------------------------
healthypi fw info --bundle "$BUNDLE" --pubkey "$HP6_SIGNING_KEY"
# An OFFICIAL release must also verify against the release public key(s) the
# published tools ship with -- otherwise every user's `healthypi fw update`
# refuses it. Not for the dev key (the tools never trust it), and not with
# --own-key: an owner who re-keyed their unit signs with a key the tools do not
# ship, and applies with --pubkey (.github/SECURITY.md, "Your device, your key").
REL_KEYS="$HPI_ROOT/tools/healthypi/src/healthypi/fw/release_keys"
if [ "$OWN_KEY" = 1 ]; then
    echo "  --own-key: signed with your key; apply with --pubkey $HP6_SIGNING_KEY"
elif [ "$HP6_SIGNING_KEY" != "$HPI_ROOT/keys/hp6_dev_ec256.pem" ]; then
    if ! ls "$REL_KEYS"/*.pub.pem > /dev/null 2>&1; then
        echo "⚠️  no release public key in $REL_KEYS -- the published tools cannot"
        echo "   verify this release. Add it (keys/README.md) before publishing."
    elif ! healthypi fw info --bundle "$BUNDLE" > /dev/null; then
        echo "❌ this bundle does not verify against the release public key(s) in"
        echo "   $REL_KEYS -- signed with the wrong key?"
        echo "   (Signing for your own re-keyed unit? Re-run with --own-key.)"
        exit 1
    else
        echo "  verifies against the release key(s) shipped with the healthypi tools"
    fi
fi

echo ""
echo "✅ release ready: $BUNDLE"
echo "   apply : healthypi fw update --port <CDC1> --bundle $BUNDLE"
echo "   factory programming: scripts/flash.sh factory"
