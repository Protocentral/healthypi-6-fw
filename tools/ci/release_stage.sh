#!/usr/bin/env bash
# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT
#
# Collect an UNSIGNED release build into one directory for signing.
#
#   tools/ci/release_stage.sh <out-dir> <healthybridge-esp32 dir> [<C6 build dir name>]
#
# Run after:
#   HP6_SIGNING_KEY=<release PUBLIC key> scripts/build.sh signed prod
#   scripts/build.sh m4
#   (cd <healthybridge-esp32> && ./hp6.sh build)
#
# The output is everything tools/ci/release_sign.sh needs, and nothing else:
# the signer never builds and never needs a toolchain. Layout:
#
#   m7/mcuboot.hex  m7/mcuboot.bin        bootloader (embeds the public key)
#   m7/app.bin      m7/app.hex            M7 application, UNSIGNED
#   m7/imgtool_sign.args                  how MCUboot expects it to be signed
#   m7/signing_pubkey.pem                 the public key the build embedded
#   m4/m4.bin       m4/m4.hex             M4 image (signed in the bundle)
#   esp32c6/healthybridge_esp32c6.bin     C6 app image (signed in the bundle)
#   release.env                           versions and source revisions
#   SHA256SUMS                            of every file above
#
# Used by .github/workflows/release.yml (build job), and by hand for an
# offline signing session.
set -euo pipefail

OUT="${1:?usage: release_stage.sh <out-dir> <healthybridge-esp32 dir> [build dir]}"
HB="${2:?usage: release_stage.sh <out-dir> <healthybridge-esp32 dir> [build dir]}"
HB_BUILD="${3:-build.hp6}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
M7S="${HPI_SIGNED_OUT:-$ROOT/build/m7s}"
M4="$ROOT/build/m4"

die() { echo "❌ $*" >&2; exit 1; }

z="$M7S/app_m7/zephyr"
[ -f "$z/imgtool_sign.args" ] || die "$M7S is not an externally-signed build (no imgtool_sign.args). Build it with HP6_SIGNING_KEY=<public key> scripts/build.sh signed prod"
[ -f "$z/zephyr.bin" ] || die "no M7 image in $z"
[ ! -f "$z/zephyr.signed.bin" ] || die "$z holds a signed image: this build had a private key. Release builds must not."
key="$(sed -n 's/^CONFIG_BOOT_SIGNATURE_KEY_FILE="\(.*\)"$/\1/p' "$M7S/mcuboot/zephyr/.config")"
[ -f "$key" ] || die "cannot find the key MCUboot was built with ($key)"
grep -q -- '-----BEGIN PUBLIC KEY-----' "$key" || die "MCUboot was built with $key, which is not a public key"
[ -f "$M4/zephyr/zephyr.bin" ] || die "no M4 build in $M4 (scripts/build.sh m4)"
C6="$(python3 "$ROOT/tools/ci/c6_image.py" "$HB" "$HB_BUILD")" || die "no usable C6 image (above)"

version_of() {   # VERSION file -> "1.0.0"
    local f="$1" extra
    extra=$(sed -n 's/^EXTRAVERSION *= *//p' "$f")
    printf '%s.%s.%s%s' "$(sed -n 's/^VERSION_MAJOR *= *//p' "$f")" \
        "$(sed -n 's/^VERSION_MINOR *= *//p' "$f")" \
        "$(sed -n 's/^PATCHLEVEL *= *//p' "$f")" "${extra:+-$extra}"
}

rm -rf "$OUT"
mkdir -p "$OUT/m7" "$OUT/m4" "$OUT/esp32c6"
cp "$M7S/mcuboot/zephyr/zephyr.hex" "$OUT/m7/mcuboot.hex"
cp "$M7S/mcuboot/zephyr/zephyr.bin" "$OUT/m7/mcuboot.bin"
cp "$z/zephyr.bin"                  "$OUT/m7/app.bin"
cp "$z/zephyr.hex"                  "$OUT/m7/app.hex"
cp "$z/imgtool_sign.args"           "$OUT/m7/imgtool_sign.args"
cp "$key"                           "$OUT/m7/signing_pubkey.pem"
cp "$M4/zephyr/zephyr.bin"          "$OUT/m4/m4.bin"
cp "$M4/zephyr/zephyr.hex"          "$OUT/m4/m4.hex"
cp "$C6"                            "$OUT/esp32c6/$(basename "$C6")"

{
    echo "M7_VERSION=$(version_of "$ROOT/app_m7/VERSION")"
    echo "M4_VERSION=$(version_of "$ROOT/app_m4/VERSION")"
    echo "C6_VERSION=$(cat "$HB/version.txt")"
    echo "FW_REV=$(git -C "$ROOT" rev-parse HEAD)"
    echo "FW_DESCRIBE=$(git -C "$ROOT" describe --always --dirty --tags 2>/dev/null || echo unknown)"
    echo "C6_REV=$(git -C "$HB" rev-parse HEAD 2>/dev/null || echo unknown)"
    echo "C6_DESCRIBE=$(git -C "$HB" describe --always --dirty --tags 2>/dev/null || echo unknown)"
    echo "CREATED=$(git -C "$ROOT" log -1 --format=%cI)"
} > "$OUT/release.env"

( cd "$OUT" && find . -type f ! -name SHA256SUMS | sort | xargs shasum -a 256 > SHA256SUMS )
echo "✅ staged unsigned release in $OUT"
cat "$OUT/release.env" | sed 's/^/   /'
