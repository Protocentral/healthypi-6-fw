#!/usr/bin/env bash
# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT
# HealthyPi 6 — build a shippable release: production images + a firmware bundle (zip).
#
#   scripts/release.sh                        dev key, for rehearsing the flow
#   HP6_SIGNING_KEY=/abs/release.pem scripts/release.sh
#   HP6_SIGNING_KEY=/abs/my_key.pem scripts/release.sh --own-key   (a re-keyed unit)
#   scripts/release.sh --no-esp32             a bundle that CANNOT update the C6
#
# Output: build/release/
#   m7s/                       the sysbuild tree (MCUboot + signed app)
#   hpi6-firmware-<version>.zip  the bundle a customer or Studio applies
#
# The ESP32-C6 app image is REQUIRED and is not built here: it comes from the
# healthybridge-esp32 repo (beside this one, or $HEALTHYBRIDGE_DIR), built
# there with `./hp6.sh build`, which leaves build.hp6/healthybridge_esp32c6.bin.
# It is checked (target, HP6 profile, OTA receiver, not stale) and signed into
# the bundle with the same key as the M4 image.
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
        -h|--help)        awk 'NR>1 && /^#/ {sub(/^# ?/, ""); print; next} NR>1 {exit}' "${BASH_SOURCE[0]}"; exit 0 ;;
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

# --- 0. the ESP32-C6 image (checked first: it fails in a second, the builds
#        below take minutes) ---------------------------------------------------
#
# Required since the C6 is updated through the M7 (group-64 espfw_*): a bundle
# without it leaves every unit's C6 on whatever it shipped with. Not built
# here -- the C6 is its own repo with its own toolchain (ESP-IDF), and a release
# must not depend on which ESP-IDF this shell happens to have sourced -- but
# checked hard, because a wrong image is refused only on the device, after the
# transfer, or worse is accepted by the M7 and then refused by the C6.
#
# Everything is read from ESP-IDF's own build metadata rather than guessed: the
# .bin name is per-target, and a guessed "healthybridge.bin" once left the image
# silently out of every bundle.
c6_image() {   # <healthybridge dir> -> path of the HP6 (esp32c6) app image
    python3 - "$1" <<'PY'
import json, pathlib, sys
repo = pathlib.Path(sys.argv[1])
fix = f"Build it there:  (cd {repo} && ./hp6.sh build)"
desc = repo / "build.hp6" / "project_description.json"
if not desc.is_file():
    sys.exit(f"no HealthyPi 6 build of the C6 firmware in {repo}/build.hp6. {fix}")
d = json.loads(desc.read_text())
if d.get("target") != "esp32c6":
    sys.exit(f"{repo}/build.hp6 was built for {d.get('target')!r}, not esp32c6 -- "
             f"wrong target. Rebuild:  (cd {repo} && ./hp6.sh clean && ./hp6.sh build)")
cfg = pathlib.Path(d["build_dir"]) / "config" / "sdkconfig.json"
k = json.loads(cfg.read_text()) if cfg.is_file() else {}
if not k.get("HB_PROFILE_HP6"):
    sys.exit(f"{repo}/build.hp6 is not the HealthyPi 6 profile (CONFIG_HB_PROFILE_HP6 "
             f"unset in {cfg}) -- wrong profile. Rebuild:  (cd {repo} && ./hp6.sh "
             "clean && ./hp6.sh build)")
if not k.get("HB_OTA_RECEIVER"):
    sys.exit(f"{repo}/build.hp6 has no firmware-update receiver (CONFIG_HB_OTA_RECEIVER "
             "unset): a unit given this image could never be updated over USB again. "
             "Use a healthybridge-esp32 with the HP6 OTA receiver.")
img = pathlib.Path(d["build_dir"]) / d["app_bin"]
if not img.is_file():
    sys.exit(f"{img} is missing. {fix}")
want = (repo / "version.txt").read_text().strip() if (repo / "version.txt").is_file() else ""
built = d.get("project_version", "")
if want and built != want:
    sys.exit(f"{img} is stale: built as {built}, but {repo}/version.txt says {want}. {fix}")
print(img)
PY
}
ESP_BIN=""
if [ "$SKIP_ESP32" = 1 ]; then
    echo ""
    echo "  ⚠️  --no-esp32: this bundle will carry NO ESP32-C6 image. It cannot"
    echo "     update the C6 -- every unit keeps the C6 firmware it already has."
else
    if ! hb="$(hpi_find_healthybridge)"; then
        echo ""
        echo "❌ the release needs the ESP32-C6 app image (above: where to get the repo)."
        echo "   Or, deliberately without it: scripts/release.sh --no-esp32"
        exit 1
    fi
    if ! ESP_BIN="$(c6_image "$hb")"; then
        echo ""
        echo "❌ no usable ESP32-C6 app image (above). Or, deliberately without it:"
        echo "   scripts/release.sh --no-esp32"
        exit 1
    fi
    ESP_REV="$(git -C "$hb" describe --always --dirty 2>/dev/null || echo unknown)"
    echo "  C6 $ESP_BIN"
    echo "     healthybridge-esp32 @ $ESP_REV"
    case "$ESP_REV" in
        *-dirty) echo "  ⚠️  healthybridge-esp32 has uncommitted changes: this C6 image is"
                 echo "     not reproducible from any commit." ;;
    esac
fi
echo ""

# --- 1. build ---------------------------------------------------------------
FLAVOR=prod HPI_SIGNED_OUT="$M7S" "$HPI_ROOT/scripts/build.sh" signed prod
"$HPI_ROOT/scripts/build.sh" m4

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
if [ -n "$ESP_BIN" ]; then
    # The version comes from the image's own app descriptor, which is what the
    # C6 reports once it runs it; bundle create also re-checks chip and project.
    BUNDLE_ARGS+=(--esp32c6 "$ESP_BIN")
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
if [ "$SKIP_ESP32" = 1 ]; then
    echo "⚠️  built with --no-esp32: this bundle CANNOT update the ESP32-C6."
fi
echo "✅ release ready: $BUNDLE"
echo "   apply : healthypi fw update --port <CDC1> --bundle $BUNDLE"
echo "   factory programming: scripts/flash.sh factory"
