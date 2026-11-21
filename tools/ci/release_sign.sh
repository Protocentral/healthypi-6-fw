#!/usr/bin/env bash
# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT
#
# Sign a staged release and build the .hpifw bundle.
#
#   tools/ci/release_sign.sh <staged-dir> <private-key.pem> <out-dir> [--own-key]
#
# <staged-dir> comes from tools/ci/release_stage.sh. This script never builds
# and needs only bash, python3, imgtool and this repo's healthypi package, so it
# runs on a machine (or CI job) that holds the key and nothing else.
#
# Refuses, before anything is signed, when:
#   - the staged files do not match their SHA256SUMS (altered in transit);
#   - the private key's public half is not the key the build embedded in
#     MCUboot and the M7 -- an image signed with any other key would be refused
#     by every unit built from this release;
#   - (without --own-key) that key is not one of the release public keys the
#     healthypi tools ship with, so users' `fw update` would refuse the bundle.
#
# Output: hpi6-firmware-<version>.zip (the bundle) plus the factory images
# (mcuboot.hex, app_m7.signed.hex, m4.hex) and SHA256SUMS.
#
# Used by .github/workflows/release.yml (sign job) and, for an offline
# signing session, by hand.
set -euo pipefail

IN="${1:?usage: release_sign.sh <staged-dir> <key.pem> <out-dir> [--own-key]}"
KEY="${2:?usage: release_sign.sh <staged-dir> <key.pem> <out-dir> [--own-key]}"
OUT="${3:?usage: release_sign.sh <staged-dir> <key.pem> <out-dir> [--own-key]}"
OWN_KEY=0
[ "${4:-}" = "--own-key" ] && OWN_KEY=1
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
REL_KEYS="$ROOT/tools/healthypi/src/healthypi/fw/release_keys"

die() { echo "❌ $*" >&2; exit 1; }
command -v imgtool >/dev/null || die "imgtool not found (pip install imgtool)"
command -v healthypi >/dev/null || die "healthypi not found (pip install ./tools/smpgroup ./tools/healthypi)"
[ -f "$KEY" ] || die "no key file $KEY"

# --- 1. the staged files are the ones that were built ----------------------
( cd "$IN" && shasum -a 256 -c --quiet SHA256SUMS ) || die "staged files do not match SHA256SUMS"
# shellcheck disable=SC1091
. "$IN/release.env"

# --- 2. the key is the one the build embedded (and a release key) ----------
pub_of() {   # <pem> -> DER public key, hex. Accepts a private or a public key.
    python3 - "$1" <<'PY'
import sys
from cryptography.hazmat.primitives import serialization
blob = open(sys.argv[1], "rb").read()
try:
    pub = serialization.load_pem_private_key(blob, password=None).public_key()
except (ValueError, TypeError):
    pub = serialization.load_pem_public_key(blob)
print(pub.public_bytes(serialization.Encoding.DER,
                       serialization.PublicFormat.SubjectPublicKeyInfo).hex())
PY
}
grep -q -- 'PRIVATE KEY-----' "$KEY" || die "$KEY is not a private key"
mine="$(pub_of "$KEY")"
[ "$mine" = "$(pub_of "$IN/m7/signing_pubkey.pem")" ] || die \
    "this key is not the one the build embedded in MCUboot ($IN/m7/signing_pubkey.pem). Units built from this release would refuse everything it signs."
if [ "$OWN_KEY" = 0 ]; then
    found=0
    for k in "$REL_KEYS"/*.pub.pem; do
        [ -f "$k" ] && [ "$(pub_of "$k")" = "$mine" ] && found=1
    done
    [ "$found" = 1 ] || die \
        "this key is not among the release public keys in $REL_KEYS, so users' healthypi fw update would refuse the bundle. (Your own re-keyed unit? Pass --own-key.)"
fi

# --- 3. sign the M7 exactly as MCUboot expects -----------------------------
rm -rf "$OUT"
mkdir -p "$OUT"
ARGS=()   # one argument per line; no mapfile, so it also runs on macOS bash 3.2
while IFS= read -r a; do [ -n "$a" ] && ARGS+=("$a"); done < "$IN/m7/imgtool_sign.args"
imgtool sign "${ARGS[@]}" --key "$KEY" "$IN/m7/app.bin" "$OUT/app_m7.signed.bin"
imgtool sign "${ARGS[@]}" --key "$KEY" "$IN/m7/app.hex" "$OUT/app_m7.signed.hex"
imgtool verify --key "$IN/m7/signing_pubkey.pem" "$OUT/app_m7.signed.bin" \
    || die "the signed M7 does not verify against the embedded key"
cp "$IN/m7/mcuboot.hex" "$OUT/mcuboot.hex"
cp "$IN/m4/m4.hex"      "$OUT/m4.hex"

# --- 4. the bundle ---------------------------------------------------------
BUNDLE="$OUT/hpi6-firmware-${M7_VERSION}.zip"
C6_BIN="$(ls "$IN"/esp32c6/*.bin)"
healthypi fw bundle create "$BUNDLE" \
    --m7 "$OUT/app_m7.signed.bin" --m7-version "$M7_VERSION" \
    --m4 "$IN/m4/m4.bin"          --m4-version "$M4_VERSION" \
    --esp32c6 "$C6_BIN" \
    --release "$M7_VERSION" --hw-rev v5 \
    --key "$KEY" --created "$CREATED"
healthypi fw info --bundle "$BUNDLE" --pubkey "$IN/m7/signing_pubkey.pem"
if [ "$OWN_KEY" = 0 ]; then
    # What every user's tool will do: verify against the shipped release keys.
    healthypi fw info --bundle "$BUNDLE" > /dev/null \
        || die "the bundle does not verify against the shipped release keys"
fi

cp "$IN/release.env" "$OUT/release.env"
( cd "$OUT" && find . -type f ! -name SHA256SUMS | sort | xargs shasum -a 256 > SHA256SUMS )
echo "✅ signed release in $OUT"
sed 's/^/   /' "$OUT/SHA256SUMS"
