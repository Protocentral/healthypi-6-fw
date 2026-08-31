#!/usr/bin/env bash
# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT
#
# check_prod_surface.sh — assert a build is fit to ship.
#
# Two things are checked, and they are different questions:
#
#   1. SURFACE (always): a production image must expose no dev/debug or remote
#      shell/filesystem management surface. Reads the merged Kconfig .config so
#      it verifies what actually built, not what a conf fragment asked for.
#
#   2. UPDATEABILITY (--release, or automatically when a bootloader image is
#      present): a shipped unit must be field-updateable. An image with no
#      MCUboot, no img group and no recovery path is permanently SWD-only —
#      the single most expensive mistake available here, and invisible until a
#      customer needs an update. Nothing in the tree used to check for it.
#
# Usage:
#   FLAVOR=prod scripts/build.sh m7 && tools/ci/check_prod_surface.sh build/m7
#   scripts/release.sh        && tools/ci/check_prod_surface.sh --release build/release/m7s
#
# Exits 1 on any violation; 2 if no merged .config is found.
set -euo pipefail

REQUIRE_RELEASE=0
BUILD=""

for arg in "$@"; do
  case "$arg" in
    --release) REQUIRE_RELEASE=1 ;;
    -h|--help)
      sed -n '3,20p' "$0"
      exit 0
      ;;
    *) BUILD="$arg" ;;
  esac
done
BUILD="${BUILD:-build/m7}"

# A plain build puts the app config at <build>/zephyr/.config; a sysbuild build
# nests it under the image name AND keeps a <build>/zephyr/.config of its own for
# sysbuild's Kconfig. The nested one is checked FIRST — the top-level file exists
# in both layouts but only holds application symbols in the plain one, so
# preferring it silently checks the wrong config for every signed build.
if   [ -f "$BUILD/app_m7/zephyr/.config" ]; then APP_CFG="$BUILD/app_m7/zephyr/.config"
elif [ -f "$BUILD/zephyr/.config" ];        then APP_CFG="$BUILD/zephyr/.config"
else
  echo "check_prod_surface: no merged .config under $BUILD" >&2
  echo "  build first, e.g. FLAVOR=prod scripts/build.sh m7   (or scripts/release.sh)" >&2
  exit 2
fi

BOOT_CFG=""
if [ -f "$BUILD/mcuboot/zephyr/.config" ]; then
  BOOT_CFG="$BUILD/mcuboot/zephyr/.config"
elif [ -f "$(dirname "$APP_CFG")/../../mcuboot/zephyr/.config" ]; then
  BOOT_CFG="$(dirname "$APP_CFG")/../../mcuboot/zephyr/.config"
fi

rc=0
note() { echo "  $*"; }
fail() { echo "VIOLATION: $*" >&2; rc=1; }

is_y()  { grep -q "^$1=y" "$2"; }
val_of() { sed -n "s/^$1=\(.*\)$/\1/p" "$2" | head -1; }

echo "check_prod_surface: app=$APP_CFG${BOOT_CFG:+ boot=$BOOT_CFG}"

# ---------------------------------------------------------------------------
# 1. Surface — these must NOT be =y in a shipped image (see app_m7/prj.prod.conf)
# ---------------------------------------------------------------------------
FORBIDDEN=(
  CONFIG_HPI_DEV_MODE
  CONFIG_SHELL
  CONFIG_SHELL_BACKEND_SERIAL
  CONFIG_MCUMGR_GRP_FS
  CONFIG_MCUMGR_GRP_SHELL
  CONFIG_LLEXT
  CONFIG_HPI_BUS_SELFTEST
  CONFIG_HPI_ACQ_DEBUG
  CONFIG_HPI_HEARTBEAT_LOG
  CONFIG_HPI_SPI4_LOOPBACK_TEST
  CONFIG_HPI_NPU_COMMS_CHECK
  CONFIG_HPI_CONN_DEBUG
  CONFIG_HEALTHYBRIDGE_ESP32_DEBUG
  CONFIG_THREAD_ANALYZER
)
for sym in "${FORBIDDEN[@]}"; do
  is_y "$sym" "$APP_CFG" && fail "${sym}=y (dev/debug surface in a prod image)"
done

# The unlock gate is deliberately OFF for v1 -- a recorded decision, not an
# oversight. What is NOT acceptable is having it on with the shared
# build-time secret — the firmware itself logs DO-NOT-SHIP for that combination,
# because every unit would then answer the same challenge.
if is_y CONFIG_HPI_SECURITY "$APP_CFG"; then
  if is_y CONFIG_HPI_UNLOCK_SECRET_SOURCE_KCONFIG "$APP_CFG"; then
    fail "CONFIG_HPI_SECURITY=y with the Kconfig (shared, build-time) unlock secret — every unit would share one secret"
  else
    note "security: unlock gate ENABLED with a non-Kconfig secret source"
  fi
else
  note "security: unlock gate disabled (recorded v1 posture — images are still signature-verified by MCUboot)"
fi

# ---------------------------------------------------------------------------
# 2. Updateability — a shipped unit must be able to receive a firmware update
# ---------------------------------------------------------------------------
if [ "$REQUIRE_RELEASE" = 1 ] || [ -n "$BOOT_CFG" ]; then
  echo "check_prod_surface: release checks enabled"

  if [ -z "$BOOT_CFG" ]; then
    fail "--release asked for, but no MCUboot image under $BUILD — this build cannot be updated in the field (build with scripts/release.sh)"
  fi

  # App side: bootloader-linked, stock img group for the M7, group-64 upload for
  # the M4, and a way back into the bootloader.
  for sym in CONFIG_BOOTLOADER_MCUBOOT CONFIG_MCUMGR_GRP_IMG CONFIG_IMG_MANAGER \
             CONFIG_HPI_M4_UPDATE CONFIG_HPI_RECOVERY_MODE CONFIG_RETENTION_BOOT_MODE; do
    is_y "$sym" "$APP_CFG" || fail "${sym} is not =y — the shipped app would not be field-updateable"
  done

  if [ -n "$BOOT_CFG" ]; then
    # Bootloader side: signed images, no downgrade, and serial recovery so a bad
    # image is not an RMA. Overwrite-only has no revert; recovery is the only
    # non-SWD way back.
    for sym in CONFIG_BOOT_SIGNATURE_TYPE_ECDSA_P256 CONFIG_MCUBOOT_DOWNGRADE_PREVENTION \
               CONFIG_MCUBOOT_SERIAL CONFIG_BOOT_SERIAL_CDC_ACM \
               CONFIG_BOOT_SERIAL_NO_APPLICATION CONFIG_BOOT_SERIAL_BOOT_MODE \
               CONFIG_BOOT_WATCHDOG_FEED; do
      is_y "$sym" "$BOOT_CFG" || fail "MCUboot: ${sym} is not =y"
    done
    is_y CONFIG_BOOT_SIGNATURE_TYPE_NONE "$BOOT_CFG" && fail "MCUboot: signature type NONE — images would not be verified"

    # BOOT_MAX_IMG_SECTORS_AUTO derived 8 on this board and bricked boot
    # Pin it or do not ship it.
    is_y CONFIG_BOOT_MAX_IMG_SECTORS_AUTO "$BOOT_CFG" && fail "MCUboot: BOOT_MAX_IMG_SECTORS_AUTO=y — it derives 8 on this board and produces an unbootable device"
  fi

  # USB identity. Assert the product VID rather than merely rejecting the
  # Zephyr development one: an unset or mistyped value would otherwise pass.
  #
  # Deliberately NOT asserted: any relationship between the application and
  # recovery PIDs. They happen to be equal today because pid.codes allocates one
  # PID per entry, but nothing depends on that. Both ports enumerate either way,
  # and a host tells them apart from the PROTOCOL -- the application answers
  # group 64, the bootloader does not -- which is what `healthypi fw recover`
  # probes. Pinning the two together here would buy nothing and would break a
  # future second allocation.
  app_vid="$(val_of CONFIG_HPI_USB_VID "$APP_CFG" | tr 'A-F' 'a-f')"
  app_pid="$(val_of CONFIG_HPI_USB_PID "$APP_CFG" | tr 'A-F' 'a-f')"
  if [ "$app_vid" = "0x2fe3" ]; then
    fail "CONFIG_HPI_USB_VID is still the 0x2FE3 Zephyr development VID"
  elif [ "$app_vid" != "0x1209" ]; then
    fail "CONFIG_HPI_USB_VID is '${app_vid:-<unset>}', expected 0x1209 (pid.codes)"
  elif [ "$app_pid" = "0x0100" ] || [ -z "$app_pid" ]; then
    fail "CONFIG_HPI_USB_PID is '${app_pid:-<unset>}' — that is the development default, not an allocated PID"
  else
    note "usb: app VID=$app_vid PID=$app_pid"
  fi
  if [ -n "$BOOT_CFG" ]; then
    boot_vid="$(val_of CONFIG_USB_DEVICE_VID "$BOOT_CFG" | tr 'A-F' 'a-f')"
    if [ "$boot_vid" = "0x2fe3" ]; then
      fail "MCUboot CONFIG_USB_DEVICE_VID is still 0x2FE3 -- the recovery port ships too"
    elif [ "$boot_vid" != "0x1209" ]; then
      fail "MCUboot CONFIG_USB_DEVICE_VID is '${boot_vid:-<unset>}', expected 0x1209"
    else
      note "usb: recovery VID=$boot_vid PID=$(val_of CONFIG_USB_DEVICE_PID "$BOOT_CFG")"
    fi
  fi
  # Visible on every release build until the allocation is confirmed. Not a
  # failure: adopting the applied-for PID was a deliberate call.
  if [ "$app_pid" = "0xff90" ]; then
    note "usb: PID 0xFF90 is PROVISIONAL — pid.codes allocation unconfirmed as of 2026-07-27"
  fi
fi

if [ "$rc" -eq 0 ]; then
  echo "check_prod_surface: OK"
fi
exit "$rc"
