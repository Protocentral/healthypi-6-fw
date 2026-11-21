# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT
#
# Sysbuild hook for the M7 application (included by sysbuild after the images
# are added, before they are configured).
#
# HP6_SIGN_EXTERNALLY=ON (set by `scripts/build.sh signed` when HP6_SIGNING_KEY
# is a PUBLIC key): build MCUboot with that public key, but leave the M7 image
# unsigned so it can be signed where the private key lives
# (tools/ci/release_sign.sh, .github/workflows/release.yml).
#
# Sysbuild copies SB_CONFIG_BOOT_SIGNATURE_KEY_FILE into the app's
# CONFIG_MCUBOOT_SIGNATURE_KEY_FILE from its own image-default script, which
# runs after anything passed on the command line -- so `-Dapp_m7_CONFIG_...`
# cannot override it. Appending our own script to the image's
# IMAGE_CONF_SCRIPT list runs it after those defaults, and an empty key file is
# Zephyr's documented "sign externally" configuration (cmake/mcuboot.cmake).
if(HP6_SIGN_EXTERNALLY)
  set_property(TARGET ${DEFAULT_IMAGE} APPEND PROPERTY IMAGE_CONF_SCRIPT
               ${CMAKE_CURRENT_LIST_DIR}/sysbuild/unsigned_app.cmake)
endif()
