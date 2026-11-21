# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT
#
# Image configuration script for the M7 app when it is signed externally (see
# ../sysbuild.cmake). Runs after sysbuild's MAIN_image_default.cmake.
set_config_string(${ZCMAKE_APPLICATION} CONFIG_MCUBOOT_SIGNATURE_KEY_FILE "")
